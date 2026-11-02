#!/bin/bash
# D20 第 1 步: 编译并运行 3 线程真 NPU 并行
set -u
B=/root/edgevision/deploy/cpp/3rdparty/rknn_api
cd /root/lab || exit 1

sed -i 's/\r$//' d20_step1_npu_threads.cpp

echo "=== 编译 ==="
g++ -O2 -std=c++17 d20_step1_npu_threads.cpp \
    -I"$B/include" -L"$B/lib" -lrknnrt -pthread -o d20_step1 || exit 2
echo "BUILD_OK"
echo

echo "=== 运行 ==="
export LD_LIBRARY_PATH="$B/lib:${LD_LIBRARY_PATH:-}"
timeout 120 ./d20_step1
echo "退出码 = $?"
