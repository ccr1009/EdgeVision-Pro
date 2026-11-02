#!/bin/bash
set -u
cd /root/lab
for f in 15 25; do
  echo "=== 请求 ${f} fps ==="
  ./bench_cam_ts /dev/video83 $f 80
done
echo "=== DONE ==="
