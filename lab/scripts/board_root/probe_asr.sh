#!/bin/bash
# /root/probe_asr.sh —— 采集 ASR 真实输入样本（一次停网关，批量录音）
# 目的：搞清楚 VAD 到底在切什么、识别成什么，而不是靠猜
set -u
OUT=/root/asr_samples
mkdir -p $OUT

echo "[1/4] 停网关（释放麦克风）"
systemctl stop voice-gateway
sleep 3

amixer -c 2 cset numid=16 on >/dev/null 2>&1
amixer -c 2 sset PCM 200 >/dev/null 2>&1

echo "[2/4] 录 12 秒空场噪声"
timeout 14 arecord -D hw:0,0 -r 16000 -c 1 -f S16_LE -d 12 $OUT/ambient.wav >/dev/null 2>&1
echo "      ambient.wav $(stat -c%s $OUT/ambient.wav 2>/dev/null || echo 0) 字节"

echo "[3/4] 合成测试语音（走 TTS，不占麦克风）"
/root/voice_gateway --say "小瑞" >/dev/null 2>&1 && cp -f /tmp/vgw_spk.wav $OUT/wake.wav
/root/voice_gateway --say "温度多少" >/dev/null 2>&1 && cp -f /tmp/vgw_spk.wav $OUT/ask.wav
/root/voice_gateway --say "开启语音助手" >/dev/null 2>&1 && cp -f /tmp/vgw_spk.wav $OUT/cmd.wav
ls -la $OUT/*.wav | awk "{print \"      \" \$9, \$5}"

echo "[4/4] 声学回路录 3 轮（唤醒词+问题）"
for i in 1 2 3; do
  timeout 12 arecord -D hw:0,0 -r 16000 -c 1 -f S16_LE -d 9 $OUT/loop$i.wav >/dev/null 2>&1 &
  apid=$!
  sleep 1
  aplay -D hw:2,0 $OUT/wake.wav >/dev/null 2>&1
  sleep 1.5
  aplay -D hw:2,0 $OUT/ask.wav  >/dev/null 2>&1
  wait $apid
  echo "      loop$i.wav $(stat -c%s $OUT/loop$i.wav 2>/dev/null || echo 0) 字节"
  sleep 1
done

echo "[完成] 重新拉起网关"
systemctl start voice-gateway
echo "      gateway=$(systemctl is-active voice-gateway)"
