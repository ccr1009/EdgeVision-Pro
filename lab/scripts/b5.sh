#!/bin/bash
cd /root/lab
g++ -O2 -std=c++11 -o rga_min rga_min.cpp -I/usr/include/rga -lrga -lpthread -ldl 2>&1 | grep -E "error" | head -10
[ -x ./rga_min ] && timeout 30 ./rga_min 2>&1 | head -30 || echo BUILD_FAIL
