#!/bin/bash
# 降 conf 到 0.01 复测: 排除「图里真没目标」和「阈值偏高」
# 做法: 直接用 eval_dump 二进制对 a.jpg 导出预测(它用 conf=0.01 口径)
cd /root/edgevision/deploy/cpp/build_native || exit 1
ls -la /tmp/mppo/*.jpg
echo '=== eval_dump 对单图导出(conf=0.01) ==='
mkdir -p /tmp/evaltest
cp /tmp/mppo/a.jpg /tmp/evaltest/
/root/lab/eval_dump /root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn /tmp/evaltest > /tmp/evaltest_preds.txt 2>/tmp/evaltest_err.txt
tail -3 /tmp/evaltest_err.txt
echo '--- preds ---'
cat /tmp/evaltest_preds.txt
