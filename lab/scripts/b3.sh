#!/bin/bash
cd /root/lab
D=/root/edgevision/third_party/mpp
g++ -O2 -std=c++11 -o rgatest rgatest.cpp -I$D/include -L$D/lib -lrockchip_mpp \
  -I/usr/include/rga -lrga -lpthread -ldl -lrt -Wl,-rpath,$D/lib 2>&1 | head -30
[ -x ./rgatest ] && { echo "=== RUN ==="; timeout 120 ./rgatest /tmp/mppo/a.jpg 640 640 100 2>/dev/null; }
