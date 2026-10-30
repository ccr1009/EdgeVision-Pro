#!/bin/bash
# ============================================================================
#  voice_start.sh —— 语音网关启停脚本
# ============================================================================
#  用法：
#    bash /root/voice_start.sh                 停止 gateway，独占 NPU 启动（Agent 响应快）
#    bash /root/voice_start.sh --keep-gateway  保留 gateway 启动（NPU 争用，响应慢）
#    bash /root/voice_start.sh --stop          停止语音网关，恢复 gateway
#    bash /root/voice_start.sh --status        查看运行状态
#
#  为什么默认要停 gateway：
#    实测 gateway(:9000) 占着 NPU 时，Agent 单次回复要 39 秒；独占后降到 5 秒左右。
# ============================================================================

set -u

VGW=/root/voice_gateway
LOG=/tmp/vgw.log
ASR_LOG=/tmp/vgw_agent.log
TTS_DAEMON=/root/tts_daemon
TTS_LOG=/tmp/tts_daemon.log
TTS_SOCK=/tmp/tts_daemon.sock

# pkill -f 会匹配到调用者自身的命令行，这里用变量拼接规避自匹配。
# 注意：**不要加 $ 锚定**。ASR 子进程的命令行是
#   ".../sherpa-onnx-vad-alsa-offline-asr --silero-vad-model=... hw:0,0"
# 末尾是 hw:0,0 而不是程序名，加 $ 永远匹配不上 —— 会漏杀，
# 孤儿进程继续霸占麦克风，新实例全部 exit 255（实测踩过）。
PW="voice_gatewa"
AW="rkllm_embodied_agen"
RW="vad-alsa-offline-as"

# 兜底：谁占着麦克风就杀谁（比按名字匹配更可靠）
free_mic() {
    local p
    p=$(fuser /dev/snd/pcmC0D0c 2>/dev/null | tr -d ' ')
    if [ -n "${p:-}" ]; then
        echo "    释放麦克风（占用者 pid=$p）"
        kill -9 "$p" 2>/dev/null
        sleep 2
    fi
}

stop_children() {
    local p
    for p in $(pgrep -f "$PW" 2>/dev/null); do kill "$p" 2>/dev/null; done
    sleep 3
    for p in $(pgrep -f "$PW" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
    for p in $(pgrep -f "$AW" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
    for p in $(pgrep -f "$RW" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
    for p in $(pgrep -f "tts_daemo" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
    rm -f "$TTS_SOCK"
    free_mic
    sleep 2
}

# 启动常驻 TTS 服务：省掉每次合成前 3.75 秒的 ONNX session 构建
start_tts_daemon() {
    if [ ! -x "$TTS_DAEMON" ]; then
        echo "    (未找到 $TTS_DAEMON，语音播报将走 fork/exec 降级路径)"
        return
    fi
    rm -f "$TTS_SOCK"
    nohup "$TTS_DAEMON" > "$TTS_LOG" 2>&1 < /dev/null &
    local i
    for i in $(seq 1 20); do
        [ -S "$TTS_SOCK" ] && break
        sleep 1
    done
    if [ -S "$TTS_SOCK" ]; then
        echo "    ✓ TTS daemon 就绪（socket=$TTS_SOCK）"
    else
        echo "    ! TTS daemon 未就绪，将走降级路径（每段多 3.7s）"
        tail -3 "$TTS_LOG" 2>/dev/null | sed 's/^/      /'
    fi
}

status() {
    echo "voice_gateway : $(pgrep -cf "$PW" 2>/dev/null || echo 0) 个进程"
    echo "Agent 子进程  : $(pgrep -cf "$AW" 2>/dev/null || echo 0) 个"
    echo "ASR   子进程  : $(pgrep -cf "$RW" 2>/dev/null || echo 0) 个"
    echo "edgevision    : $(systemctl is-active edgevision.service)"
    local m
    m=$(fuser /dev/snd/pcmC0D0c 2>/dev/null | tr -d ' ')
    echo "麦克风占用    : ${m:-空闲}"
    if [ -f "$LOG" ]; then
        echo "--- 日志最后 5 行 ---"
        tail -5 "$LOG"
    fi
}

case "${1:-}" in
    --stop)
        stop_children
        systemctl start edgevision.service 2>/dev/null
        sleep 2
        echo "已停止语音网关；edgevision = $(systemctl is-active edgevision.service)"
        exit 0
        ;;
    --status)
        status
        exit 0
        ;;
esac

if [ ! -x "$VGW" ]; then
    echo "找不到可执行文件 $VGW"
    exit 1
fi

KEEP=0
[ "${1:-}" = "--keep-gateway" ] && KEEP=1

if [ "$KEEP" -eq 0 ]; then
    systemctl stop edgevision.service 2>/dev/null
    echo "[1/3] 已停 edgevision.service（让 Agent 独占 NPU，响应更快）"
else
    echo "[1/3] 保留 edgevision.service（NPU 会争用，Agent 响应会慢）"
fi

stop_children
echo "[2/3] 已清理残留进程"

echo "[2.5] 启动 TTS 常驻服务（加载模型约 4 秒）..."
start_tts_daemon

rm -f "$LOG" "$ASR_LOG"
nohup "$VGW" > "$LOG" 2>&1 < /dev/null &

echo "[3/3] 启动中，等 42 秒（Agent 有约 30s 的模型预热，别在这期间说话）..."
sleep 42
cat "$LOG"

echo
echo "──────────────────────────────────────────────"
echo " 说话即可，它会把语音送进 Agent 并语音回答"
echo " 实时日志: tail -f $LOG"
echo " 止状态  : bash /root/voice_start.sh --status"
echo " 停止    : bash /root/voice_start.sh --stop"
echo "──────────────────────────────────────────────"
