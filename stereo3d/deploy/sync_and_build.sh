#!/usr/bin/env bash
# ============================================================================
#  sync_and_build.sh —— 把本机 stereo3d-rk3588 同步到板上并编译
#  在 PC 的 PowerShell 里跑：
#     bash -lc "./deploy/sync_and_build.sh"        # 需 git-bash / WSL
#  或者直接：
#     python deploy/sync_and_build.py
#
#  为什么在板上原生编译？
#    PC 可能是 Ubuntu 22 (glibc 2.35)，板子是 Debian 11 (glibc 2.31)，
#    交叉编译的二进制在板子上会报 GLIBC_2.34 not found。
#    板子 8 核，cmake+make 编译这个项目不到 1 分钟，不值得折腾交叉工具链。
# ============================================================================
set -euo pipefail

BOARD="${BOARD:?请先设置 BOARD=root@<board-ip>（板子内网 IP 不入库）}"
REMOTE_DIR="${REMOTE_DIR:-/root/stereo3d}"
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> 本地: $LOCAL_DIR"
echo "==> 板端: $BOARD:$REMOTE_DIR"

# 1) 只同步源码（不传 build/ captures/ 大文件）
ssh "$BOARD" "mkdir -p $REMOTE_DIR/{include,src,tools,calib,captures,models}"
echo "==> 同步 include/ src/ tools/ CMakeLists.txt"
scp -q -r "$LOCAL_DIR/include" "$BOARD:$REMOTE_DIR/"
scp -q -r "$LOCAL_DIR/src"     "$BOARD:$REMOTE_DIR/"
scp -q -r "$LOCAL_DIR/tools"   "$BOARD:$REMOTE_DIR/" 2>/dev/null || true
scp -q    "$LOCAL_DIR/CMakeLists.txt" "$BOARD:$REMOTE_DIR/"

# 2) 编译
echo "==> 板上原生编译"
ssh "$BOARD" "cd $REMOTE_DIR && \
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /tmp/cmake.log 2>&1 || (tail -40 /tmp/cmake.log; exit 1) && \
  cmake --build build -j\$(nproc) 2>&1 | tail -40"

echo "==> 完成：$BOARD:$REMOTE_DIR/build/stereo3d_demo"
