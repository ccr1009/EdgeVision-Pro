#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

mkdir -p build && cd build
cmake .. -DCROSS_COMPILE_RK3588=OFF
make -j$(nproc)
echo "[+] x86 Build completed: ${SCRIPT_DIR}/build/edgevision_x86"
