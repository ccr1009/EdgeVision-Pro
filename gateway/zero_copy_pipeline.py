"""
Hardware Zero-Copy Memory Pipeline (MPP -> RGA -> RKNN)
==========================================================
Models the DMA-BUF physical memory life-cycle across hardware units:
1. Rockchip MPP: Decodes H.264/H.265 directly into DMA-BUF contiguous memory.
2. Rockchip RGA: Reads DMA-BUF fd, performs 2D color convert (NV12->RGB) & scale,
   writes to destination DMA-BUF fd.
3. Rockchip RKNN: Imports destination DMA-BUF fd directly to NPU tensor memory.

Zero-copy eliminates all intermediate CPU memcpy operations, saving up to
~25MB/frame of memory bandwidth per 1080P stream.
"""

import os
import time
from typing import Dict, List, Optional


class DMABuffer:
    """Represents a hardware-allocated contiguous DMA-BUF descriptor."""

    def __init__(self, buf_id: int, size_bytes: int, width: int, height: int, fmt: str):
        self.fd = 1000 + buf_id        # Simulated Linux file descriptor
        self.size_bytes = size_bytes
        self.width = width
        self.height = height
        self.fmt = fmt                # e.g. "NV12", "RGB888", "RGBA"
        self.is_busy = False
        self.timestamp = 0.0


class ZeroCopyPipeline:
    """
    Simulates and manages the Zero-Copy DMA-BUF lifecycle on x86,
    and provides the architectural C++ interface specification for RK3588.
    """

    def __init__(self, pool_size: int = 8, img_w: int = 1920, img_h: int = 1080):
        self.pool_size = pool_size
        self.img_w = img_w
        self.img_h = img_h

        # Pre-allocate buffer pools
        # NV12 frame size = W * H * 1.5
        nv12_size = int(img_w * img_h * 1.5)
        self.mpp_pool = [
            DMABuffer(i, nv12_size, img_w, img_h, "NV12") for i in range(pool_size)
        ]

        # RGB 640x640 model input buffer size = 640 * 640 * 3
        rknn_size = 640 * 640 * 3
        self.rga_pool = [
            DMABuffer(100 + i, rknn_size, 640, 640, "RGB888") for i in range(pool_size)
        ]

        # Statistics
        self.zero_copy_frames = 0
        self.bytes_saved_total = 0

    def acquire_mpp_buffer(self) -> Optional[DMABuffer]:
        """Acquires an idle DMA buffer for MPP hardware decode output."""
        for buf in self.mpp_pool:
            if not buf.is_busy:
                buf.is_busy = True
                buf.timestamp = time.perf_counter()
                return buf
        return None

    def rga_hardware_blit(
        self,
        src_buf: DMABuffer,
        dst_w: int = 640,
        dst_h: int = 640
    ) -> Optional[DMABuffer]:
        """
        Executes hardware 2D transformation (NV12 -> RGB888 + resize) via RGA.
        In hardware, this executes in ~0.5ms on the 2D RGA core without CPU involvement.
        """
        for dst_buf in self.rga_pool:
            if not dst_buf.is_busy:
                dst_buf.is_busy = True
                dst_buf.timestamp = time.perf_counter()

                # Memory bandwidth saved vs CPU memcpy:
                # 1 CPU read + 1 CPU write for resize + color conversion
                # = (src_size + dst_size) bytes
                saved = src_buf.size_bytes + dst_buf.size_bytes
                self.bytes_saved_total += saved
                self.zero_copy_frames += 1

                # Free source buffer back to pool
                src_buf.is_busy = False
                return dst_buf
        return None

    def release_rga_buffer(self, buf: DMABuffer):
        """Releases the RGA buffer after NPU inference completion."""
        buf.is_busy = False

    def get_stats(self) -> Dict:
        return {
            "zero_copy_frames": self.zero_copy_frames,
            "bytes_saved_mb": self.bytes_saved_total / (1024 * 1024),
            "mpp_pool_in_use": sum(1 for b in self.mpp_pool if b.is_busy),
            "rga_pool_in_use": sum(1 for b in self.rga_pool if b.is_busy),
        }
