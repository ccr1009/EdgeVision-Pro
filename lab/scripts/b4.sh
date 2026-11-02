#!/bin/bash
cd /root/lab
D=/root/edgevision/third_party/mpp
g++ -O2 -std=c++11 -o rga_nv12 rga_nv12.cpp -I$D/include -L$D/lib -lrockchip_mpp \
  -I/usr/include/rga -lrga -lpthread -ldl -lrt -Wl,-rpath,$D/lib 2>&1 | grep -E "error|Error" | head -20
[ -x ./rga_nv12 ] && { echo "=== RUN ==="; timeout 120 ./rga_nv12 /tmp/mppo/a.jpg 640 640 100 2>/dev/null; } || echo BUILD_FAIL
