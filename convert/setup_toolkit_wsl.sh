#!/bin/bash
# ==============================================================================
# 在 WSL(Ubuntu) 里准备 rknn-toolkit2 转换环境
# ------------------------------------------------------------------------------
# 为什么必须这样：
#   rknn-toolkit2 只有 Linux x86_64 的 wheel，且版本绑死 Python 3.10（cp310）。
#   WSL 里自带的 python 是 3.14，装不上，所以用 uv 拉一个独立的 3.10。
# 为什么把文件拷进 WSL 家目录：
#   项目路径含中文（/mnt/d/a研究生/...），toolkit 读数据集清单时容易出问题。
# 用法（在 WSL 里）:
#   bash /mnt/d/a研究生/项目/rk3588项目/edgevision-rk3588/convert/setup_toolkit_wsl.sh
# ==============================================================================
set -euo pipefail

WIN_PROJ="/mnt/d/a研究生/项目/rk3588项目/edgevision-rk3588"
WORK="$HOME/edgevision_rknn"
ENV_DIR="$WORK/env"
WHL="$WIN_PROJ/3rdparty/wheels/rknn_toolkit2-1.6.0+81f21f4d-cp310-cp310-linux_x86_64.whl"

echo "=== [1/5] 目录准备 ==="
mkdir -p "$WORK/models" "$WORK/calib" "$WORK/envs"
echo "    $WORK"

echo "=== [2/5] 拷贝模型与校准集（避开中文路径） ==="
cp -v "$WIN_PROJ/data/models/yolov8n_dflfree.onnx" "$WORK/models/" 2>/dev/null || echo "    [!] dflfree onnx 还没生成"
cp -v "$WIN_PROJ/data/models/yolov8n.onnx" "$WORK/models/" 2>/dev/null || true
rm -rf "$WORK/calib" && mkdir -p "$WORK/calib"
cp "$WIN_PROJ/data/calib_v2/"*.jpg "$WORK/calib/" 2>/dev/null || echo "    [!] calib_v2 为空"
ls "$WORK/calib"/*.jpg 2>/dev/null | sort > "$WORK/calib/dataset.txt"
echo "    校准图: $(wc -l < "$WORK/calib/dataset.txt") 张"

echo "=== [3/5] 准备 Python 3.10（uv） ==="
if ! command -v uv >/dev/null 2>&1; then
  echo "    安装 uv ..."
  curl -LsSf https://astral.sh/uv/install.sh | sh || {
      echo "    [!] uv 官方源失败，改用 pip 清华源"
      python3 -m pip install --user -i https://pypi.tuna.tsinghua.edu.cn/simple uv
  }
  export PATH="$HOME/.local/bin:$PATH"
fi
command -v uv || { echo "[-] uv 安装失败"; exit 1; }

echo "=== [4/5] 建 3.10 虚拟环境并装 toolkit ==="
if [ ! -x "$ENV_DIR/bin/python" ]; then
  uv venv --python 3.10 "$ENV_DIR"
fi
"$ENV_DIR/bin/python" -V
if ! "$ENV_DIR/bin/python" -c "import rknn" 2>/dev/null; then
  # 老版本 toolkit 依赖这些固定版本，numpy>=2 会直接崩
  "$ENV_DIR/bin/pip" install -q "numpy<2" "protobuf==3.20.3" "onnx==1.14.0" "onnxruntime==1.15.1" \
      "opencv-python-headless" 2>&1 | tail -3 || true
  "$ENV_DIR/bin/pip" install "$WHL" 2>&1 | tail -5
fi

echo "=== [5/5] 验证 ==="
"$ENV_DIR/bin/python" - <<'PY'
from rknn.api import RKNN
r = RKNN()
print("rknn-toolkit2 version:", getattr(r, 'version', 'unknown'))
r.release()
PY

cat <<EOF

[SUCCESS] 环境就绪
  工作目录 : $WORK
  模型     : $WORK/models/
  校准集   : $WORK/calib/dataset.txt
  虚拟环境 : $ENV_DIR

下一步（在 WSL 里跑转换）:
  cd "$WIN_PROJ"
  $ENV_DIR/bin/python convert/rknn_convert_v2.py \\
      --onnx "$WORK/models/yolov8n_dflfree.onnx" \\
      --calib "$WORK/calib/dataset.txt" \\
      --tag dflfree --fp16-too
EOF
