#!/bin/bash
# ==============================================================================
# 采集真实场景校准图（双 USB 摄像头）
# 背景：原校准集是从「合成色块动画视频」抽帧的，与真实摄像头分布严重偏离，
#       这是 INT8 PTQ 掉点的主因之一。本脚本从两块真实 UVC 摄像头抓图。
# 用法: bash /root/lab/collect_calib.sh [每路张数]
# ==============================================================================
set -u
N=${1:-25}
OUT=/root/edgevision/data/calib_real
mkdir -p "$OUT"
echo "[*] 输出目录: $OUT  每路 $N 张"

shoot() {
  dev=$1; tag=$2
  # 清空旧图
  rm -f "$OUT"/${tag}_*.jpg
  v4l2-ctl -d "$dev" --set-fmt-video=width=1280,height=720,pixelformat=MJPG >/dev/null 2>&1
  ok=0
  for n in $(seq 0 $((N-1))); do
    f="$OUT/${tag}_$(printf %03d "$n").jpg"
    # 1 帧直存（MJPG 本身是 JPEG，无需解码）
    if v4l2-ctl -d "$dev" --stream-mmap --stream-count=1 --stream-to="$f" >/dev/null 2>&1; then
      sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
      if [ "$sz" -gt 3000 ]; then ok=$((ok+1)); else rm -f "$f"; fi
    fi
    sleep 0.35
  done
  echo "[+] $tag ($dev): 成功 $ok / $N 张"
}

shoot /dev/video81 cam1
shoot /dev/video83 cam2

echo "[*] 总计: $(ls -1 "$OUT"/*.jpg 2>/dev/null | wc -l) 张"
ls -la "$OUT" | head -5
