#!/bin/bash
cd /root/lab
D=/root/edgevision/third_party/mpp
g++ -O2 -std=c++11 -o dg dg.cpp -I$D/include -L$D/lib -lrockchip_mpp -lpthread -ldl -lrt -Wl,-rpath,$D/lib 2>&1|head -20
[ -x ./dg ] && timeout 60 ./dg 2>/dev/null
