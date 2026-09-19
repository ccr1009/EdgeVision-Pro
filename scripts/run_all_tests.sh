#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

echo "=================================================================="
echo "EdgeVision-RK3588 Pro: Running Comprehensive Test Suite"
echo "=================================================================="

echo -e "
[TEST 1/7] Environment Verification:"
bash scripts/test_env.sh

echo -e "
[TEST 2/7] ByteTrack Target Association Test:"
.venv/bin/python -m unittest track/test_tracker.py

echo -e "
[TEST 3/7] BEV Spatial Perception & 3D Box Geometry Test:"
PYTHONPATH=. .venv/bin/python tests/test_bev.py

echo -e "
[TEST 4/7] Multi-Stream Gateway & Zero-Copy NPU Scheduler Test:"
PYTHONPATH=. .venv/bin/python tests/test_gateway.py

echo -e "
[TEST 5/7] YOLOv8 ONNX Export & Graph Verification:"
.venv/bin/python convert/export_onnx.py

echo -e "
[TEST 6/7] x86 RKNN Simulator INT8 Precision Test:"
.venv/bin/python simulate/simulator_eval.py

echo -e "
[TEST 7/7] C++ Pro Multi-Channel Zero-Copy & BEV Engine Execution:"
(cd deploy/cpp/build && ./edgevision_pro)

echo -e "
=================================================================="
echo "[ALL 7/7 TESTS PASSED] EdgeVision Pro Suite Verified 100%!"
echo "=================================================================="
