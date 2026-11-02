#!/bin/bash
# 复测 edgevision_pro_rknn 真实链路 x3 (热身后的稳态数字)
cd /root/edgevision
for i in 1 2 3; do
    echo "=== run $i ==="
    ./deploy/cpp/build_native/edgevision_pro_rknn /tmp/mppo/a.jpg 2>/dev/null | grep -E "decode_ms|CSC:|Letterbox|inference:|ground X|person|car|DONE"
done
echo "=== 用 cam1_focused.jpg 再测一张 (人像场景) ==="
./deploy/cpp/build_native/edgevision_pro_rknn /root/lab/cam1_focused.jpg 2>/dev/null
