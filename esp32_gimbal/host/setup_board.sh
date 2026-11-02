#!/usr/bin/env bash
# setup_board.sh — 在 RK3588 上一次装好云台追踪的环境并做自检
#
# 用法（在板子上）：
#   bash setup_board.sh              # 装依赖 + 自检
#   bash setup_board.sh --run        # 装依赖 + 自检 + 起网关 + 跑追踪
#
# 为什么要有这个脚本：板子上 Python 3.9 系统环境没有 pip、没有 pyserial，
# 但项目里已有 /root/rknn_env（带 pip 24.3.1）可以直接复用，
# 不用另起一个 venv 再去解压 pip wheel（那是上次踩过的坑）。

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_PY="/root/rknn_env/bin/python"
PORT="${GIMBAL_PORT:-}"
DO_RUN=0

for arg in "$@"; do
    case "$arg" in
        --run) DO_RUN=1 ;;
        --port) shift ;;
        /dev/*|COM*) PORT="$arg" ;;
    esac
done

echo "================================================================"
echo "ESP32 云台追踪 — 板端环境部署"
echo "================================================================"

# ---------- 1. 选 Python 环境 ----------
echo ""
echo "[1/5] 检查 Python 与 pyserial"
if [ -x "$VENV_PY" ]; then
    PY="$VENV_PY"
    echo "      复用 /root/rknn_env（避免重装 pip）"
else
    PY="python3"
    echo "      /root/rknn_env 不存在，回退系统 python3"
fi

if ! "$PY" -c "import serial" 2>/dev/null; then
    echo "      pyserial 未安装，尝试安装 ..."
    if "$PY" -m pip install pyserial >/tmp/gimbal_pip.log 2>&1; then
        echo "      安装成功"
    else
        echo "      [FAIL] 安装失败，日志尾部："
        tail -5 /tmp/gimbal_pip.log
        echo "      手动处理：$PY -m pip install pyserial"
        exit 1
    fi
fi
"$PY" -c "import serial; print('      pyserial', serial.__version__, 'OK')"

# ---------- 2. 逻辑自测 ----------
echo ""
echo "[2/5] 闭环逻辑自测（18 项，不需要硬件）"
if "$PY" "$HERE/test_loopback.py" >/tmp/gimbal_logic.log 2>&1; then
    grep -E "自测全部通过|❌" /tmp/gimbal_logic.log | sed 's/^/      /'
else
    echo "      [FAIL] 逻辑自测未通过："
    tail -20 /tmp/gimbal_logic.log | sed 's/^/      /'
    exit 1
fi

# ---------- 3. 找 ESP32 串口 ----------
echo ""
echo "[3/5] 查找 ESP32 串口"
if [ -z "$PORT" ]; then
    for p in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0 /dev/ttyUSB1; do
        [ -e "$p" ] && PORT="$p" && break
    done
fi

if [ -z "$PORT" ]; then
    echo "      [未找到] /dev/ttyACM* 和 /dev/ttyUSB* 都不存在"
    echo ""
    echo "      请确认：ESP32 的 USB 口已插到 EVM3588-B 的 USB2.0 HOST 口。"
    echo "      插上后重新跑本脚本，或用 GIMBAL_PORT=/dev/ttyACM0 bash setup_board.sh"
    echo ""
    echo "      注意：ESP32-S3 N16R8 用原生 USB-Serial-JTAG，插上应直接出现 ttyACM0，"
    echo "            不需要 CH340 驱动。若插上没有，先 lsusb 看有没有 VID 303A。"
    HAS_PORT=0
else
    echo "      找到串口: $PORT"
    ls -l "$PORT" | sed 's/^/      /'
    HAS_PORT=1
fi

# ---------- 4. 视觉网关 ----------
echo ""
echo "[4/5] 视觉网关（9000 端口）"
GW_STATE=$(systemctl is-active edgevision 2>/dev/null || echo inactive)
echo "      edgevision.service: $GW_STATE"

if [ "$GW_STATE" != "active" ]; then
    echo "      正在启动 ..."
    systemctl start edgevision
    sleep 8
    GW_STATE=$(systemctl is-active edgevision 2>/dev/null || echo inactive)
    echo "      启动后: $GW_STATE"
fi

if [ "$GW_STATE" = "active" ]; then
    echo "      核对真实帧格式与坐标口径："
    "$PY" "$HERE/check_gateway.py" --frames 8 2>&1 | tail -20 | sed 's/^/      /'
else
    echo "      [WARN] 网关没起来，追踪会因为拿不到帧而空转"
fi

# ---------- 5. 云台连通性 ----------
echo ""
echo "[5/5] ESP32 云台连通性"
if [ "$HAS_PORT" = "1" ]; then
    "$PY" "$HERE/gimbal_probe.py" --port "$PORT" 2>&1 | tail -22 | sed 's/^/      /'
else
    echo "      (跳过 —— 没有串口)"
fi

echo ""
echo "================================================================"
if [ "$HAS_PORT" = "1" ]; then
    echo "环境就绪 ✅"
    echo ""
    echo "跑追踪："
    echo "  $PY $HERE/gimbal_tracker.py --port $PORT --class person --img-w 1280 --img-h 720"
    echo ""
    echo "调参建议：先加 --dry-run 只算不发，确认角度方向对了再真正驱动舵机。"
    if [ "$DO_RUN" = "1" ]; then
        echo ""
        echo "================================================================"
        echo "启动追踪（Ctrl-C 结束）"
        echo "================================================================"
        exec "$PY" "$HERE/gimbal_tracker.py" --port "$PORT" \
             --class person --img-w 1280 --img-h 720
    fi
else
    echo "还差一步：把 ESP32 插到板子的 USB 口 ⚠️"
fi
echo "================================================================"
