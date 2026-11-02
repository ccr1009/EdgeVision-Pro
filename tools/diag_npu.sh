#!/bin/bash
# 诊断: 单帧 37ms 到底是 NPU 在算, 还是偷偷回退到 CPU 了
set -u
MODEL=/root/edgevision/data/models/yolov8n_rk3588_i8.rknn
cd /root/lab || exit 1
export LD_LIBRARY_PATH=/root/edgevision/deploy/cpp/3rdparty/rknn_api/lib:${LD_LIBRARY_PATH:-}

echo "=== 1. CPU 时间 vs 墙钟 (若 Percent of CPU 很低 => 进程在等 NPU, 没跑 CPU) ==="
/usr/bin/time -v ./bench_npu "$MODEL" 200 > /tmp/bench_out.log 2> /tmp/time.log
grep -E "User time|System time|Elapsed \(wall|Percent of CPU" /tmp/time.log

echo
echo "=== 2. 压测期间 CPU 占用 (top 快照) ==="
./bench_npu "$MODEL" 400 > /dev/null 2>&1 &
BPID=$!
sleep 6
top -b -n 1 | head -12
kill "$BPID" 2>/dev/null
wait 2>/dev/null

echo
echo "=== 3. dmesg 里 NPU 相关的报错/告警 ==="
dmesg | grep -i -E "rknpu|npu" | tail -10
