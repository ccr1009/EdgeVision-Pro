#!/bin/bash
# MPP / JPEG 硬解能力侦察（只读，不碰摄像头）
echo "===== 1. MPP 库版本 ====="
strings /usr/lib/aarch64-linux-gnu/librockchip_mpp.so.0 2>/dev/null | grep -iE '^mpp_|version|1\.[0-9]+\.[0-9]+' | head -20
dpkg -l | grep -iE 'mpp|rockchip' | head -20

echo
echo "===== 2. dmesg 里所有 mpp 相关设备 ====="
dmesg | grep -iE 'mpp_|jpeg|jpege|jpegd|vdpu|rkvdec|rkvenc' | head -60

echo
echo "===== 3. mpp_service debugfs ====="
ls -la /sys/kernel/debug/mpp_service/ 2>/dev/null
for f in /sys/kernel/debug/mpp_service/*; do
  [ -f "$f" ] && echo "--- $f ---" && head -20 "$f" 2>/dev/null
done

echo
echo "===== 4. 设备树里的 jpeg 节点 ====="
find /proc/device-tree -maxdepth 2 -iname '*jpeg*' 2>/dev/null
ls /proc/device-tree/ 2>/dev/null | grep -iE 'jpeg|vpu|vdec|venc'

echo
echo "===== 5. 已加载的内核模块 ====="
lsmod | grep -iE 'mpp|vcodec|rkvdec|jpeg' 

echo
echo "===== 6. MPP 头文件版本 / 能力表 ====="
grep -rn "MPP_VIDEO_CodingMJPEG\|MPP_DEVICE_JPEGD\|CodingJPEG" /usr/include/rockchip/*.h | head -20

echo "===== DONE ====="
