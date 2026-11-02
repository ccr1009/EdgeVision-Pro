#!/bin/bash
# rkllm_init failed —— 决定性的 A/B 对照实验
# 目的：判断根因是「驱动版本太老」还是「NPU/CMA 内存被抢占」
# 全部操作可恢复：测完自动把 edgevision.service 恢复 active

cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy

snap() {
  echo "  CmaTotal=$(grep CmaTotal /proc/meminfo | awk '{print $2}')kB  CmaFree=$(grep CmaFree /proc/meminfo | awk '{print $2}')kB  MemAvailable=$(grep MemAvailable /proc/meminfo | awk '{print $2}')kB"
  echo "  NPU load: $(cat /sys/kernel/debug/rknpu/load 2>/dev/null | tr -d '\n')"
  echo "  占 renderD129 的进程: $(for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q renderD129 && echo -n "$(basename $p):$(tr '\0' ' ' < $p/cmdline 2>/dev/null | cut -c1-40) "; done)"
}

echo "================ A. 基线（当前运行状态）================"
snap
echo
echo "---- A1: 现在直接跑 llm_demo（预期复现失败）----"
timeout 60 ./llm_demo 2>&1 | head -20
echo "[A1 exit=$?]"

echo
echo "================ B. 停掉占用 NPU 的进程 ================"
ORIG_EDGEVISION=$(systemctl is-active edgevision.service 2>/dev/null)
echo "edgevision.service 原始状态 = $ORIG_EDGEVISION"
systemctl stop edgevision.service 2>/dev/null
pkill -f edgevision_dashboard 2>/dev/null
sleep 3
echo "停完后 ---"
snap
echo "  残留: $(ps -eo pid,args | grep -E 'edgevision|rkllm' | grep -v grep | tr '\n' '|')"

echo
echo "---- B1: 干净环境下再跑 llm_demo ----"
timeout 120 ./llm_demo 2>&1 | head -30
echo "[B1 exit=$?]"

echo
echo "================ C. 恢复现场 ================"
if [ "$ORIG_EDGEVISION" = "active" ]; then
  systemctl start edgevision.service
  sleep 2
  echo "edgevision.service = $(systemctl is-active edgevision.service)"
fi
echo "恢复后 ---"
snap
echo "===== 实验结束 ====="
