#!/bin/bash
# 板上原生编译 8080 实时预览（检测框 + 3D 棱边 + BEV 小地图）
#
# ⚠️ 版本说明（2026-11-02 重命名后）：
#   仓库源码叫 lab/vision/edgevision_preview_real.cpp（去掉 w4_ 前缀，
#   因为它早已是项目主线，不是第 4 周练习）。
#   **板上源码与二进制名保持 w4_ 前缀不变**，避免打断既有部署习惯与脚本。
#   两边文件名不需要一致——本脚本按板端真实文件名编译。
#
# 部署方式：
#   scp lab/vision/edgevision_preview_real.cpp rk3588:/root/lab/w4_edgevision_preview_real.cpp
#   然后板上执行本脚本。
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
