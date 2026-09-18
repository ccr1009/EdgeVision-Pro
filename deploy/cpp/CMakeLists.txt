cmake_minimum_required(VERSION 3.10)
project(edgevision_rk3588_cpp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CROSS_COMPILE_RK3588 "Cross compile for RK3588 aarch64" OFF)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(SOURCES
    src/main.cpp
    src/postprocess.cpp
    src/rknn_engine.cpp
)

if(CROSS_COMPILE_RK3588)
    message(STATUS "=== Configuring for RK3588 aarch64 (Real NPU) ===")
    add_definitions(-DENABLE_RKNN)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknn_api/include)
    link_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/rknn_api/lib)

    add_executable(edgevision_rknn ${SOURCES})
    target_link_libraries(edgevision_rknn rknnrt pthread)
else()
    message(STATUS "=== Configuring for x86_64 Native Simulation Host ===")
    add_executable(edgevision_x86 ${SOURCES})
    target_link_libraries(edgevision_x86 pthread)
endif()
