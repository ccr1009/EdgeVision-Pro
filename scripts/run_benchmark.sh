#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

echo "=================================================================="
echo "EdgeVision-RK3588: Running Comprehensive Benchmarks"
echo "=================================================================="

echo "--> [1/2] Running Precision Benchmark (ONNX FP32 vs RKNN INT8 Simulator)..."
.venv/bin/python simulate/simulator_eval.py

echo "--> [2/2] Running Multi-Threaded Pipeline Latency Breakdown..."
.venv/bin/python pipeline/demo_pipeline.py \
    --video data/videos/test_surveillance.mp4 \
    --backend onnx \
    --output outputs/benchmark_annotated.mp4 \
    --log outputs/benchmark_events.log

echo "[+] Benchmarks completed! Review docs/BENCHMARKS.md for full specs."
