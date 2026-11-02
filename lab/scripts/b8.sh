#!/bin/bash
cd /root/lab
D=/root/edgevision/third_party/mpp
g++ -O2 -std=c++11 -o rga_final rga_final.cpp -I$D/include -L$D/lib -lrockchip_mpp \
  -I/usr/include/rga -lrga -lpthread -ldl -lrt -Wl,-rpath,$D/lib 2>&1 | grep -E "error" | head
if [ -x ./rga_final ]; then
  echo "--- MODE=0 default ---"
  timeout 60 ./rga_final /tmp/mppo/a.jpg 1280 720 20 0 2>/dev/null
  echo "--- MODE=1 full ---"
  timeout 60 ./rga_final /tmp/mppo/a.jpg 1280 720 20 1 2>/dev/null
fi
