#!/bin/bash
# 上板验证: edgevision_pro 真实链路编译 + 跑真实 jpg
set -e
echo "=== 1. 同步文件已在 PC 侧 scp 完成, 板上直接编译 ==="
cd /root/edgevision/deploy/cpp
rm -rf build_native && mkdir -p build_native && cd build_native
cmake .. -DCROSS_COMPILE_RK3588=ON 2>&1 | tail -8
echo "=== 2. make ==="
make -j4 2>&1 | tail -25
echo "=== 3. 验证产物 ==="
ls -la edgevision_pro edgevision_rknn edgevision_pro_rknn 2>&1 || true
ldd ./edgevision_pro | grep -i mpp || echo "(static, no mpp dep)"
echo "=== 4. 跑真实 jpg (a.jpg 1280x720) ==="
cd /root/edgevision
EV_MODEL=/root/edgevision/data/models/yolov8n_dflfree_i8.rknn ./deploy/cpp/build_native/edgevision_pro /tmp/mppo/a.jpg
echo "=== DONE exit=$? ==="
