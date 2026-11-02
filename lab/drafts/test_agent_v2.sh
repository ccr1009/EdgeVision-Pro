#!/bin/bash
# test_agent_v2.sh — 停掉占 NPU 的旧 Agent 后, 用新版验证「什么时候报警闪烁」
cd /root/rkllm_deploy || exit 1
OUT=/tmp/t_agent
mkdir -p $OUT

echo "=== 0. 清掉占 NPU 的旧 Agent 实例 ==="
ps ax | grep "[r]kllm_embodied" | sed 's/^/   旧进程: /'
pkill -f rkllm_embodied_agent 2>/dev/null
sleep 4
if ps ax | grep -q "[r]kllm_embodied"; then
    echo "[WARN] 仍有残留, 强杀"
    pkill -9 -f rkllm_embodied_agent 2>/dev/null
    sleep 2
fi
ps ax | grep "[r]kllm_embodied" || echo "[OK] NPU 已释放"

echo
echo "=== 1. 启动守护(20C 阈值) ==="
pkill -f edgevision_patrol_daemon 2>/dev/null
sleep 1
/root/lab/edgevision_patrol_daemon --temp-enter 20 --temp-exit 18 --interval 2 > $OUT/patrol_a.log 2>&1 &
DPID=$!
sleep 6
echo "--- 真实快照 ---"
cat /root/patrol/patrol_state.json; echo
echo "--- LED 实况(8 次, 应 0/255 交替) ---"
for i in $(seq 8); do echo -n "$(cat /sys/class/leds/work/brightness) "; sleep 0.1; done; echo

echo
echo "=== 2. Agent 回答(守护在跑, 44C > 20C 阈值) ==="
timeout 300 ./_new_agent "什么时候报警闪烁" > $OUT/answer_a.log 2>&1
echo "AGENT_RC=$?"
tail -45 $OUT/answer_a.log

echo
echo "=== 3. 停止守护, 再问一次 ==="
kill -TERM $DPID 2>/dev/null
wait $DPID 2>/dev/null
sleep 2
echo "--- 真实快照 ---"
cat /root/patrol/patrol_state.json; echo
echo "--- LED 实况 ---"
echo -n "trigger: "; cat /sys/class/leds/work/trigger | grep -o '\[[a-z-]*\]'
timeout 300 ./_new_agent "现在温度升高会自动报警吗" > $OUT/answer_b.log 2>&1
echo "AGENT_RC=$?"
tail -40 $OUT/answer_b.log
