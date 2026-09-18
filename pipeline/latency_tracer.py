# ==============================================================================
# EdgeVision-RK3588: Latency Tracer & Performance Profiler
# ==============================================================================
import time
import threading
from collections import deque
import numpy as np


class LatencyTracer:
    def __init__(self, window_size=100):
        self.window_size = window_size
        self._lock = threading.Lock()
        self.history = {
            "capture": deque(maxlen=window_size),
            "preprocess": deque(maxlen=window_size),
            "inference": deque(maxlen=window_size),
            "postprocess": deque(maxlen=window_size),
            "tracking": deque(maxlen=window_size),
            "event": deque(maxlen=window_size),
            "encode": deque(maxlen=window_size),
            "total": deque(maxlen=window_size)
        }
        self.frame_count = 0
        self.start_time = time.time()

    def record(self, stage_timings: dict):
        with self._lock:
            self.frame_count += 1
            total_ms = 0.0
            for k, v in stage_timings.items():
                if k in self.history:
                    self.history[k].append(v)
                    total_ms += v
            self.history["total"].append(total_ms)

    def get_summary(self):
        with self._lock:
            summary = {}
            for k, v in self.history.items():
                if len(v) > 0:
                    arr = np.array(v)
                    summary[k] = {
                        "avg": float(np.mean(arr)),
                        "p50": float(np.percentile(arr, 50)),
                        "p95": float(np.percentile(arr, 95)),
                        "p99": float(np.percentile(arr, 99)),
                        "min": float(np.min(arr)),
                        "max": float(np.max(arr))
                    }
                else:
                    summary[k] = {"avg": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "min": 0.0, "max": 0.0}
            
            elapsed = time.time() - self.start_time
            fps = self.frame_count / elapsed if elapsed > 0 else 0.0
            summary["fps"] = fps
            summary["frames"] = self.frame_count
            return summary
