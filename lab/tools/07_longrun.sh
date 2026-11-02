#!/bin/bash
# 第 7 步：30fps 长时稳定性 + cam1 稳定性复核
set -u
echo "########## A. cam2 30fps 长时（400 帧 ≈ 13s）##########"
/root/lab/bench_cam_ts /dev/video83 30 400

echo
echo "########## B. 停 service，cam1 30fps 长时复核 ##########"
systemctl stop edgevision
sleep 2
/root/lab/bench_cam_ts /dev/video81 30 400

echo
echo "########## C. cam1 恢复 + service 拉起 ##########"
systemctl start edgevision
sleep 3
systemctl is-active edgevision
echo "=== DONE ==="
