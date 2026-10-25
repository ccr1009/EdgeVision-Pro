# ============================================================================
#  stereo3d-rk3588 —— 双目 3D / 人脸 3D 定位 (RK3588 原生编译)
#
#  板端构建（板子上原生编译，不要交叉编译）：
#      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
#  产物：build/stereo3d_demo
# ============================================================================
cmake_minimum_required(VERSION 3.10)
project(stereo3d_rk3588 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# ---- OpenCV ----
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio calib3d objdetect highgui)
message(STATUS "OpenCV ${OpenCV_VERSION} @ ${OpenCV_DIR}")

# 项目自己的头文件目录（必须加，否则 src/*.cpp 里 #include "stereo_types.h" 找不到）
set(PROJ_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${PROJ_INCLUDE} ${OpenCV_INCLUDE_DIRS})

# ---- 可选：opencv_contrib 的 ximgproc（WLS 视差滤波）----
find_package(OpenCV QUIET COMPONENTS ximgproc)
if(OpenCV_ximgproc_FOUND OR ximgproc_FOUND)
  add_compile_definitions(HAVE_XIMGPROC=1)
  message(STATUS "ximgproc: ON  (WLS 视差滤波可用)")
else()
  message(STATUS "ximgproc: OFF (没有 opencv_contrib，WLS 自动降级)")
endif()

# ---- 核心库（demo 和自检工具共用，避免重复链接源码）----
add_library(stereo3d_core STATIC
  src/stereo_pipeline.cpp
  src/face_3d.cpp
  src/http_server.cpp
)
target_include_directories(stereo3d_core PUBLIC ${PROJ_INCLUDE} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(stereo3d_core PUBLIC ${OpenCV_LIBS} pthread)

# ---- 主程序 ----
add_executable(stereo3d_demo src/main.cpp)
target_link_libraries(stereo3d_demo PRIVATE stereo3d_core)

# ---- 探针工具（可选，不影响主程序）----
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_dual_cam.cpp)
  add_executable(probe_dual_cam tools/probe_dual_cam.cpp)
  target_link_libraries(probe_dual_cam PRIVATE ${OpenCV_LIBS})
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_synthetic_stereo.cpp)
  add_executable(gen_synthetic_stereo tools/gen_synthetic_stereo.cpp)
  target_link_libraries(gen_synthetic_stereo PRIVATE stereo3d_core)
endif()

# ---- task-5 消融工具：SGBM 参数扫描实测 ----
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/ablation_sgbm.cpp)
  add_executable(ablation_sgbm tools/ablation_sgbm.cpp)
  target_link_libraries(ablation_sgbm PRIVATE stereo3d_core)
endif()

# ---- 安装到板端运行目录 ----
install(TARGETS stereo3d_demo RUNTIME DESTINATION .)
