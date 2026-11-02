#!/bin/bash
set -e
echo "[*] 开始编译 EdgeVision 全链路网关服务..."

g++ -O2 /root/lab/w4_edgevision_gateway.cpp -o /root/lab/w4_edgevision_gateway \
    -I/usr/include/opencv4 \
    -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include \
    -lopencv_core -lopencv_videoio -lopencv_imgproc \
    -lrknnrt -lpthread

echo "[+] 编译成功: /root/lab/w4_edgevision_gateway"
ls -lh /root/lab/w4_edgevision_gateway
