#!/bin/bash
# verify_task5.sh — task-5 板上验证(全程不 stop edgevision.service)
# 用法: 在板上 bash verify_task5.sh;产物: /tmp/patrol_t5/ 下各日志与证据
set -u
BOARD_LOG=/tmp/patrol_t5
mkdir -p $BOARD_LOG
cd /root/lab

echo "===== [0] 前置检查 ====="
echo "-- edgevision.service 状态(必须 active):"
systemctl is-active edgevision.service
BEFORE_ACTIVE=$(systemctl is-active edgevision.service)
date '+%F %T' > $BOARD_LOG/verify_start.ts

echo "-- LED 初始 trigger:"
cat /sys/class/leds/work/trigger
echo "-- 残留爆闪进程检查(应为空):"
ps ax | grep -v grep | grep 'brightness' || echo "(无残留)"
echo "-- 初始温度/NPU:"
cat /sys/class/thermal/thermal_zone0/temp
cat /sys/kernel/debug/rknpu/load

echo "===== [1] 编译(要求 0 error 0 warning) ====="
rm -f edgevision_patrol_daemon
bash build_patrol_daemon.sh 2>&1 | tee $BOARD_LOG/build.log
BUILD_RC=${PIPESTATUS[0]}
echo "BUILD_RC=$BUILD_RC"
md5sum edgevision_patrol_daemon.cpp edgevision_patrol_daemon | tee $BOARD_LOG/md5.txt

echo "===== [2] 参数校验用例(非法参数应返回 2) ====="
./edgevision_patrol_daemon --temp-exit 80 --temp-enter 75 >/dev/null 2>$BOARD_LOG/bad_args.log
echo "exit>=enter rc=$? (期望2)"
./edgevision_patrol_daemon --temp-enter 999 >/dev/null 2>>$BOARD_LOG/bad_args.log
echo "temp-enter 999 rc=$? (期望2)"
./edgevision_patrol_daemon --person-score 1.5 >/dev/null 2>>$BOARD_LOG/bad_args.log
echo "person-score 1.5 rc=$? (期望2)"
./edgevision_patrol_daemon --temp-enter abc >/dev/null 2>>$BOARD_LOG/bad_args.log
echo "temp-enter abc rc=$? (期望2)"
echo "-- stderr 汇总:"
cat $BOARD_LOG/bad_args.log

echo "===== [3] 单实例锁用例(第二个实例应返回 3) ====="
./edgevision_patrol_daemon --no-led --interval 3600 > $BOARD_LOG/inst1.log 2>&1 &
INST1_PID=$!
sleep 1
./edgevision_patrol_daemon --no-led --interval 3600 > $BOARD_LOG/inst2.log 2>&1
echo "second instance rc=$? (期望3)"
cat $BOARD_LOG/inst2.log
kill -TERM $INST1_PID
wait $INST1_PID 2>/dev/null
echo "inst1 ended rc=$? (期望0)"

echo "===== [4] 20s 实跑: --temp-enter 20 --temp-exit 18 --interval 2 真实爆闪 ====="
# 清掉上轮日志, 便于取证本轮事件(旧日志挪走不删)
mv /root/patrol/patrol_events.log /root/patrol/patrol_events.log.pre_t5 2>/dev/null
mv /root/patrol/llm_pending.jsonl /root/patrol/llm_pending.jsonl.pre_t5 2>/dev/null
# 起守护(默认 LED 模式), 后台跑
./edgevision_patrol_daemon --temp-enter 20 --temp-exit 18 --interval 2 > $BOARD_LOG/run20.log 2>&1 &
RUN_PID=$!
sleep 3   # 等首个周期翻转告警并起爆闪
# 连续采样 brightness 20 次(间隔 0.1s;爆闪周期 200ms, 应看到 0/255 交替)
echo "-- brightness 连续采样(爆闪证据):"
for i in $(seq 1 20); do
    cat /sys/class/leds/work/brightness
    sleep 0.1
done | tee $BOARD_LOG/brightness_samples.txt
# 期间再取一次 trigger(爆闪需 trigger=none 才能直接控 brightness)
echo "-- 采样期间 trigger(期望含 [none]):"
cat /sys/class/leds/work/trigger | tee $BOARD_LOG/trigger_during_alarm.txt
echo "-- 爆闪子进程(应看到 sh -c while true ... brightness):"
ps ax | grep -v grep | grep 'brightness' | tee $BOARD_LOG/alarm_proc.txt
# SIGTERM 优雅退出
kill -TERM $RUN_PID
for i in $(seq 1 30); do
    kill -0 $RUN_PID 2>/dev/null || break
    sleep 0.2
done
sleep 0.5
echo "-- 退出后 trigger(期望 [heartbeat]):"
cat /sys/class/leds/work/trigger | tee $BOARD_LOG/trigger_after_exit.txt
echo "-- 退出后爆闪进程(应为空):"
ps ax | grep -v grep | grep 'brightness' || echo "(无残留)"
ps ax | grep -v grep | grep 'brightness' > $BOARD_LOG/alarm_proc_after.txt
echo "alarm_proc_after 行数: $(wc -l < $BOARD_LOG/alarm_proc_after.txt) (期望0)"
echo "-- 退出后 brightness:"
cat /sys/class/leds/work/brightness

echo "===== [5] 日志取证 ====="
echo "-- run20.log 全文:"
cat $BOARD_LOG/run20.log
echo "-- patrol_events.log 本轮全部行:"
cat /root/patrol/patrol_events.log | tee $BOARD_LOG/events_t5.log
echo "-- llm_pending.jsonl 本轮全部行:"
cat /root/patrol/llm_pending.jsonl | tee $BOARD_LOG/llm_t5.log
echo "-- edgevision.service 终态(必须 active):"
systemctl is-active edgevision.service
AFTER_ACTIVE=$(systemctl is-active edgevision.service)
[ "$BEFORE_ACTIVE" = "active" ] && [ "$AFTER_ACTIVE" = "active" ] && echo "EDGEVISION_OK: 前后均 active" || echo "EDGEVISION_CHANGED: 需要人工检查!"
date '+%F %T' > $BOARD_LOG/verify_end.ts
echo "===== verify done, 产物在 $BOARD_LOG/ ====="
