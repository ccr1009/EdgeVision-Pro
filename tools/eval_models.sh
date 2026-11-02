#!/bin/bash
# 批量对比：新校准集模型 vs 旧合成校准集模型（同 runtime / 同 toolkit / 同 ONNX）
cd /root/lab || exit 1
NEW=/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn
OLD=/root/edgevision/data/models/yolov8n_dflfree_oldcalib_rk3588_i8.rknn

printf "%-8s %-5s %-6s %-8s %s\n" TAG IMG CLS SCORE BOX
for i in 000 003 006 009 012 015 018 021 024; do
  img=/root/edgevision/data/calib_v2/real_$i.jpg
  for tag in NEW OLD; do
    if [ "$tag" = "NEW" ]; then M=$NEW; else M=$OLD; fi
    line=$(./test_accuracy "$M" "$img" dflfree 0.05 2>/dev/null | grep '#0')
    if [ -z "$line" ]; then
      printf "%-8s %-5s %-6s %-8s %s\n" "$tag" "$i" "-" "none" "-"
    else
      cls=$(echo "$line" | sed -n 's/.*cls=\([0-9]*\).*/\1/p')
      sc=$(echo "$line" | sed -n 's/.*score=\([0-9.]*\).*/\1/p')
      box=$(echo "$line" | sed -n 's/.*box=\(.*\)/\1/p')
      printf "%-8s %-5s %-6s %-8s %s\n" "$tag" "$i" "$cls" "$sc" "$box"
    fi
  done
done
