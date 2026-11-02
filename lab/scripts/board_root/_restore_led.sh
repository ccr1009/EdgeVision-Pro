echo heartbeat > /sys/class/leds/work/trigger
echo 255 > /sys/class/leds/work/brightness
sleep 1
cat /sys/class/leds/work/trigger
cat /sys/class/leds/work/brightness