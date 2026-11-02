#!/bin/bash
# 摄像头 20fps 诊断 - 第 4 步：复核异常 + 在正主 cam1 上验证
set -u
echo "########## A. cam2 复核 25fps 异常（连测两次）##########"
/root/lab/bench_cam_fps2 /dev/video83 1280 720 25
echo "--- 再来一次 ---"
/root/lab/bench_cam_fps2 /dev/video83 1280 720 25

echo
echo "########## B. cam2 复核 30fps 稳定性（连测两次）##########"
/root/lab/bench_cam_fps2 /dev/video83 1280 720 30
/root/lab/bench_cam_fps2 /dev/video83 1280 720 30

echo
echo "########## C. 停掉 edgevision.service，释放 cam1 ##########"
systemctl stop edgevision
sleep 2
fuser -v /dev/video81 2>&1 || echo "cam1 已空闲"

echo
echo "########## D. 【正主】cam1 不设 FPS（复现旧结论）##########"
/root/lab/bench_cam_fps2 /dev/video81 1280 720 0

echo
echo "########## E. 【正主】cam1 显式请求 30fps ##########"
/root/lab/bench_cam_fps2 /dev/video81 1280 720 30

echo
echo "########## F. 网络端口 8080 即将可用的验证（供后续预览用）##########"
echo "cam1 已释放"

echo
echo "########## G. 恢复 edgevision.service ##########"
systemctl start edgevision
sleep 3
systemctl is-active edgevision
echo "=== DONE ==="
