#!/bin/bash
# 第 6 步：帧率生效性扫描 —— 请求值 vs 实际值
set -u
DEV=/dev/video83
echo "########## 帧率请求 → 实测 对照表（1280x720 MJPG）##########"
printf "%-10s %-12s %-12s\n" "请求fps" "回读fps" "实测fps"
for f in 5 10 15 20 25 30; do
  out=$(/root/lab/bench_cam_fps2 $DEV 1280 720 $f 2>&1)
  rb=$(echo "$out" | grep "回读" | sed 's/.*@ //;s/fps//')
  act=$(echo "$out" | grep "实测" | sed 's/.*=>  *//;s/ fps.*//')
  printf "%-10s %-12s %-12s\n" "$f" "$rb" "$act"
done
echo "=== DONE ==="
