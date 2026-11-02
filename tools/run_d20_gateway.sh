#!/bin/bash
# D20 真 NPU 网关: 编译 + 启动(后台), 供电脑侧客户端连接
set -u
B=/root/edgevision/deploy/cpp/3rdparty/rknn_api
cd /root/lab || exit 1

sed -i 's/\r$//' d20_gateway_npu.cpp

echo "=== 编译 ==="
g++ -O2 -std=c++17 d20_gateway_npu.cpp \
    -I"$B/include" -L"$B/lib" -lrknnrt -pthread -o d20_gateway_npu || exit 2
echo "BUILD_OK"

pkill -f d20_gateway_npu 2>/dev/null
sleep 0.3
export LD_LIBRARY_PATH="$B/lib:${LD_LIBRARY_PATH:-}"
nohup ./d20_gateway_npu > /tmp/d20npu.log 2>&1 &
echo "已启动 pid=$!"
sleep 2
echo "--- 端口状态 ---"
ss -ltn 2>/dev/null | grep ":9000" || echo "(还没监听)"
