#!/bin/bash
# 探针: LED heartbeat 触发器在当前板上是否真的出光
# 现象背景: trigger=[heartbeat] 但 brightness 恒读 0 —— 需要区分三种可能
#   (A) 真的在闪, 只是采样恰好都落在暗相 (heartbeat 是双脉冲, 暗相占比高)
#   (B) 触发器挂上了但没有硬件输出 (led 类设备问题 / 引脚未接)
#   (C) 被别的进程(systemd/驱动)持续覆盖

LED_DIR=/sys/class/leds/work

echo "=== 1. 基础属性 ==="
echo "trigger     : $(cat $LED_DIR/trigger)"
echo "brightness  : $(cat $LED_DIR/brightness)"
echo "max_brightness: $(cat $LED_DIR/max_brightness)"
echo "delay_on/off: $(cat $LED_DIR/delay_on 2>/dev/null) / $(cat $LED_DIR/delay_off 2>/dev/null)"

echo
echo "=== 2. 高频采样 3 秒 (10ms 间隔), 统计非零占比 ==="
i=0
nz=0
total=0
while [ $i -lt 300 ]; do
    v=$(cat $LED_DIR/brightness)
    total=$((total + 1))
    if [ "$v" != "0" ]; then
        nz=$((nz + 1))
    fi
    i=$((i + 1))
    sleep 0.01
done
echo "非零采样: $nz / $total"

echo
echo "=== 3. 关掉触发器, 手动强制常亮, 看 brightness 是否保持 255 ==="
echo none > $LED_DIR/trigger
echo 255 > $LED_DIR/brightness
echo "写 255 后立即读: $(cat $LED_DIR/brightness)"
sleep 1
echo "1 秒后读     : $(cat $LED_DIR/brightness)"

echo
echo "=== 4. 手动常亮期间采样 1 秒 ==="
i=0
nz2=0
while [ $i -lt 20 ]; do
    v=$(cat $LED_DIR/brightness)
    if [ "$v" != "0" ]; then nz2=$((nz2 + 1)); fi
    i=$((i + 1))
    sleep 0.05
done
echo "常亮模式下非零采样: $nz2 / 20"
echo ">>> 目视检查: 此刻板载 LED 是否点亮 (常亮)?"

echo
echo "=== 5. 恢复 heartbeat ==="
echo heartbeat > $LED_DIR/trigger
echo 255 > $LED_DIR/brightness
sleep 0.5
echo "trigger    : $(cat $LED_DIR/trigger | tr ' ' '\n' | grep '\[')"
echo "brightness : $(cat $LED_DIR/brightness)"

echo
echo "=== 6. 谁是 LED 的占用者 (可能被其他进程覆盖) ==="
echo "--- 监听 work 的进程 fuser ---"
fuser -v $LED_DIR/brightness 2>&1 || echo "(无进程持有)"
echo "--- led-heartbeat 服务 ---"
systemctl is-active led-heartbeat 2>/dev/null || echo "(无该服务)"
systemctl is-enabled led-heartbeat 2>/dev/null || true
echo "--- 相关服务列表 ---"
systemctl list-units --type=service --state=running 2>/dev/null | grep -iE 'led|heartbeat' || echo "(无匹配)"

echo
echo "=== 探针结束 ==="
