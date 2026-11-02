#!/bin/bash
# 决定性对照: 已知高分校准图 real_000~002 (当年 9/9 检出 avg 0.8072)
mkdir -p /tmp/evaltest3
cp /root/edgevision/data/calib_v2/real_000.jpg /root/edgevision/data/calib_v2/real_001.jpg /root/edgevision/data/calib_v2/real_002.jpg /tmp/evaltest3/
/root/lab/eval_dump /root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn /tmp/evaltest3 > /tmp/preds3.txt 2>/dev/null
echo '--- top 5 per image ---'
for k in 0 1 2; do
  echo "img $k:"
  awk -v img=$k '$1==img' /tmp/preds3.txt | sort -k3 -rn | head -3
done
echo "total: $(wc -l < /tmp/preds3.txt)"
# 再跑 COCO128 一张(当年 mAP 0.60 的数据集)
COCO=$(ls /root/edgevision/data/coco128/images/train2017/*.jpg 2>/dev/null | head -1)
echo "coco: $COCO"
if [ -n "$COCO" ]; then
  mkdir -p /tmp/evaltest4 && cp "$COCO" /tmp/evaltest4/
  /root/lab/eval_dump /root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn /tmp/evaltest4 > /tmp/preds4.txt 2>/dev/null
  echo '--- coco top 3 ---'
  sort -k3 -rn /tmp/preds4.txt | head -3
fi
