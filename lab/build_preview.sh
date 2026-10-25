#!/bin/bash
set -e
echo "[*] 正在编译 RK3588 真实 AI 画框预览系统..."

g++ -O2 /root/lab/w4_edgevision_preview.cpp /root/edgevision/deploy/cpp/src/postprocess.cpp -o /root/lab/w4_edgevision_preview \
    -I/usr/include/opencv4 \
    -I/root/edgevision/deploy/cpp/include \
    -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include \
    -lopencv_core -lopencv_videoio -lopencv_imgproc -lopencv_imgcodecs \
    -lrknnrt -lpthread

echo "[+] 编译成功: /root/lab/w4_edgevision_preview"
ls -lh /root/lab/w4_edgevision_preview
