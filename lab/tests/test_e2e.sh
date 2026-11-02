#!/bin/bash
# ============================================================================
# /root/test_e2e.sh —— 端到端语音链路自测（全程无人）
# ============================================================================
#  做的事：板子用扬声器说「小瑞」→ 自己麦克风听到 → 唤醒
#          → 再说「温度多少」→ 走快速通道回答 → 我读日志判断对不对
#
#  这是「声学回路」的真正用法：不需要人喊，机器自己喂自己。
#
#  前置：TTS daemon 需在跑（否则每句多 3.7s 合成）
#  用法：bash test_e2e.sh
# ============================================================================
set -u

WAKE_WAV=/tmp/e2e_wake.wav
ASK_WAV=/tmp/e2e_ask.wav
LOG=/tmp/vgw.log

say_to_file() {
    # --say 会顺便播报一次（无害），同时把合成结果写进 /tmp/vgw_spk.wav
    /root/voice_gateway --say "$1" >/dev/null 2>&1
    if [ -f /tmp/vgw_spk.wav ]; then
        cp /tmp/vgw_spk.wav "$2"
        return 0
    fi
    return 1
}

echo "════════ 端到端语音自测 ════════"

# ---- 0. 前置检查 ----
[ -S /tmp/tts_daemon.sock ] || { echo "✗ TTS daemon 没跑"; exit 1; }
# ES8388 的耳机开关会被驱动重置成 off，直接强制打开（幂等）
amixer -c 2 cset numid=16 on  >/dev/null 2>&1
amixer -c 2 sset PCM 200      >/dev/null 2>&1

# 回路连通性自检：这一步不过，后面全是假数字。
# 耳机插着时声音进耳机不进空气，麦克风只收到底噪，
# 端到端会"看起来失败"，其实是没有测量。
if ! bash /root/loop_check.sh; then
    echo "✗ 声学回路不通，端到端自测无法进行"
    exit 1
fi

# ---- 1. 预生成两段测试音频 ----
echo "[1/5] 合成测试音频..."
say_to_file "小瑞"     "$WAKE_WAV" || { echo "✗ 合成唤醒词失败"; exit 1; }
say_to_file "温度多少" "$ASK_WAV"  || { echo "✗ 合成问题失败"; exit 1; }
echo "      唤醒词 $(stat -c%s "$WAKE_WAV") 字节 / 问题 $(stat -c%s "$ASK_WAV") 字节"

# ---- 2. 启动完整链路 ----
echo "[2/5] 启动语音网关（约 55s，含 Agent 预热）..."
rm -f "$LOG"
setsid bash /root/voice_start.sh > /tmp/e2e_start.log 2>&1 < /dev/null &
for i in $(seq 1 70); do
    sleep 1
    grep -q "开始监听" "$LOG" 2>/dev/null && break
done
if ! grep -q "开始监听" "$LOG" 2>/dev/null; then
    echo "✗ 网关未就绪，日志："; tail -5 "$LOG"; exit 1
fi
echo "      ✓ 已就绪，等 ASR 加载（10s）..."
sleep 10

# ---- 3. 播唤醒词 ----
echo "[3/5] 播放唤醒词「小瑞」..."
aplay -D hw:2,0 "$WAKE_WAV" >/dev/null 2>&1
sleep 10

# ---- 4. 播问题 ----
echo "[4/5] 播放问题「温度多少」..."
aplay -D hw:2,0 "$ASK_WAV" >/dev/null 2>&1
sleep 12    # 快速通道应该 1s 内答完；留足余量抓日志

# ---- 5. 判定 ----
echo "[5/5] 从日志判定结果："
echo "───────────────────────────────────────"

woke=0; answered=0
grep -q "已唤醒" "$LOG" && woke=1
grep -qE "温度|度" "$LOG" && answered=1

echo "① 唤醒：$([ $woke -eq 1 ] && echo '✓ 命中' || echo '✗ 没反应')"
grep -E "👤|🔔 已唤醒|快速通道" "$LOG" | tail -4 | sed 's/^/   /'

echo "② 回答：$([ $answered -eq 1 ] && echo '✓ 已作答' || echo '✗ 无回答')"
grep -E "🤖|温度|度" "$LOG" | tail -4 | sed 's/^/   /'

echo "───────────────────────────────────────"

# 时序统计：从「已唤醒」到「回答」隔了几行/几秒
if [ $woke -eq 1 ] && [ $answered -eq 1 ]; then
    echo "✅ 端到端链路通了：唤醒 → 识别 → 快速通道 → 播报"
else
    echo "⚠ 链路有问题，完整日志：$LOG"
fi
