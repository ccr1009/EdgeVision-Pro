"""
Unit Tests for Multi-Stream Gateway, Zero-Copy & NPU Scheduler
===============================================================
"""

import unittest
from gateway.zero_copy_pipeline import ZeroCopyPipeline
from gateway.npu_scheduler import NPUCoreScheduler, RKNNCoreMask


class TestGatewayAndZeroCopy(unittest.TestCase):

    def test_zero_copy_pipeline_lifecycle(self):
        pipeline = ZeroCopyPipeline(pool_size=4, img_w=1920, img_h=1080)

        # 1. Acquire buffer from MPP pool
        mpp_buf = pipeline.acquire_mpp_buffer()
        self.assertIsNotNone(mpp_buf)
        self.assertTrue(mpp_buf.is_busy)
        self.assertEqual(mpp_buf.fmt, "NV12")

        # 2. Perform RGA hardware 2D blit (NV12 -> RGB888 640x640)
        rga_buf = pipeline.rga_hardware_blit(mpp_buf, dst_w=640, dst_h=640)
        self.assertIsNotNone(rga_buf)
        self.assertTrue(rga_buf.is_busy)
        self.assertFalse(mpp_buf.is_busy, "MPP buffer should be recycled after RGA blit")

        # 3. Release RGA buffer after NPU inference
        pipeline.release_rga_buffer(rga_buf)
        self.assertFalse(rga_buf.is_busy)

        # Verify statistics
        stats = pipeline.get_stats()
        self.assertEqual(stats["zero_copy_frames"], 1)
        self.assertGreater(stats["bytes_saved_mb"], 4.0)

    def test_npu_scheduler_round_robin(self):
        scheduler = NPUCoreScheduler(mode="round_robin")

        # Sequentially acquire cores for 3 incoming frames
        c1 = scheduler.acquire_core(channel_id=0)
        c2 = scheduler.acquire_core(channel_id=1)
        c3 = scheduler.acquire_core(channel_id=2)
        c4 = scheduler.acquire_core(channel_id=0)

        self.assertEqual(c1, RKNNCoreMask.RKNN_NPU_CORE_0)
        self.assertEqual(c2, RKNNCoreMask.RKNN_NPU_CORE_1)
        self.assertEqual(c3, RKNNCoreMask.RKNN_NPU_CORE_2)
        self.assertEqual(c4, RKNNCoreMask.RKNN_NPU_CORE_0)  # Round-robin wrapped back to Core 0

    def test_npu_scheduler_affinity_binding(self):
        scheduler = NPUCoreScheduler()
        # Bind Channel 2 exclusively to Core 2
        scheduler.bind_channel_to_core(channel_id=2, core=RKNNCoreMask.RKNN_NPU_CORE_2)

        assigned_core = scheduler.acquire_core(channel_id=2)
        self.assertEqual(assigned_core, RKNNCoreMask.RKNN_NPU_CORE_2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
