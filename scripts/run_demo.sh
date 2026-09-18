#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

echo "=================================================================="
echo "EdgeVision-RK3588: Running End-to-End Multi-Threaded Demo"
echo "=================================================================="

# Ensure test video exists
if [ ! -f "data/videos/test_surveillance.mp4" ]; then
    echo "[*] Test video not found, generating now..."
    bash scripts/download_assets.sh
fi

# Ensure ONNX model exists
if [ ! -f "data/models/yolov8n.onnx" ]; then
    echo "[*] ONNX model not found, exporting now..."
    .venv/bin/python convert/export_onnx.py
fi

# Run Demo Pipeline
.venv/bin/python pipeline/demo_pipeline.py \
    --video data/videos/test_surveillance.mp4 \
    --backend onnx \
    --model data/models/yolov8n.onnx \
    --output outputs/demo_annotated.mp4 \
    --log outputs/events.log

echo "=================================================================="
echo "[SUCCESS] Demo completed successfully!"
echo "Output Video:    ${PROJECT_ROOT}/outputs/demo_annotated.mp4"
echo "Event Alert Log: ${PROJECT_ROOT}/outputs/events.log"
echo "=================================================================="
