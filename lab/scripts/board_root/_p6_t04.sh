#!/bin/bash
cd /root/rkllm_deploy
export LD_LIBRARY_PATH=/root/rkllm_deploy${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
echo "=== T04 heartbeat ?? ==="
timeout 180 ./rkllm_embodied_agent_p0_t6 "???????" 2>&1
echo "T04_EXIT=$?"
echo "=== T04 ? LED ?? ==="
cat /sys/class/leds/work/trigger
cat /sys/class/leds/work/brightness