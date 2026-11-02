#!/bin/bash
echo "══════════ A. 当前所有残留进程（含 LED 爆闪子进程）══════════"
ps -eo pid,ppid,etimes,rss,args --sort=-etimes | grep -E "rkllm|led|brightness|blink|trigger" | grep -v grep | sed 's/^/  /'
echo "  （空 = 无残留）"

echo
echo "══════════ B. 源码里 LED 爆闪子进程是怎么 fork 的、怎么回收的 ══════════"
echo "--- 搜索 fork / blink / alarm 相关 ---"
grep -n -E "fork\(\)|alarm|blink|brightness|kill\(|waitpid" /root/rkllm_deploy/rkllm_embodied_agent.cpp 2>/dev/null | head -40

echo
echo "══════════ C. 关键：fork 后是否 exec？是否设了 PR_SET_PDEATHSIG？══════════"
grep -n -A6 -B2 "fork()" /root/rkllm_deploy/rkllm_embodied_agent.cpp 2>/dev/null | head -60

echo
echo "══════════ D. 子进程是否关闭了 NPU 的 fd / 是否 exit 前有清理 ══════════"
grep -n -E "exit\(0\)|_exit|return 0;" /root/rkllm_deploy/rkllm_embodied_agent.cpp 2>/dev/null | head -20

echo
echo "══════════ E. 二进制里有没有 led_blink 等子进程特征串 ══════════"
strings /root/rkllm_deploy/rkllm_embodied_agent 2>/dev/null | grep -iE "blink|alarm|brightness|/sys/class/leds" | head -15

echo
echo "══════════ F. 现在系统整体 NPU 使用状态 ══════════"
echo "  NPU load: $(cat /sys/kernel/debug/rknpu/load | tr -d '\n')"
echo "  历史失败次数: $(dmesg | grep -c 'dma map')"
echo "  MemAvailable=$(awk '/MemAvailable/{print $2}' /proc/meminfo)kB"
echo "  gateway=$(systemctl is-active edgevision.service)"
echo "===== DONE ====="
