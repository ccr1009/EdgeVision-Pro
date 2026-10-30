#!/bin/bash
# verify_state.sh — 验证「运行态快照」是否真实反映守护状态
cd /root/lab || exit 1
OUT=/tmp/verify_state
rm -rf $OUT && mkdir -p $OUT

echo "=== 0. 前置: 无残留守护 ==="
pkill -f edgevision_patrol_daemon 2>/dev/null
sleep 1
ps ax | grep "[e]dgevision_patrol_daemon" || echo "(无守护在跑)"

echo
echo "=== 1. 当前 LED 残留态(启动前) ==="
echo -n "trigger: "; cat /sys/class/leds/work/trigger | grep -o '\[[a-z-]*\]'
echo -n "brightness: "; cat /sys/class/leds/work/brightness
echo -n "快照是否存在: "; [ -f /root/patrol/patrol_state.json ] && echo "存在(旧)" || echo "不存在"

echo
echo "=== 2. 启动守护(20C 阈值) ==="
./edgevision_patrol_daemon --temp-enter 20 --temp-exit 18 --interval 2 > $OUT/run.log 2>&1 &
DPID=$!
echo "daemon pid=$DPID"
sleep 6

echo
echo "=== 3. 残留自检是否触发(看日志) ==="
grep -E "残留|heartbeat" $OUT/run.log || echo "(未触发自检, 说明启动前不是残留态)"

echo
echo "=== 4. 快照文件内容 ==="
cat /root/patrol/patrol_state.json; echo

echo
echo "=== 5. 快照是否随时间刷新(间隔 2s, 采两次) ==="
A=$(grep -o '"updated_at":"[^"]*"' /root/patrol/patrol_state.json)
sleep 3
B=$(grep -o '"updated_at":"[^"]*"' /root/patrol/patrol_state.json)
echo "t0: $A"
echo "t1: $B"
[ "$A" != "$B" ] && echo "[OK] 快照在刷新" || echo "[FAIL] 快照未刷新"

echo
echo "=== 6. 运行时 LED 实况 ==="
echo -n "brightness 连采 8 次: "
for i in $(seq 8); do echo -n "$(cat /sys/class/leds/work/brightness) "; sleep 0.1; done; echo

echo
echo "=== 7. SIGTERM 优雅退出 ==="
kill -TERM $DPID
wait $DPID 2>/dev/null
echo "exit_rc=$?"
echo -n "退出后 trigger: "; cat /sys/class/leds/work/trigger | grep -o '\[[a-z-]*\]'
echo -n "退出后快照: "; cat /root/patrol/patrol_state.json; echo

echo
echo "=== 8. 日志尾部 ==="
tail -8 $OUT/run.log
