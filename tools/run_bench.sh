#!/bin/bash
# 在板子上跑 NPU 性能探针，同时后台采样三核负载
# 用法: bash run_bench.sh [迭代次数]
set -u
MODEL=/root/edgevision/data/models/yolov8n_rk3588_i8.rknn
ITERS=${1:-300}
LIB=/root/edgevision/deploy/cpp/3rdparty/rknn_api/lib

cd /root/lab || exit 1
export LD_LIBRARY_PATH="$LIB:${LD_LIBRARY_PATH:-}"
rm -f /tmp/npu_load.log

(
  while true; do
    cat /sys/kernel/debug/rknpu/load >> /tmp/npu_load.log 2>/dev/null
    sleep 0.15
  done
) &
SPID=$!

echo "=== NPU 频率 ==="
for f in /sys/class/devfreq/*/cur_freq; do
  [ -f "$f" ] && echo "$f = $(cat "$f")"
done

timeout 300 ./bench_npu "$MODEL" "$ITERS"
RC=$?

kill "$SPID" 2>/dev/null
echo
echo "=== NPU 三核负载采样(出现次数 | 采样值) ==="
sort /tmp/npu_load.log | uniq -c | sort -rn | head -8
exit $RC
