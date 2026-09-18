#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

if ! command -v aarch64-linux-gnu-g++ &>/dev/null; then
    echo "[-] Error: aarch64-linux-gnu-g++ cross compiler not found on PATH."
    echo "    To install: sudo apt-get update && sudo apt-get install -y g++-aarch64-linux-gnu gcc-aarch64-linux-gnu"
    exit 1
fi

mkdir -p build_aarch64 && cd build_aarch64
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain/aarch64-toolchain.cmake -DCROSS_COMPILE_RK3588=ON
make -j$(nproc)
echo "[+] RK3588 aarch64 Cross-compilation completed: ${SCRIPT_DIR}/build_aarch64/edgevision_rknn"
