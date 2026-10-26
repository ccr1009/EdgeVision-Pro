cmake_minimum_required(VERSION 3.10)
project(edgevision_rk3588_cpp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===========================================================================
#  构建类型 / 优化级别 (task-7: 消除"无优化编译")
# ---------------------------------------------------------------------------
#  历史坑: 板端一直是 `cmake .. -DCROSS_COMPILE_RK3588=ON` 裸调,
#         CMAKE_BUILD_TYPE 为空串 → CMake 不附加任何优化标志,
#         flags.make 里 CXX_FLAGS 只有 `-std=gnu++17`(等效 -O0)。
#         因此 CSC 51~65ms / 全链路 67~91ms 这些 CPU 侧数字都是未优化代码跑出来的。
#  解法:   未指定就默认 Release; Release 固定 `-O3 -DNDEBUG`。
#         仍可 `-DCMAKE_BUILD_TYPE=Debug` 显式覆盖(诊断/单步时用)。
# ===========================================================================
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release"
        CACHE STRING "Build type: Release / Debug / RelWithDebInfo" FORCE)
    message(STATUS "未指定 CMAKE_BUILD_TYPE -> 默认 Release (-O3 -DNDEBUG)")
endif()
# 只兜底填空, 不 FORCE —— 保留 `-DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"` 之类
# 的命令行覆盖能力(O2/O3 对照实验要用)。
if(NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "C++ Release flags" FORCE)
endif()
if(NOT CMAKE_C_FLAGS_RELEASE)
    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "C Release flags" FORCE)
endif()
message(STATUS "CMAKE_BUILD_TYPE = '${CMAKE_BUILD_TYPE}'")

option(CROSS_COMPILE_RK3588 "Cross compile for RK3588 aarch64" OFF)
option(BUILD_BENCH "Build bench tools (bench_dflfree) with the same flags" OFF)

# ===========================================================================
#  CPU 架构优化: RK3588 = 4x Cortex-A76(大核) + 4x Cortex-A55(小核)
# ---------------------------------------------------------------------------
#  -mcpu=cortex-a76.cortex-a55 让 GCC 生成"大小核都合法"的指令序列(不会用
#  A76 独有的、A55 上会 SIGILL 的指令), 并按 A76 排布调度。
#  只在 aarch64 目标上追加; x86 仿真构建不受影响。
#  工具链不支持时自动降级 -mtune=cortex-a76, 再不行就什么都不加。
#  也可手工覆盖: -DEVISION_ARCH_FLAGS="-mtune=cortex-a76"
#  设 -DEVISION_ARCH_FLAGS=none 可完全关闭(做纯 -O 级别对照实验时用)。
# ===========================================================================
include(CheckCXXCompilerFlag)
set(EVISION_ARCH_FLAGS "" CACHE STRING "Override CPU arch flags (empty = auto, none = disabled)")
if(EVISION_ARCH_FLAGS STREQUAL "none")
    message(STATUS "CPU arch flags disabled by -DEVISION_ARCH_FLAGS=none")
    set(EVISION_ARCH_FLAGS "")
elseif(NOT EVISION_ARCH_FLAGS)
    if(CROSS_COMPILE_RK3588 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        check_cxx_compiler_flag("-mcpu=cortex-a76.cortex-a55" EVISION_HAVE_MCPU_A76_A55)
        if(EVISION_HAVE_MCPU_A76_A55)
            set(EVISION_ARCH_FLAGS "-mcpu=cortex-a76.cortex-a55" CACHE STRING "" FORCE)
        else()
            check_cxx_compiler_flag("-mtune=cortex-a76" EVISION_HAVE_MTUNE_A76)
            if(EVISION_HAVE_MTUNE_A76)
                set(EVISION_ARCH_FLAGS "-mtune=cortex-a76" CACHE STRING "" FORCE)
                message(STATUS "-mcpu=cortex-a76.cortex-a55 不支持 -> 降级 -mtune=cortex-a76")
            else()
                message(WARNING "编译器不支持 -mcpu=cortex-a76.cortex-a55 / -mtune=cortex-a76; "
                                "按通用 aarch64 编译(仍保留 -O3)")
            endif()
        endif()
    endif()
endif()
if(EVISION_ARCH_FLAGS)
    add_compile_options(${EVISION_ARCH_FLAGS})
endif()
message(STATUS "CPU arch flags = '${EVISION_ARCH_FLAGS}'")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(COMMON_SOURCES
    src/postprocess.cpp
    src/postprocess_dfl_free.cpp
    src/rknn_engine.cpp
    src/rknn_engine_dfl_free.cpp
)

# ---------------------------------------------------------------------------
#  MPP 硬件 JPEG 解码器 (仅 RK3588 aarch64 目标)
#  依赖: third_party/mpp/{include,lib} —— 必须用与源码同源编译的库,
#        系统自带 v1.5.0 与本板驱动不匹配会返回 -1012(MPP_ERR_BUFFER_FULL)。
# ---------------------------------------------------------------------------
if(CROSS_COMPILE_RK3588)
    set(MPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/mpp")
    if(EXISTS "${MPP_ROOT}/lib/librockchip_mpp.so.0")
        message(STATUS "MPP found: ${MPP_ROOT}")
        list(APPEND COMMON_SOURCES src/mpp_jpeg_decoder.cpp)
        include_directories(${MPP_ROOT}/include)
        link_directories(${MPP_ROOT}/lib)
        set(MPP_LIBS rockchip_mpp)

        # -------------------------------------------------------------------
        #  [集成点] MPP 硬件 JPEG 编码器 (task-8, 由另一位同学实现)
        #  文件存在才编, 不存在时什么都不做 —— 保证本仓库随时可构建。
        #  定义 HAVE_MPP_JPEG_ENCODER 供调用方 #ifdef 判断。
        # -------------------------------------------------------------------
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/mpp_jpeg_encoder.cpp")
            message(STATUS "MPP JPEG encoder found -> enabled")
            list(APPEND COMMON_SOURCES src/mpp_jpeg_encoder.cpp)
            add_definitions(-DHAVE_MPP_JPEG_ENCODER)
        else()
            message(STATUS "MPP JPEG encoder not present -> skipped (集成点已预留)")
        endif()
    else()
        message(WARNING "MPP not found at ${MPP_ROOT}; mpp_jpeg_decoder disabled")
        set(MPP_LIBS "")
    endif()

    # OpenCV (板上 4.5.1, edgevision_pro 的 letterbox 用)
    find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs)
    if(OpenCV_FOUND)
        message(STATUS "OpenCV found: ${OpenCV_VERSION}")
        add_definitions(-DHAVE_OPENCV)
    else()
        message(WARNING "OpenCV not found; edgevision_pro will be disabled")
    endif()
else()
    set(MPP_LIBS "")
endif()

if(CROSS_COMPILE_RK3588)
    message(STATUS "=== Configuring for RK3588 aarch64 (Real NPU) ===")
    add_definitions(-DENABLE_RKNN)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknn_api/include)
    link_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknn_api/lib)

    add_executable(edgevision_rknn src/main.cpp ${COMMON_SOURCES})
    target_link_libraries(edgevision_rknn rknnrt pthread ${MPP_LIBS})

    # edgevision_pro 依赖 OpenCV (letterbox), 没有 OpenCV 就整个跳过
    if(OpenCV_FOUND)
        add_executable(edgevision_pro_rknn src/edgevision_pro_main.cpp ${COMMON_SOURCES})
        target_link_libraries(edgevision_pro_rknn rknnrt pthread ${MPP_LIBS}
                              ${OpenCV_LIBS})
        target_include_directories(edgevision_pro_rknn PRIVATE ${OpenCV_INCLUDE_DIRS})
    else()
        message(WARNING "edgevision_pro_rknn skipped (needs OpenCV)")
    endif()

    # 让产物自带 rpath, 免去 LD_LIBRARY_PATH
    if(MPP_LIBS)
        set_target_properties(edgevision_rknn
            PROPERTIES INSTALL_RPATH "${MPP_ROOT}/lib"
                       BUILD_RPATH "${MPP_ROOT}/lib")
        if(TARGET edgevision_pro_rknn)
            set_target_properties(edgevision_pro_rknn
                PROPERTIES INSTALL_RPATH "${MPP_ROOT}/lib"
                           BUILD_RPATH "${MPP_ROOT}/lib")
        endif()
    endif()
else()
    message(STATUS "=== Configuring for x86_64 Native Simulation Host ===")
    add_executable(edgevision_x86 src/main.cpp ${COMMON_SOURCES})
    target_link_libraries(edgevision_x86 pthread)

    # edgevision_pro 依赖 OpenCV (letterbox), 没有 OpenCV 就整个跳过
    find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs)
    if(OpenCV_FOUND)
        add_executable(edgevision_pro src/edgevision_pro_main.cpp ${COMMON_SOURCES})
        target_link_libraries(edgevision_pro pthread ${OpenCV_LIBS})
        target_include_directories(edgevision_pro PRIVATE ${OpenCV_INCLUDE_DIRS})
    else()
        message(WARNING "edgevision_pro skipped (needs OpenCV)")
    endif()
endif()

# ===========================================================================
#  Bench 工具 (task-7: 统一优化口径)
# ---------------------------------------------------------------------------
#  历史问题: bench/ 下的探针都是手工 `g++ -O2 ...` 编的, 与 CMake target
#            (-O0) 口径不一致 → 数字不能横向比较。
#  BUILD_BENCH=ON 时用**同一套编译标志**产出 bench_dflfree, 保证 A/B 公平。
#  默认 OFF, 不改变原有构建行为。
# ===========================================================================
if(BUILD_BENCH AND CROSS_COMPILE_RK3588)
    add_executable(bench_dflfree
        bench/bench_dflfree.cpp
        src/postprocess_dfl_free.cpp)
    target_link_libraries(bench_dflfree rknnrt pthread)
    target_include_directories(bench_dflfree PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknn_api/include)
    if(MPP_LIBS)
        set_target_properties(bench_dflfree
            PROPERTIES INSTALL_RPATH "${MPP_ROOT}/lib"
                       BUILD_RPATH "${MPP_ROOT}/lib")
    endif()
endif()

# ===========================================================================
#  构建配置摘要 (一眼确认优化是否真的生效)
# ===========================================================================
message(STATUS "----------------------------------------------------------")
message(STATUS " build type      : '${CMAKE_BUILD_TYPE}'")
# 坑(2026-10-25 实测): CMake 的变量名区分大小写!
#   CMAKE_BUILD_TYPE 的值是 'Release'(大小写混合), 但标志变量是全大写
#   `CMAKE_CXX_FLAGS_RELEASE`。直接拼 "CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}"
#   会得到 `CMAKE_CXX_FLAGS_Release` -> DEFINED 为假 -> 摘要恒为空并误报
#   "flags 里没有 -O"(实际 flags.make 里 -O3 是好的)。
#   必须 string(TOUPPER ...) 转大写后再拼。
string(TOUPPER "${CMAKE_BUILD_TYPE}" _EV_TYPE_UPPER)
set(_EV_TYPE_FLAGS_VAR "CMAKE_CXX_FLAGS_${_EV_TYPE_UPPER}")
if(CMAKE_BUILD_TYPE AND DEFINED ${_EV_TYPE_FLAGS_VAR})
    set(_EV_TYPE_FLAGS "${${_EV_TYPE_FLAGS_VAR}}")
else()
    set(_EV_TYPE_FLAGS "")
endif()
message(STATUS " CXX flags       : '${CMAKE_CXX_FLAGS} ${_EV_TYPE_FLAGS}' (已生效: ${EVISION_ARCH_FLAGS})")
if(NOT CMAKE_BUILD_TYPE)
    message(WARNING "CMAKE_BUILD_TYPE 为空 -> 编译器不加任何优化标志(等效 -O0)!")
elseif(_EV_TYPE_FLAGS MATCHES "-O[0-9s]")
    message(STATUS " 优化标志检查    : OK (命中 '${_EV_TYPE_FLAGS}' 里的 -O)")
else()
    message(WARNING "构建类型 '${CMAKE_BUILD_TYPE}' 的 flags 里没有 -O: '${_EV_TYPE_FLAGS}'")
endif()
message(STATUS " arch flags      : ${EVISION_ARCH_FLAGS}")
message(STATUS " BUILD_BENCH     : ${BUILD_BENCH}")
message(STATUS "----------------------------------------------------------")
