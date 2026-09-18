#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

echo "=================================================================="
echo "EdgeVision-RK3588: Running Full Test Suite"
echo "=================================================================="

echo "\n[TEST 1/5] Environment Verification:"
bash scripts/test_env.sh

echo "\n[TEST 2/5] ByteTrack Unit Test:"
.venv/bin/python -m unittest track/test_tracker.py

echo "\n[TEST 3/5] YOLOv8 ONNX Verification:"
.venv/bin/python convert/export_onnx.py

echo "\n[TEST 4/5] x86 RKNN Simulator Precision Test:"
.venv/bin/python simulate/simulator_eval.py

echo "\n[TEST 5/5] C++ Pipeline x86 Execution Test:"
(cd deploy/cpp/build && ./edgevision_x86)

echo "\n=================================================================="
echo "[ALL TESTS PASSED] EdgeVision PC Simulation Suite Verified 100%!"
echo "=================================================================="
