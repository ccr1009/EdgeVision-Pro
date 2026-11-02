#!/bin/bash
# 终验: 真实目标图走 MPP 全链路
cd /root/edgevision/deploy/cpp/build_native || exit 1
cp /root/edgevision/data/calib_v2/real_000.jpg /tmp/mppo/real_000.jpg
for i in 1 2 3; do
  ./edgevision_pro_rknn /tmp/mppo/real_000.jpg 2>/dev/null | grep -E "detect|Detection|obj|ms|decode|csc|npu" | head -8
  echo '---'
done
echo "=== COCO 图 ==="
cp /root/edgevision/data/coco128/images/train2017/000000000009.jpg /tmp/mppo/coco9.jpg
./edgevision_pro_rknn /tmp/mppo/coco9.jpg 2>/dev/null | grep -E "detect|Detection|obj|ms|decode|csc|npu" | head -8
