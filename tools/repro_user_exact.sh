#!/bin/bash
# 最终复现：2 个 rkllm 实例常驻 → 再跑完整 agent，看是否报出用户原始的 196MB 错误
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy

cleanall() {
  pkill -9 -f rkllm_embodied_agent 2>/dev/null
  pkill -9 -f "tail -f /dev/null" 2>/dev/null
  pkill -f edgevision_dashboard 2>/dev/null
  systemctl stop edgevision.service 2>/dev/null
  sleep 4
}

cleanall
echo "══════ 第 0 步：清场 ══════"
echo "  进程: $(pgrep -af 'rkllm_embodied_agent|edgevision_dashboard' || echo 无)"
echo "  累计失败次数: $(dmesg | grep -c 'rknpu_gem_get_pages: dma map')"

echo
echo "══════ 第 1 步：起 2 个常驻实例（占满 NPU 地址空间）══════"
for N in 1 2; do
  tail -f /dev/null | ./rkllm_embodied_agent > /tmp/keep$N.log 2>&1 &
  sleep 1
  PID=$(pgrep -f "rkllm_embodied_agent$" | tail -1)
  echo "  实例$N PID=$PID 启动中..."
done
for i in $(seq 1 120); do
  OK=$(grep -lc "已就绪" /tmp/keep1.log /tmp/keep2.log 2>/dev/null | wc -l)
  [ "$OK" -ge 2 ] && break
  sleep 2
done
echo "  实例1: $(grep -q 已就绪 /tmp/keep1.log && echo '✅已就绪' || echo '❌失败')  RSS=$(pgrep -f rkllm_embodied_agent | head -1 | xargs -I{} awk '/VmRSS/{print $2$3}' /proc/{}/status 2>/dev/null)"
echo "  实例2: $(grep -q 已就绪 /tmp/keep2.log && echo '✅已就绪' || echo '❌失败')"
echo "  存活实例数 = $(pgrep -c -f 'rkllm_embodied_agent$')"
echo "  NPU: $(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"
echo "  MemAvailable=$(awk '/MemAvailable/{print $2}' /proc/meminfo)kB"

echo
echo "══════ 第 2 步：现在跑用户那条原始命令（第 3 个实例）══════"
MARK=$(dmesg | wc -l)
echo "quit" | timeout 200 ./rkllm_embodied_agent > /tmp/user_repro.log 2>&1
echo "  ---- 输出（逐字）----"
cat /tmp/user_repro.log | grep -vE "^\s*$" | sed 's/^/     /'

echo
echo "  ---- 【新增】dmesg ----"
dmesg | tail -n +$((MARK+1)) | grep -E "^\s*[0-9]+: \[|^\s*used:|dma map" | tail -45 | sed 's/^/     /'

echo
echo "══════ 第 3 步：与用户原始日志逐字对比 ══════"
echo "  用户原始: failed to malloc npu memory, size: 205799424, flags: 0x2"
echo "  本次实测: $(grep 'failed to malloc npu memory' /tmp/user_repro.log || echo '（没报这句）')"

echo
echo "══════ 恢复 ══════"
cleanall
systemctl start edgevision.service 2>/dev/null
sleep 3
echo "  gateway=$(systemctl is-active edgevision.service)"
echo "===== DONE ====="
