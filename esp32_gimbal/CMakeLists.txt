cmake_minimum_required(VERSION 3.16)

# ESP32-S3 N16R8 舵机云台执行器
# 目标芯片 esp32s3（16MB Flash + 8MB PSRAM）
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_gimbal)
