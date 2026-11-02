kill 20495 2>/dev/null; sleep 0.5
ps aux | grep 20495 | grep -v grep | wc -l
ps aux | grep -iE 'patrol' | grep -v grep | head -5
echo heartbeat > /sys/class/leds/work/trigger
echo 255 > /sys/class/leds/work/brightness
sleep 1
cat /sys/class/leds/work/trigger
cat /sys/class/leds/work/brightness