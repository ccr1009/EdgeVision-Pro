#!/bin/bash
# 编译并运行三核并行吞吐测试, 同时采样三核负载
set -u
B=/root/edgevision/deploy/cpp/3rdparty/rknn_api
MODEL=/root/edgevision/data/models/yolov8n_rk3588_i8.rknn
cd /root/lab || exit 1

g++ -O2 -std=c++17 bench_multicore.cpp -I"$B/include" -L"$B/lib" -lrknnrt -pthread -o bench_multicore || exit 2
echo "BUILD_OK"
echo

export LD_LIBRARY_PATH="$B/lib:${LD_LIBRARY_PATH:-}"
rm -f /tmp/npu_load_mc.log
(
  while true; do
    cat /sys/kernel/debug/rknpu/load >> /tmp/npu_load_mc.log 2>/dev/null
    sleep 0.2
  done
) &
SPID=$!

./bench_multicore "$MODEL" 100
RC=$?

kill "$SPID" 2>/dev/null
echo
echo "=== 并行期间 NPU 三核负载采样 ==="
sort /tmp/npu_load_mc.log | uniq -c | sort -rn | head -6
exit $RC
