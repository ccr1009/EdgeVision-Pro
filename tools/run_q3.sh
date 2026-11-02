#!/bin/bash
# 编译并运行虚假唤醒演示, 同时打印退出码
set -u
cd /root/lab || exit 1
sed -i 's/\r$//' q3_spurious.cpp
g++ -O2 -std=c++11 -pthread q3_spurious.cpp -o q3_spurious || exit 2
echo "=== 编译 OK ==="
echo
echo "--- 用 if 的那版(当前代码) ---"
./q3_spurious
RC=$?
echo "退出码 = $RC"
