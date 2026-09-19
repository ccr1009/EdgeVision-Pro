"""
RK3588 Tri-Core NPU Dynamic Load Scheduler
============================================
RK3588 features 3 independent NPU cores (Core 0, Core 1, Core 2),
each providing 2.0 TOPS (total 6.0 TOPS INT8).
This scheduler maps multi-channel inference workloads across cores
using core affinity masks (rknn_core_mask).
"""

from enum import IntEnum
import threading
from typing import Dict, List, Optional


class RKNNCoreMask(IntEnum):
    RKNN_NPU_CORE_AUTO = 0
    RKNN_NPU_CORE_0    = 1    # 1 << 0
    RKNN_NPU_CORE_1    = 2    # 1 << 1
    RKNN_NPU_CORE_2    = 4    # 1 << 2
    RKNN_NPU_CORE_0_1  = 3    # Cores 0 + 1
    RKNN_NPU_CORE_ALL  = 7    # Cores 0 + 1 + 2


class NPUCoreScheduler:
    """
    Distributes multi-channel inference tasks across RK3588 NPU cores.
    Supports Round-Robin, Core Affinity Binding, and Dynamic Load Balancing.
    """

    def __init__(self, mode: str = "round_robin"):
        self.mode = mode
        self.cores = [
            RKNNCoreMask.RKNN_NPU_CORE_0,
            RKNNCoreMask.RKNN_NPU_CORE_1,
            RKNNCoreMask.RKNN_NPU_CORE_2,
        ]
        self._lock = threading.Lock()
        self._next_core_idx = 0
        self.core_task_counts = {c: 0 for c in self.cores}
        self.channel_affinity: Dict[int, RKNNCoreMask] = {}

    def bind_channel_to_core(self, channel_id: int, core: RKNNCoreMask):
        """Statically binds a video channel to a designated NPU core."""
        with self._lock:
            self.channel_affinity[channel_id] = core

    def acquire_core(self, channel_id: int) -> RKNNCoreMask:
        """Assigns the optimal NPU core for the incoming frame."""
        with self._lock:
            # 1. Static affinity override
            if channel_id in self.channel_affinity:
                core = self.channel_affinity[channel_id]
                self.core_task_counts[core] += 1
                return core

            # 2. Round-Robin distribution
            if self.mode == "round_robin":
                core = self.cores[self._next_core_idx]
                self._next_core_idx = (self._next_core_idx + 1) % len(self.cores)
                self.core_task_counts[core] += 1
                return core

            # 3. Least-Loaded Core dynamic balancing
            min_core = min(self.core_task_counts, key=self.core_task_counts.get)
            self.core_task_counts[min_core] += 1
            return min_core

    def release_core(self, core: RKNNCoreMask):
        """Marks core processing complete."""
        with self._lock:
            if core in self.core_task_counts:
                self.core_task_counts[core] = max(0, self.core_task_counts[core] - 1)

    def get_load_distribution(self) -> Dict[str, int]:
        with self._lock:
            return {f"Core_{c.name.split('_')[-1]}": count for c, count in self.core_task_counts.items()}
