#!/bin/bash
# 在板上排查 stereo3d_demo 的来源进程树
grep -n -E 'stereo3d_demo|ablation|smoke|timeout' /tmp/ps_dump.txt
echo ===PPID-CHAIN===
P=22620
for i in 1 2 3 4 5; do
  LINE=$(ps -o ppid= -p $P 2>/dev/null | tr -d ' ')
  [ -z "$LINE" ] && break
  ps -o pid=,ppid=,lstart=,cmd= -p $P 2>/dev/null
  P=$LINE
  [ "$P" = "1" ] || [ "$P" = "0" ] && { ps -o pid=,ppid=,lstart=,cmd= -p $P 2>/dev/null; break; }
done
