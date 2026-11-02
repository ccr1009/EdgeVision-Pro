#!/bin/bash
# ============================================================================
# /root/bench_voice.sh —— 语音链路自动化基准（"尺子"）
# ============================================================================
#  干什么：板子自己说 → 自己听 → 自己识别 → 自动比对 → 出命中率
#
#  用途：
#    bash bench_voice.sh                      # 测「小瑞小瑞」10 次
#    bash bench_voice.sh "温度多少" 20        # 测「温度多少」20 次
#    bash bench_voice.sh "小瑞" 10 4          # 录 4 秒
#
#  前置条件（脚本会自检）：
#    1. tts_daemon 在跑（否则降级 fork/exec，每轮多 3.7s）
#    2. voice_gateway 已停（它会独占麦克风）
#    3. Headphone Switch = on（ES8388 的 numid=16，默认可能是 off → 耳机无声）
#
#  输出的两种命中率：
#    严格：识别文本与目标完全一致
#    容错：把同音字（小锐/晓瑞/小睿/小蕊）归一后一致，或包含目标串
#          —— 软唤醒实际吃的是"容错"这一档，因为唤醒词本来就收同音字
# ============================================================================
set -u

TEXT="${1:-小瑞小瑞}"
N="${2:-10}"
DUR="${3:-5}"

VGW=/root/voice_gateway
MODEL=/root/asr/sherpa-onnx-paraformer-zh-small-2024-03-09
BIN=/root/asr/sherpa-onnx-v1.12.12-linux-aarch64-static/bin
WAV=/tmp/bench_rec.wav
MONO=/tmp/bench_rec_m.wav

# ---------------- 前置自检 ----------------
[ -S /tmp/tts_daemon.sock ] || { echo "✗ TTS daemon 未运行：/root/tts_daemon &"; exit 1; }
if pgrep -x voice_gateway >/dev/null 2>&1; then
    echo "✗ voice_gateway 在跑，会独占麦克风。先执行： bash /root/voice_start.sh --stop"
    exit 1
fi
if ! amixer -c 2 cget numid=16 2>/dev/null | grep -q "values=on"; then
    echo "⚠ Headphone Switch 是 off —— 耳机不会出声，测试必然全失败"
    echo "  修： amixer -c 2 cset numid=16 on"
    exit 1
fi
# 回路连通性：声音真的进了空气吗？
# 没有这一关，出现 0/10 时你无法区分"识别率差"和"根本没出声"——
# 2026-10-31 就在这儿栽过一次，见 loop_check.sh 头部说明。
if ! bash /root/loop_check.sh; then
    echo
    echo "✗ 回路不通，本轮不作数（不是失败，是没有测量）"
    exit 1
fi

echo "───────────────────────────────────────────────"
echo " 目标文本 : $TEXT"
echo " 次数     : $N    每次录音 $DUR 秒"
echo "───────────────────────────────────────────────"

exact=0; near=0; fail=0

for i in $(seq 1 "$N"); do
    # 播放放后台，录音立刻开始 —— 必须并行，
    # 否则「合成 1s + 播放 2s」会在录音开始前就播完（这个时序坑踩过）
    ( "$VGW" --say "$TEXT" >/dev/null 2>&1 ) &
    arecord -D plughw:0,0 -f S16_LE -r 16000 -c 2 -d "$DUR" "$WAV" 2>/dev/null
    wait 2>/dev/null

    sox "$WAV" -c 1 "$MONO" 2>/dev/null
    rec=$("$BIN/sherpa-onnx-offline" \
              --tokens="$MODEL/tokens.txt" \
              --paraformer="$MODEL/model.int8.onnx" \
              --num-threads=2 "$MONO" 2>&1 \
          | grep -oE '"text": "[^"]*"' | sed 's/^"text": "//; s/"$//')

    # 同音字归一（和 voice_gateway 里 kWakeWords 保持一致）
    norm=$(printf '%s' "$rec" | sed 's/小锐/小瑞/g; s/晓瑞/小瑞/g; s/小睿/小瑞/g; s/小蕊/小瑞/g')

    if [ "$rec" = "$TEXT" ]; then
        exact=$((exact+1)); mark="✓ 完全"
    elif [ "$norm" = "$TEXT" ]; then
        near=$((near+1));  mark="~ 同音"
    elif printf '%s' "$norm" | grep -qF "$TEXT"; then
        near=$((near+1));  mark="~ 包含"
    else
        fail=$((fail+1));  mark="✗ 失败"
    fi

    printf "[%2d/%d] %s  识别=「%s」\n" "$i" "$N" "$mark" "$rec"
    sleep 1
done

echo "───────────────────────────────────────────────"
awk -v e="$exact" -v n="$near" -v f="$fail" -v t="$N" 'BEGIN{
    printf " 严格 %d / 容错 %d / 失败 %d   (共 %d)\n", e, n, f, t
    printf " 严格命中率 %.0f%%   容错命中率 %.0f%%\n", e*100/t, (e+n)*100/t
}'
echo "───────────────────────────────────────────────"
