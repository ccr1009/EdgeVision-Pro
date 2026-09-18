# ==============================================================================
# EdgeVision-RK3588: Abstract Inference Backend Interface
# ==============================================================================
from abc import ABC, abstractmethod
import numpy as np


class BaseBackend(ABC):
    @abstractmethod
    def load(self, model_path: str):
        """Load model weights or binary graph."""
        pass

    @abstractmethod
    def infer(self, img_bgr: np.ndarray) -> np.ndarray:
        """
        Runs inference on raw BGR frame [H, W, 3].
        Returns: detections [N, 6] (x1, y1, x2, y2, score, class_id)
        """
        pass

    @abstractmethod
    def get_input_shape(self):
        """Returns input shape (width, height)."""
        pass
