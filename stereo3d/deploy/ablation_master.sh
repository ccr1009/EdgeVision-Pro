#!/bin/bash
# =============================================================================
# ablation_master.sh —— task-5 消融主控（板上执行）
# 硬性协议：测前 systemctl stop edgevision（释放 /dev/video81），
#           测完【无论如何】systemctl start edgevision 并确认 active。
# =============================================================================
set -u
LOG=/root/stereo3d/ablation_master.log

echo "==== [master] 停 edgevision 释放摄像头 $(date '+%F %T') ===="
systemctl stop edgevision
sleep 1
systemctl is-active edgevision || true

echo "==== [master] 跑消融 ===="
bash /root/stereo3d/ablation_run.sh 2>&1

RC=$?
echo "==== [master] 消融退出码 $RC，恢复 edgevision $(date '+%F %T') ===="
systemctl start edgevision
sleep 2
systemctl is-active edgevision && echo "edgevision ACTIVE - 摄像头已归还生产服务" \
    || echo "!!! edgevision NOT ACTIVE - 需要人工介入 !!!"
