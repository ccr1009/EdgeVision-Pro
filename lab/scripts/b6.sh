#!/bin/bash
cd /root/lab
D=/root/edgevision/third_party/mpp
g++ -O2 -std=c++11 -o rga_drm rga_drm.cpp -I$D/include -L$D/lib -lrockchip_mpp \
  -I/usr/include/rga -lrga -lpthread -ldl -lrt -Wl,-rpath,$D/lib 2>&1 | grep -E "error" | head -10
[ -x ./rga_drm ] && { echo "=== RUN ==="; timeout 120 ./rga_drm /tmp/mppo/a.jpg 640 640 100 2>/dev/null; } || echo BUILD_FAIL
