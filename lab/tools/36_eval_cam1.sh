#!/bin/bash
# cam1_focused.jpg 用 conf=0.01 口径验证
ls -la /root/edgevision/data/calib_v2/*.jpg 2>/dev/null | head -3
CAM1=$(find /root -maxdepth 4 -name "cam1_focused.jpg" 2>/dev/null | head -1)
echo "cam1_focused: $CAM1"
if [ -n "$CAM1" ]; then
  mkdir -p /tmp/evaltest2 && cp "$CAM1" /tmp/evaltest2/
  /root/lab/eval_dump /root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn /tmp/evaltest2 > /tmp/preds2.txt 2>/dev/null
  echo '--- top scores ---'
  sort -k3 -rn /tmp/preds2.txt | head -5
  echo "total lines: $(wc -l < /tmp/preds2.txt)"
fi
