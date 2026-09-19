"""
Unit Tests for BEV & Monocular 3D Perception
==============================================
"""

import numpy as np
import unittest
from bev.camera_model import CameraModel
from bev.monocular_3d import Monocular3DProjector
from bev.radar_canvas import BEVRadarCanvas


class TestBEVPerception(unittest.TestCase):

    def test_camera_projection_roundtrip(self):
        """Test 3D ground point -> 2D pixel -> 3D ground point roundtrip accuracy."""
        cam = CameraModel(
            img_w=1280,
            img_h=720,
            hfov_deg=65.0,
            mount_height_m=1.6,
            pitch_deg=8.0
        )

        test_points = [
            np.array([10.0,  0.0, 0.0]),  # 10m directly ahead
            np.array([25.0, -3.5, 0.0]),  # 25m ahead, right lane
            np.array([40.0,  3.5, 0.0]),  # 40m ahead, left lane
            np.array([55.0,  0.0, 0.0]),  # 55m far ahead
        ]

        for pt_v in test_points:
            res = cam.point_vehicle_to_pixel(pt_v)
            self.assertIsNotNone(res, f"Failed to project {pt_v}")
            u, v, z_c = res
            self.assertTrue(0 <= u < 1280, f"u={u} out of bounds")
            self.assertTrue(0 <= v < 720, f"v={v} out of bounds")
            self.assertTrue(z_c > 0, "Depth z_c must be positive")

            pt_rec = cam.pixel_to_ground_point(u, v)
            self.assertIsNotNone(pt_rec, f"Failed to back-project ({u}, {v})")

            err = float(np.linalg.norm(pt_v - pt_rec))
            self.assertLess(err, 1e-3, f"Roundtrip error too high: {err:.6f}m for {pt_v}")

    def test_monocular_3d_box_generation(self):
        """Test estimating 3D box from 2D bounding box."""
        cam = CameraModel(img_w=1280, img_h=720, hfov_deg=65.0, mount_height_m=1.6, pitch_deg=8.0)
        projector = Monocular3DProjector(cam)

        bbox_car = [500.0, 420.0, 780.0, 620.0]
        box_3d = projector.estimate_3d_box(bbox_car, "car", yaw_rad=0.0)

        self.assertIsNotNone(box_3d, "Failed to estimate 3D box for car")
        self.assertEqual(len(box_3d["corners_2d"]), 8)
        self.assertGreater(box_3d["distance_m"], 3.0)
        self.assertLess(box_3d["distance_m"], 30.0)
        self.assertGreater(box_3d["longitudinal_m"], 0)

        dummy_img = np.zeros((720, 1280, 3), dtype=np.uint8)
        rendered = projector.draw_3d_box(dummy_img, box_3d)
        self.assertEqual(rendered.shape, (720, 1280, 3))
        self.assertTrue(np.any(rendered > 0), "Rendered image should contain wireframe pixels")

    def test_radar_canvas_rendering(self):
        """Test BEV radar canvas generation and frame overlay."""
        radar = BEVRadarCanvas(canvas_size=320, range_fwd_m=50.0, range_lat_m=16.0)

        base_grid = radar.render_base_grid()
        self.assertEqual(base_grid.shape, (320, 320, 3))
        self.assertEqual(base_grid.dtype, np.uint8)

        dummy_obstacles = [
            {
                "track_id": 1,
                "class_name": "car",
                "longitudinal_m": 18.5,
                "lateral_m": -2.1,
                "dimensions": (4.5, 1.8, 1.5)
            },
            {
                "track_id": 2,
                "class_name": "person",
                "longitudinal_m": 8.0,
                "lateral_m": 3.0,
                "dimensions": (0.6, 0.6, 1.7)
            }
        ]

        canvas_with_obs = radar.render_obstacles(base_grid.copy(), dummy_obstacles)
        self.assertEqual(canvas_with_obs.shape, (320, 320, 3))

        main_frame = np.ones((720, 1280, 3), dtype=np.uint8) * 100
        overlaid = radar.overlay_on_frame(main_frame, canvas_with_obs)
        self.assertEqual(overlaid.shape, (720, 1280, 3))


if __name__ == "__main__":
    unittest.main(verbosity=2)
