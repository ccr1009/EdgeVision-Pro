#!/bin/bash
# 场景复现：只跑 gateway（systemd 自启的那个），rkllm 能不能加载？
# 这是用户最可能的「日常状态」：开机自启 gateway → 想跑 agent → 失败
cd /root/rkllm_deploy

try_load() {
  local tag="$1"
  local out
  out=$(echo "quit" | timeout 180 ./rkllm_embodied_agent 2>&1)
  if echo "$out" | grep -q "已就绪"; then
    echo "  [$tag] ✅ 加载成功"
  else
    echo "  [$tag] ❌ 加载失败"
    echo "$out" | grep -E "failed to allocate|failed to malloc|rkllm_init failed" | sed 's/^/       /'
    echo "       --- 驱动 IOMMU 占用 ---"
    dmesg 2>/dev/null | grep -E "used: " | tail -1 | sed 's/^/       /'
  fi
}

cleanup() {
  pkill -f edgevision_dashboard 2>/dev/null
  pkill -f "rkllm_embodied_agent" 2>/dev/null
  systemctl stop edgevision.service 2>/dev/null
  sleep 3
}

echo "############ 场景 1：只剩 gateway 在跑（模拟开机自启）############"
cleanup
systemctl start edgevision.service 2>/dev/null
sleep 8
echo "  gateway = $(systemctl is-active edgevision.service), PID=$(pgrep -f w4_edgevision_gateway)"
echo "  gateway 运行 8 秒后："
try_load "gateway 刚起 8s"

echo
echo "  等 gateway 跑 60 秒积累后再试："
sleep 60
echo "  gateway RSS=$(ps -p $(pgrep -f w4_edgevision_gateway) -o rss= 2>/dev/null)kB  NPU=$(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"
try_load "gateway 跑 68s"

echo
echo "############ 场景 2：停掉 gateway，只留系统干净 ############"
cleanup
sleep 2
echo "  gateway = $(systemctl is-active edgevision.service)"
try_load "全干净"

echo
echo "############ 恢复 ############"
systemctl start edgevision.service 2>/dev/null
sleep 2
echo "  gateway = $(systemctl is-active edgevision.service)"
echo "===== DONE ====="
