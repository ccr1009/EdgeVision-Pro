#!/bin/bash
# test_led_duty.sh — 验证换灯状态查询不再把「心跳灭相」误报成「关闭」
cd /root/rkllm_deploy || exit 1
OUT=/tmp/t_led
mkdir -p $OUT

echo "########## 准备: 腾出 NPU(停视觉网关/清残留) ##########"
systemctl stop edgevision.service 2>/dev/null
pkill -f rkllm_embodied 2>/dev/null
sleep 3
ps ax | grep -q "[r]kllm_embodied" && echo "[WARN] 仍有残留" || echo "[OK] NPU 空闲"

echo
echo "########## 用例 A: heartbeat 模式(守护 75C, NORMAL) ##########"
systemctl stop edgevision-patrol 2>/dev/null
pkill -f edgevision_patrol_daemon 2>/dev/null
sleep 1
/root/lab/edgevision_patrol_daemon --temp-enter 75 --temp-exit 70 --interval 5 > $OUT/patrol_ok.log 2>&1 &
sleep 6
echo "--- 物理事实: trigger / 30 次采样 ---"
cat /sys/class/leds/work/trigger | grep -o "\[[a-z-]*\]"
echo -n "采样: "; for i in $(seq 20); do echo -n "$(cat /sys/class/leds/work/brightness) "; sleep 0.06; done; echo
echo "--- Agent 回答 ---"
timeout 300 ./rkllm_embodied_agent "查看当前警告灯状态" > $OUT/a.log 2>&1
tail -22 $OUT/a.log

echo
echo "########## 用例 B: 报警模式(守护 20C, THERMAL_ALERT) ##########"
pkill -f edgevision_patrol_daemon 2>/dev/null
sleep 2
/root/lab/edgevision_patrol_daemon --temp-enter 20 --temp-exit 18 --interval 2 > $OUT/patrol_alarm.log 2>&1 &
sleep 6
echo "--- 物理事实 ---"
cat /root/patrol/patrol_state.json; echo
echo -n "采样: "; for i in $(seq 20); do echo -n "$(cat /sys/class/leds/work/brightness) "; sleep 0.06; done; echo
echo "--- Agent 回答 ---"
timeout 300 ./rkllm_embodied_agent "查看当前警告灯状态" > $OUT/b.log 2>&1
tail -22 $OUT/b.log

echo
echo "########## 收尾: 恢复生产配置 ##########"
pkill -f edgevision_patrol_daemon 2>/dev/null
sleep 1
systemctl start edgevision-patrol
systemctl start edgevision.service
sleep 3
echo "edgevision=$(systemctl is-active edgevision) patrol=$(systemctl is-active edgevision-patrol)"
