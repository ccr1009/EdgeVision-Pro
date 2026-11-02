#!/bin/bash
# 决定性实验：验证「edgevision_dashboard 泄漏 NPU IOMMU 空间」是根因
cd /root/rkllm_deploy

show() {
  echo "  [状态] dashboard在跑? $(pgrep -f edgevision_dashboard >/dev/null && echo YES || echo NO) | gateway在跑? $(systemctl is-active edgevision.service) | CmaFree=$(awk '/CmaFree/{print $2}' /proc/meminfo)kB"
}

try_load() {
  local tag="$1"
  local out
  out=$(echo "quit" | timeout 150 ./rkllm_embodied_agent 2>&1)
  if echo "$out" | grep -q "已就绪"; then
    echo "  [$tag] ✅ 加载成功"
  else
    echo "  [$tag] ❌ 加载失败:"
    echo "$out" | grep -E "failed to allocate|failed to malloc|load model file error|rkllm_init failed" | sed 's/^/       /'
    # 抓驱动打印的 IOMMU 空间 dump
    dmesg 2>/dev/null | grep "used: " | tail -2
  fi
}

echo "================= 阶段 0：当前状态 ================="
show
ps -eo pid,etimes,args | grep -E "edgevision_dashboard|w4_edgevision" | grep -v grep

echo
echo "================= 阶段 1：清空所有 NPU 用户后加载（对照组）================="
pkill -f edgevision_dashboard 2>/dev/null
systemctl stop edgevision.service 2>/dev/null
sleep 4
show
try_load "干净环境"

echo
echo "================= 阶段 2：启动 dashboard 后再加载（实验组）================="
cd /root/edgevision 2>/dev/null || cd /
nohup ./deploy/cpp/build_release/edgevision_dashboard --port 8090 >/tmp/dash.log 2>&1 &
DASH_PID=$!
echo "  dashboard PID = $DASH_PID，等它跑 90 秒积累 IOMMU 映射..."
sleep 90
show
echo "  dashboard 是否还活着: $(ps -p $DASH_PID -o stat= 2>/dev/null || echo 已退出)"
try_load "dashboard 占着 NPU"

echo
echo "================= 阶段 3：恢复现场 ================="
pkill -f edgevision_dashboard 2>/dev/null
sleep 2
systemctl start edgevision.service 2>/dev/null
sleep 2
show
echo "===== DONE ====="
