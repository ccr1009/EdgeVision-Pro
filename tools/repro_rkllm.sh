#!/bin/bash
# 用真正报错的 binary 复现 + 查 NPU 驱动内存池
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy

echo "===== 0. NPU 驱动 debugfs 全貌 ====="
ls -l /sys/kernel/debug/rknpu/ 2>/dev/null
for f in /sys/kernel/debug/rknpu/*; do
  [ -f "$f" ] || continue
  echo "--- $f ---"
  head -20 "$f" 2>/dev/null
done

echo
echo "===== 1. 复现（当前环境，edgevision 在跑）====="
snap() {
  echo "  CmaFree=$(grep CmaFree /proc/meminfo | awk '{print $2}')kB MemFree=$(grep MemFree /proc/meminfo | awk '{print $2}')kB MemAvailable=$(grep MemAvailable /proc/meminfo | awk '{print $2}')kB"
}
snap
timeout 90 ./rkllm_embodied_agent 2>&1 | head -25
echo "[exit=$?]"
echo "-- 跑完后 --"
snap
echo "--- dmesg 新增 ---"
dmesg 2>/dev/null | tail -8

echo
echo "===== 2. 停掉 edgevision 后再试（对照）====="
ORIG=$(systemctl is-active edgevision.service 2>/dev/null)
systemctl stop edgevision.service 2>/dev/null; pkill -f edgevision_dashboard 2>/dev/null
sleep 3
snap
timeout 90 ./rkllm_embodied_agent 2>&1 | head -25
echo "[exit=$?]"
echo "-- 跑完后 --"
snap
echo "--- dmesg 新增 ---"
dmesg 2>/dev/null | tail -8

echo
echo "===== 3. 恢复 ====="
[ "$ORIG" = "active" ] && { systemctl start edgevision.service; sleep 2; }
echo "edgevision.service = $(systemctl is-active edgevision.service)"
echo "===== DONE ====="
