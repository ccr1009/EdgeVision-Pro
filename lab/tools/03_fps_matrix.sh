#!/bin/bash
# 摄像头 20fps 诊断 - 第 3 步（最终版）
# 用 cam2 (/dev/video83, 当前空闲) 做实验，不干扰正在跑的 edgevision.service
set -u
DEV=/dev/video83

echo "########## 0. cam2 基线能力 ##########"
v4l2-ctl -d $DEV --get-parm 2>&1 | grep -E "Frames per second"

echo
echo "########## 1. 【基线】不设 FPS，1280x720（复现旧结论 20fps）##########"
/root/lab/bench_cam_fps2 $DEV 1280 720 0

echo
echo "########## 2. 【关键】显式请求 30fps，1280x720 ##########"
/root/lab/bench_cam_fps2 $DEV 1280 720 30

echo
echo "########## 3. 请求 25fps ##########"
/root/lab/bench_cam_fps2 $DEV 1280 720 25

echo
echo "########## 4. 降到 640x480 请求 30fps（USB 带宽对照）##########"
/root/lab/bench_cam_fps2 $DEV 640 480 30

echo
echo "########## 5. 1920x1080 请求 30fps（带宽压力测试）##########"
/root/lab/bench_cam_fps2 $DEV 1920 1080 30

echo
echo "########## 6. 锁曝光（exposure_auto=1, 157）后再测 30fps ##########"
v4l2-ctl -d $DEV --set-ctrl=exposure_auto=1 2>&1
v4l2-ctl -d $DEV --set-ctrl=exposure_absolute=157 2>&1
v4l2-ctl -d $DEV --get-ctrl=exposure_auto,exposure_absolute 2>&1
/root/lab/bench_cam_fps2 $DEV 1280 720 30

echo
echo "########## 7. 极短曝光 30 + gain 拉高，验证 AE 假说 ##########"
v4l2-ctl -d $DEV --set-ctrl=exposure_absolute=30 2>&1
v4l2-ctl -d $DEV --set-ctrl=gain=70 2>&1
/root/lab/bench_cam_fps2 $DEV 1280 720 30

echo
echo "########## 8. 恢复自动曝光 ##########"
v4l2-ctl -d $DEV --set-ctrl=gain=0 2>&1
v4l2-ctl -d $DEV --set-ctrl=exposure_auto=3 2>&1
v4l2-ctl -d $DEV --get-ctrl=exposure_auto,exposure_absolute,gain 2>&1
echo "=== DONE ==="
