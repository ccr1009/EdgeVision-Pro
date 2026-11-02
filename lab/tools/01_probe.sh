#!/bin/bash
# 摄像头 20fps 诊断 - 第 1 步：环境侦察
echo "===== 1. 摄像头设备 ====="
v4l2-ctl --list-devices 2>&1

echo
echo "===== 2. /root/lab 相关工具 ====="
ls -la /root/lab/bench_cam_fps 2>/dev/null || echo "NO bench_cam_fps"
ls -la /root/lab/ | grep -Ei "bench|cam|preview|mjpg" || true

echo
echo "===== 3. edgevision.service 状态 ====="
systemctl is-active edgevision 2>&1
systemctl is-enabled edgevision 2>&1

echo
echo "===== 4. 谁占用 video 设备 ====="
fuser -v /dev/video81 /dev/video83 2>&1 || echo "no occupant"

echo
echo "===== 5. mpp 相关工具 ====="
which mpi_dec_test mpi_dec_harness 2>&1 || echo "no mpp test binary in PATH"
ls -la /root/lab/sample.mjpg 2>/dev/null || echo "NO sample.mjpg"

echo
echo "===== 6. 板子型号与 USB 速度 ====="
cat /proc/device-tree/model 2>/dev/null; echo
for d in /sys/bus/usb/devices/*/; do
  if [ -f "$d/idVendor" ]; then
    speed=$(cat "$d/speed" 2>/dev/null)
    prod=$(cat "$d/product" 2>/dev/null)
    echo "  $(basename $d)  speed=${speed}Mbps  product=${prod}"
  fi
done
