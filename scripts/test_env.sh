#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}/.."

.venv/bin/python -c "
import rknn.api
from rknn.api import RKNN
import cv2
import onnxruntime
import torch
import scipy
import numpy as np

print('=============================================')
print('[SUCCESS] All Core AI & Edge Vision Libraries Verified!')
print(f'NumPy version:       {np.__version__}')
print(f'PyTorch version:     {torch.__version__}')
print(f'ONNXRuntime version: {onnxruntime.__version__}')
print(f'OpenCV version:      {cv2.__version__}')
print(f'SciPy version:       {scipy.__version__}')
print(f'RKNN Toolkit class:  {RKNN}')
print('=============================================')
"
