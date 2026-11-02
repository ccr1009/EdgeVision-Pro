#!/bin/bash
# ============================================================================
# /root/test_kws.sh  —— 唤醒词「小瑞小瑞」手动实测
# ============================================================================
#  用法：
#    bash /root/test_kws.sh               # 默认设备 plughw:0,0
#    bash /root/test_kws.sh hw:0,0        # 直连硬件
#
#  连喊 10 次「小瑞小瑞」，每次间隔约 2 秒，然后 Ctrl+C。
#  退出后统计命中：
#    grep -c '"keyword"' /tmp/kws_test.log
#
#  版本组合（重要，踩过坑）：
#    运行时必须用 v1.11.0，不能用主目录里的 v1.12.12。
#    v1.12.12 的 KWS runtime 要求 encoder 带 `attention_dims` 元数据，
#    而官方所有 KWS 模型（含 2025-12 最新版）都没导出该字段 →
#    报 'attention_dims' does not exist in the metadata 后 exit 255。
#    v1.10.30 反而要 GLIBC_2.33，板上是 2.31，跑不起来。
#    v1.11.0 是唯一同时满足「GLIBC≤2.31」和「不要求 attention_dims」的版本。
#
#  ！！！ 会独占麦克风 hw:0,0 ！！！ 运行前必须停掉 voice_gateway
# ============================================================================
set -u

KWS_BIN=/root/asr/kws110          # v1.11.0 的 keyword-spotter-alsa，单文件
KWS_DIR=/root/asr/kws
DEV="${1:-plughw:0,0}"
LOG=/tmp/kws_test.log

if pgrep -x voice_gateway >/dev/null 2>&1 || pgrep -x sherpa-onnx-vad >/dev/null 2>&1; then
    echo "[ABORT] voice_gateway 正在运行，麦克风被占用。" >&2
    echo "        先执行： bash /root/voice_start.sh --stop" >&2
    exit 1
fi

if [ ! -x "$KWS_BIN" ]; then
    echo "[ABORT] 找不到 $KWS_BIN" >&2
    exit 1
fi

echo "───────────────────────────────────────────────"
echo " 唤醒词：小瑞小瑞（x iǎo r uì x iǎo r uì）"
echo " 设备  ：$DEV"
echo " 请连续喊 10 次，每次间隔约 2 秒"
echo " 喊完按 Ctrl+C，会自动打印命中统计"
echo "───────────────────────────────────────────────"

# Ctrl+C 会杀掉整条管道，统计必须靠 trap 兜住，否则最后那几行打不出来
report() {
    echo
    echo "───────────────────────────────────────────────"
    echo " 命中次数：$(grep -c '"keyword"' "$LOG" 2>/dev/null || echo 0)"
    echo " 完整日志：$LOG"
    echo "───────────────────────────────────────────────"
    exit 0
}
trap report INT TERM

"$KWS_BIN" \
    --tokens="$KWS_DIR/tokens.txt" \
    --encoder="$KWS_DIR/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx" \
    --decoder="$KWS_DIR/decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx" \
    --joiner="$KWS_DIR/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx" \
    --keywords-file="$KWS_DIR/keywords.txt" \
    --keywords-score=1.0 \
    --keywords-threshold=0.25 \
    --provider=cpu \
    --num-threads=2 \
    "$DEV" 2>&1 | tee "$LOG" | tr '\r' '\n' | grep --line-buffered -o '"keyword": "[^"]*"'

echo
echo "───────────────────────────────────────────────"
echo " 命中次数：$(grep -c '"keyword"' "$LOG" 2>/dev/null || echo 0) / 10"
echo " 完整日志：$LOG"
echo "───────────────────────────────────────────────"
