#!/bin/bash
set -e
echo "[*] 正在编译 RK3588 3D BEV 空间感知实时预览系统..."

g++ -O2 -std=c++17 /root/lab/w4_edgevision_preview_real.cpp \
    /root/edgevision/deploy/cpp/src/postprocess_dfl_free.cpp \
    -o /root/lab/w4_edgevision_preview_real \
    -I/root/lab \
    -I/root/edgevision/deploy/cpp/include \
    -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include \
    `pkg-config --cflags --libs opencv4` \
    -lrknnrt -lpthread

echo "[+] 编译成功: /root/lab/w4_edgevision_preview_real"
ls -lh /root/lab/w4_edgevision_preview_real
