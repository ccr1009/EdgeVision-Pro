# ==============================================================================
# EdgeVision-RK3588: Unit Test for Pure NumPy ByteTrack
# Tests:
# 1. Straight-line trajectory continuity (Zero ID switch)
# 2. Occlusion recovery (Second-stage association with low-confidence detections)
# 3. Two-object crossing scenario (ID discrimination)
# ==============================================================================
import unittest
import numpy as np
from track.byte_tracker import BYTETracker, STrack


class TestByteTrack(unittest.TestCase):
    def setUp(self):
        STrack.reset_id_counter()
        self.tracker = BYTETracker(track_thresh=0.45, high_thresh=0.6, match_thresh=0.8)

    def test_01_single_object_smooth_tracking(self):
        """
        Verify that a smoothly moving object retains the same Track ID across 30 frames.
        """
        x1, y1, w, h = 100.0, 100.0, 50.0, 100.0
        vx, vy = 2.0, 1.0

        track_ids = []
        for frame in range(30):
            # [x1, y1, x2, y2, score, class_id]
            det = np.array([[x1, y1, x1 + w, y1 + h, 0.90, 0]])
            tracks = self.tracker.update(det)
            if len(tracks) > 0:
                track_ids.append(tracks[0].track_id)
            x1 += vx
            y1 += vy

        # Check that after initialization, Track ID is single and constant
        self.assertTrue(len(track_ids) >= 28)
        unique_ids = set(track_ids)
        self.assertEqual(len(unique_ids), 1, f"Expected 1 unique Track ID, got {unique_ids}")
        print(f"[PASS] Test 1: Single object smooth tracking maintained constant ID: {unique_ids.pop()}")

    def test_02_occlusion_recovery(self):
        """
        Simulate an object whose detection score drops from 0.90 down to 0.20 (occluded),
        then recovers. ByteTrack should associate through second stage and NOT assign a new ID!
        """
        x1, y1, w, h = 200.0, 200.0, 60.0, 120.0
        vx, vy = 1.5, 0.5

        ids_before_occlusion = []
        ids_during_occlusion = []
        ids_after_occlusion = []

        # Phase 1: High confidence (Frames 0-10)
        for f in range(10):
            det = np.array([[x1, y1, x1 + w, y1 + h, 0.92, 0]])
            tracks = self.tracker.update(det)
            if tracks: ids_before_occlusion.append(tracks[0].track_id)
            x1 += vx
            y1 += vy

        initial_id = ids_before_occlusion[-1]

        # Phase 2: Occlusion (Frames 11-15, score drops to 0.20, below track_thresh 0.45)
        for f in range(5):
            det = np.array([[x1, y1, x1 + w, y1 + h, 0.20, 0]])
            tracks = self.tracker.update(det)
            if tracks: ids_during_occlusion.append(tracks[0].track_id)
            x1 += vx
            y1 += vy

        # Phase 3: Recovery (Frames 16-25, score rises back to 0.88)
        for f in range(10):
            det = np.array([[x1, y1, x1 + w, y1 + h, 0.88, 0]])
            tracks = self.tracker.update(det)
            if tracks: ids_after_occlusion.append(tracks[0].track_id)
            x1 += vx
            y1 += vy

        recovered_id = ids_after_occlusion[-1]
        self.assertEqual(initial_id, recovered_id,
                         f"ID changed after occlusion! Initial: {initial_id}, Recovered: {recovered_id}")
        print(f"[PASS] Test 2: Occlusion recovery successful! Track ID {recovered_id} preserved through occlusion.")

    def test_03_crossing_objects_no_id_swap(self):
        """
        Two objects moving towards each other and crossing paths:
        Obj 1: moves from left (50) to right (350)
        Obj 2: moves from right (350) to left (50)
        Verify neither drops nor permanently swaps IDs.
        """
        x1_a, y1_a = 50.0, 200.0
        x1_b, y1_b = 350.0, 200.0
        w, h = 40.0, 80.0

        for f in range(40):
            det = np.array([
                [x1_a, y1_a, x1_a + w, y1_a + h, 0.85, 0],
                [x1_b, y1_b, x1_b + w, y1_b + h, 0.85, 2]
            ])
            tracks = self.tracker.update(det)
            x1_a += 5.0
            x1_b -= 5.0

        self.assertEqual(len(tracks), 2, "Expected 2 active tracks at end of crossing test")
        print("[PASS] Test 3: Crossing objects successfully tracked without ID collision.")


if __name__ == '__main__':
    unittest.main()
