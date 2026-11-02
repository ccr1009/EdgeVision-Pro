#!/bin/bash
# run_agent.sh —— 在「NPU 地址空间已被 gateway/dashboard 占满」的板子上安全启动具身 Agent
#
# 背景：RK3588 NPU 走 IOMMU，地址空间只有 32 位 = 4 GiB（与物理内存无关）。
#       每个 rkllm 大模型实例约占 1.95 GB IOVA。
#       edgevision.service（yolov8n 网关）常驻时，再起 rkllm 就会
#       报 `failed to malloc npu memory ... errno: 14`。
#
# 用法：
#   bash /root/run_agent.sh              # 交互式（有终端）
#   bash /root/run_agent.sh "查一下温度"  # 单条指令
#
# 脚本退出时自动把 edgevision.service 拉回来（除非 --no-restore）。

RESTORE=1
[ "$1" = "--no-restore" ] && { RESTORE=0; shift; }

GATEWAY_WAS_ACTIVE=0
systemctl is-active --quiet edgevision.service && GATEWAY_WAS_ACTIVE=1

restore() {
  [ $RESTORE -eq 1 ] || return 0
  if [ $GATEWAY_WAS_ACTIVE -eq 1 ]; then
    echo
    echo "[恢复] 重新拉起 edgevision.service（视觉网关）..."
    systemctl start edgevision.service 2>/dev/null
    sleep 2
    echo "[恢复] edgevision.service = $(systemctl is-active edgevision.service)"
  fi
}
trap restore EXIT INT TERM

echo "════════════════════════════════════════════════════════"
echo " ① 释放 NPU 地址空间"
echo "════════════════════════════════════════════════════════"
# 1. 清掉所有残留的 rkllm 实例（每个 ~2GB IOVA，是最常见的元凶）
LEFT=$(pgrep -f "rkllm_embodied_agent" | wc -l)
if [ "$LEFT" -gt 0 ]; then
  echo "   清理 $LEFT 个残留 rkllm 实例（每个约 2GB NPU 地址空间）"
  pkill -f "rkllm_embodied_agent" 2>/dev/null
  sleep 2
else
  echo "   ✅ 无残留 rkllm 实例"
fi

# 2. 停掉视觉网关（yolov8n，常驻占 NPU）
if [ $GATEWAY_WAS_ACTIVE -eq 1 ]; then
  echo "   暂停 edgevision.service（视觉网关）"
  systemctl stop edgevision.service 2>/dev/null
  sleep 2
fi

# 3. 停掉看板（如果手动起的）
if pgrep -f edgevision_dashboard >/dev/null 2>&1; then
  echo "   暂停 edgevision_dashboard（3D 看板）"
  pkill -f edgevision_dashboard 2>/dev/null
  sleep 2
fi

# 4. 等 NPU 地址空间真正释放
sleep 1
echo "   NPU 负载: $(cat /sys/kernel/debug/rknpu/load 2>/dev/null | sed 's/NPU load: //')"

echo
echo "════════════════════════════════════════════════════════"
echo " ② 启动具身 Agent"
echo "════════════════════════════════════════════════════════"
cd /root/rkllm_deploy || exit 1
export LD_LIBRARY_PATH=/root/rkllm_deploy:$LD_LIBRARY_PATH

if [ -n "$1" ]; then
  # 单条指令模式
  for q in "$@"; do
    echo "$q"
  done | ./rkllm_embodied_agent
  RC=$?
else
  # 交互模式
  ./rkllm_embodied_agent
  RC=$?
fi

if [ $RC -ne 0 ]; then
  echo
  echo "⚠️  退出码 $RC。如果又是 'failed to malloc npu memory ... errno 14'："
  echo "   说明还有别的程序占着 NPU 地址空间，跑 bash /root/npu_doctor.sh 看是谁。"
  echo "   实在收不回来就 reboot（冷重启后地址空间彻底复位）。"
fi
exit $RC
