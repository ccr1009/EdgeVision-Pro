#!/bin/bash
# 诊断: 守护启动脚本为什么退出码非 0, 以及清理残留进程

echo "=== 1. 当前残留守护进程 ==="
pgrep -af edgevision_patrol_daemon || echo "(无)"

echo
echo "=== 2. 清理残留 ==="
pkill -TERM -f edgevision_patrol_daemon 2>/dev/null
sleep 1
pkill -KILL -f edgevision_patrol_daemon 2>/dev/null
sleep 0.5
echo "清理后: $(pgrep -af edgevision_patrol_daemon || echo '(无)')"

echo
echo "=== 3. 复现启动命令, 逐段看返回码 ==="
cd /root/lab || exit 1
echo "--- 3a. 二进制存在? ---"
test -x ./edgevision_patrol_daemon && echo "yes" || echo "no"

echo "--- 3b. 带 nohup 后台启动 ---"
nohup ./edgevision_patrol_daemon --interval 2 --dry-run > /tmp/diag_patrol.log 2>&1 &
BGPID=$!
echo "后台 pid=$BGPID"
sleep 0.6
echo "ps 检查:"
ps -p $BGPID -o pid,stat,cmd 2>/dev/null || echo "(ps 找不到该 pid)"

echo "--- 3c. 退出码变量 ==="
echo $BGPID > /tmp/diag_patrol.pid
echo "写入的 pid = $(cat /tmp/diag_patrol.pid)"

echo
echo "=== 4. 守护是否真跑起来了 (外部检查) ==="
sleep 2
pgrep -af edgevision_patrol_daemon || echo "(外部 pgrep 也没找到)"

echo
echo "=== 5. 守护日志 ==="
head -20 /tmp/diag_patrol.log 2>/dev/null || echo "(无日志)"

echo
echo "=== 6. 停止 ==="
kill -TERM $(cat /tmp/diag_patrol.pid) 2>/dev/null
sleep 1
pgrep -af edgevision_patrol_daemon || echo "(已停止)"
