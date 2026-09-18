#!/usr/bin/env bash
# ==============================================================================
# EdgeVision-RK3588: Environment Setup Script (WSL2 / Ubuntu 22.04+ / Python 3.10)
# ==============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

echo "=== [1/5] Checking uv package manager ==="
UV_BIN="uv"
if ! command -v uv &>/dev/null; then
    UV_BIN="${HOME}/.local/bin/uv"
fi
if [ ! -f "${UV_BIN}" ] && ! command -v uv &>/dev/null; then
    echo "Installing uv..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
    UV_BIN="${HOME}/.local/bin/uv"
fi

echo "=== [2/5] Initializing Python 3.10 Virtualenv (.venv) ==="
if [ ! -d ".venv" ]; then
    "${UV_BIN}" venv --python 3.10 .venv
fi

echo "=== [3/5] Installing PyTorch (CPU) and Core Dependencies ==="
"${UV_BIN}" pip install --python .venv/bin/python \
    --index-url https://download.pytorch.org/whl/cpu \
    torch torchvision

echo "=== [4/5] Installing Pinned Dependencies from requirements.txt ==="
"${UV_BIN}" pip install --python .venv/bin/python -r requirements.txt

echo "=== [5/5] Installing Rockchip RKNN-Toolkit2 (Official Wheel) ==="
RKNN_WHL_NAME="rknn_toolkit2-1.6.0+81f21f4d-cp310-cp310-linux_x86_64.whl"
RKNN_WHL_URL="https://raw.githubusercontent.com/rockchip-linux/rknn-toolkit2/master/rknn-toolkit2/packages/${RKNN_WHL_NAME}"
RKNN_WHL_DIR="${PROJECT_ROOT}/3rdparty/wheels"
mkdir -p "${RKNN_WHL_DIR}"

if [ ! -f "${RKNN_WHL_DIR}/${RKNN_WHL_NAME}" ]; then
    echo "Downloading ${RKNN_WHL_NAME}..."
    curl -L "${RKNN_WHL_URL}" -o "${RKNN_WHL_DIR}/${RKNN_WHL_NAME}"
fi

echo "Installing ${RKNN_WHL_NAME} (no-deps)..."
"${UV_BIN}" pip install --python .venv/bin/python --no-deps "${RKNN_WHL_DIR}/${RKNN_WHL_NAME}"

echo "Patching onnxruntime execstack if needed..."
if [ -f ".venv/bin/patchelf" ]; then
    find .venv/lib/python3.10/site-packages/onnxruntime/ -name "*.so*" -exec .venv/bin/patchelf --clear-execstack {} + 2>/dev/null || true
fi

echo "Running environment verification..."
bash scripts/test_env.sh
