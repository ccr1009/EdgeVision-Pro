# ==============================================================================
# EdgeVision-RK3588: RKNN Inference Backend (RK3588 NPU Target / x86 Simulator Stub)
# ==============================================================================
import os
import sys
import numpy as np
from .base import BaseBackend


class RKNNBackend(BaseBackend):
    def __init__(self, model_path: str, conf_thresh=0.25, iou_thresh=0.45, target_platform="rk3588"):
        self.conf_thresh = conf_thresh
        self.iou_thresh = iou_thresh
        self.target_platform = target_platform
        self.is_board_runtime = False
        self.rknn = None
        self.load(model_path)

    def load(self, model_path: str):
        if not os.path.exists(model_path):
            raise FileNotFoundError(f"RKNN model not found: {model_path}")

        try:
            from rknnlite.api import RKNNLite
            print("[*] RKNNBackend: Detected RK3588 board environment (rknnlite)")
            self.rknn = RKNNLite()
            ret = self.rknn.load_rknn(model_path)
            if ret != 0:
                raise RuntimeError("Failed to load RKNN model via RKNNLite")
            ret = self.rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_AUTO)
            if ret != 0:
                raise RuntimeError("Failed to init RKNNLite runtime on board NPU")
            self.is_board_runtime = True
            print("[+] RKNNBackend: NPU initialized successfully with Core Auto Balance!")
            return
        except ImportError:
            pass

        print("[!] RKNNBackend: Board NPU runtime (rknnlite) not found.")
        print("    --> Note: On x86, RKNN binary models (.rknn) cannot execute without physical NPU.")
        print("    --> Use ONNXRuntimeBackend for x86 simulation, or deploy to RK3588 board for NPU.")
        self.is_board_runtime = False

    def get_input_shape(self):
        return (640, 640)

    def infer(self, img_bgr: np.ndarray) -> np.ndarray:
        if not self.is_board_runtime:
            raise NotImplementedError(
                "RKNNBackend.infer() requires RK3588 physical board with rknnlite/rknpu2.\n"
                "For x86 PC development, please set backend='onnx' in pipeline configuration."
            )
        return np.empty((0, 6), dtype=np.float32)

    def release(self):
        if self.rknn is not None:
            self.rknn.release()
