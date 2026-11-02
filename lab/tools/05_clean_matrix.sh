#!/bin/bash
# 第 5 步：干净状态三点对照 + 查明 25fps 异常
set -u

restore() {
  D=$1
  v4l2-ctl -d $D --set-ctrl=gain=0            >/dev/null 2>&1
  v4l2-ctl -d $D --set-ctrl=exposure_auto=3   >/dev/null 2>&1
  v4l2-ctl -d $D --set-ctrl=white_balance_temperature_auto=1 >/dev/null 2>&1
  v4l2-ctl -d $D --set-ctrl=brightness=0      >/dev/null 2>&1
  v4l2-ctl -d $D --set-ctrl=gamma=110         >/dev/null 2>&1
  sleep 1
  echo "  [restore] $(v4l2-ctl -d $D --get-ctrl=exposure_auto,gain 2>&1 | tr '\n' ' ')"
}

echo "########## A. cam2 恢复到出厂默认后，重测三点 ##########"
restore /dev/video83
for f in 0 20 25 30; do
  echo "--- 请求 ${f} fps (0=不设置) ---"
  /root/lab/bench_cam_fps2 /dev/video83 1280 720 $f
done

echo
echo "########## B. cam2 用 v4l2-ctl 直接设 25fps，再用 OpenCV 读 ##########"
v4l2-ctl -d /dev/video83 --set-fmt-video=width=1280,height=720,pixelformat=MJPG 2>&1
v4l2-ctl -d /dev/video83 --set-parm=25 2>&1
echo "--- v4l2 设完后 parm: ---"
v4l2-ctl -d /dev/video83 --get-parm 2>&1 | grep -E "Frames per second"
echo "--- 用 OpenCV 不设 fps 采 100 帧（看是否继承 v4l2 的 25） ---"
/root/lab/bench_cam_fps2 /dev/video83 1280 720 0

echo
echo "########## C. cam2 用 v4l2-ctl 直接设 30fps，再用 OpenCV 读 ##########"
v4l2-ctl -d /dev/video83 --set-parm=30 2>&1
v4l2-ctl -d /dev/video83 --get-parm 2>&1 | grep -E "Frames per second"
/root/lab/bench_cam_fps2 /dev/video83 1280 720 0

echo
echo "########## D. 恢复 30fps 默认 ##########"
v4l2-ctl -d /dev/video83 --set-parm=30 2>&1
echo "=== DONE ==="
