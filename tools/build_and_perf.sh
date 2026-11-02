#!/bin/bash
# 编译 + 逐层剖析。全部走脚本文件, 避免 Windows ssh 的引号问题。
set -u
B=/root/edgevision/deploy/cpp/3rdparty/rknn_api
MODEL=/root/edgevision/data/models/yolov8n_rk3588_i8.rknn
cd /root/lab || exit 1

g++ -O2 -std=c++17 bench_npu.cpp -I"$B/include" -L"$B/lib" -lrknnrt -o bench_npu || exit 2
echo "BUILD_OK"

export LD_LIBRARY_PATH="$B/lib:${LD_LIBRARY_PATH:-}"
./bench_npu "$MODEL" 1 --perf > /tmp/perf.log 2>&1
echo "--- 前 8 行 ---"
head -8 /tmp/perf.log
echo "--- 总行数: $(wc -l < /tmp/perf.log) ---"
