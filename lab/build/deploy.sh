#!/bin/bash
# 一条命令上线：同步源码 → 板上编译 → （可选）重启服务
#
# 用法：
#   ./deploy.sh agent             # 只同步+编译 Agent
#   ./deploy.sh agent --restart   # 编译后重启语音网关（会等约 70s 就绪）
#   ./deploy.sh gateway           # 语音网关
#   ./deploy.sh gateway --restart
#
# 设计原则：**先编译成功再替换**。编译失败时旧二进制原样保留，
# 板子不会变成"改坏了不能跑"的状态——这是踩过 IOVA 泄漏、重启三次换来的习惯。

set -uo pipefail

HOST="${HOST:-rk3588}"
SSH="ssh -o LogLevel=ERROR $HOST"      # 屏蔽 KEX 警告，只留有用输出
SCP="scp -q -o LogLevel=ERROR"
# 本脚本在 lab/build/ 下，源码分在 lab/agent/ 与 lab/voice/
# 用 dirname 反推 lab/ 根，无论从哪里调用都不受当前目录影响
LAB="$(cd "$(dirname "$0")/.." && pwd)"

TARGET="${1:-}"
RESTART=0
[ "${2:-}" = "--restart" ] && RESTART=1

case "$TARGET" in
  agent)
    SRC="$LAB/agent/rkllm_embodied_agent.cpp"
    REMOTE_DIR="/root/rkllm_deploy"
    REMOTE_SRC="$REMOTE_DIR/rkllm_embodied_agent.cpp"
    BIN="$REMOTE_DIR/rkllm_embodied_agent_p0"
    CXXFLAGS="-O2"
    LINK="-L. -lrkllmrt -Wl,-rpath=."
    UNIT="voice-gateway"
    ;;
  gateway)
    SRC="$LAB/voice/voice_gateway.cpp"
    REMOTE_DIR="/root"
    REMOTE_SRC="/root/voice_gateway.cpp"
    BIN="/root/voice_gateway"
    CXXFLAGS="-O2 -std=c++17 -Wall -Wextra"
    LINK="-lpthread -lutil"
    UNIT="voice-gateway"
    ;;
  *)
    echo "用法: $0 {agent|gateway} [--restart]"; exit 2
    ;;
esac

[ -f "$SRC" ] || { echo "✗ 找不到源码: $SRC"; exit 1; }

echo "→ 同步 $(basename "$SRC") → $HOST:$REMOTE_SRC"
$SCP "$SRC" "$HOST:$REMOTE_DIR/__upload.tmp" || { echo "✗ 上传失败"; exit 1; }

echo "→ 板上编译（编译失败会保留旧二进制）"
$SSH "cd '$REMOTE_DIR' && \
  tr -d '\r' < __upload.tmp > __build.cpp && \
  g++ $CXXFLAGS __build.cpp $LINK -o __build.bin 2>&1" | head -20

if ! $SSH "[ -x '$REMOTE_DIR/__build.bin' ]"; then
  echo "✗ 编译失败，旧二进制未动"
  $SSH "rm -f '$REMOTE_DIR/__upload.tmp' '$REMOTE_DIR/__build.cpp'"
  exit 1
fi

$SSH "cd '$REMOTE_DIR' && mv -f __build.bin '$BIN' && mv -f __build.cpp '$REMOTE_SRC' && \
  rm -f __upload.tmp && echo \"✓ 已替换 \$(basename '$BIN') (\$(stat -c%s '$BIN') 字节)\""

if [ "$RESTART" = "1" ]; then
  echo "→ 重启 $UNIT（就绪约需 70s）"
  $SSH "systemctl restart $UNIT"
  for i in $(seq 1 45); do
    sleep 2
    if $SSH "journalctl -u $UNIT --no-pager -n 40 2>/dev/null | grep -q '麦克风已就绪'"; then
      echo "✓ $UNIT 已就绪"; break
    fi
    [ "$i" = "45" ] && echo "⚠ 等待超时，查: ssh $HOST 'journalctl -u $UNIT -n 30'"
  done
fi

echo "→ 提醒：跑一次自测（约 2~3 分钟，会自动腾让 NPU）"
echo "   ssh $HOST 'python3 /root/regress.py && python3 /root/explore.py'"
