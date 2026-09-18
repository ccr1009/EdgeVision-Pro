#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="${SCRIPT_DIR}/rknn_api"

mkdir -p "${TARGET_DIR}/include"
mkdir -p "${TARGET_DIR}/lib"

echo "=== Downloading Rockchip RK3588 librknn_api Headers & Libraries ==="
HEADER_URL="https://raw.githubusercontent.com/rockchip-linux/rknpu2/master/runtime/RK3588/Linux/librknn_api/include/rknn_api.h"
LIB_URL="https://raw.githubusercontent.com/rockchip-linux/rknpu2/master/runtime/RK3588/Linux/librknn_api/aarch64/librknnrt.so"

if [ ! -f "${TARGET_DIR}/include/rknn_api.h" ]; then
    echo "Downloading rknn_api.h..."
    curl -L "${HEADER_URL}" -o "${TARGET_DIR}/include/rknn_api.h"
fi

if [ ! -f "${TARGET_DIR}/lib/librknnrt.so" ]; then
    echo "Downloading aarch64 librknnrt.so..."
    curl -L "${LIB_URL}" -o "${TARGET_DIR}/lib/librknnrt.so"
fi

echo "[+] RKNN C/C++ libraries and headers successfully downloaded to ${TARGET_DIR}"
