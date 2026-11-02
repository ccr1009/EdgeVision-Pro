cat /sys/class/leds/work/trigger | head -c 200 | od -c | head -5
ls -la /sys/class/leds/work/trigger
ps aux | grep -iE 'patrol|alarm_blink|led' | grep -v grep | head -10