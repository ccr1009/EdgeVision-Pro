"""
Camera Model & Coordinate System Transformation
=================================================
Implements pinhole camera geometry, intrinsic/extrinsic matrices,
and ground-plane projection (Inverse Perspective Mapping, IPM).

Coordinate Conventions:
1. Pixel Frame (u, v): u right [0, W-1], v down [0, H-1]
2. Camera Frame (Xc, Yc, Zc) [OpenCV Convention]:
   - Xc: Right
   - Yc: Down
   - Zc: Forward (Optical Axis / Depth)
3. Ego-Vehicle / Road Frame (Xv, Yv, Zv) [ISO Standard]:
   - Xv: Forward (Longitudinal distance)
   - Yv: Left (Lateral offset)
   - Zv: Up (Ground plane is Zv = 0, Camera at [0, 0, h])
"""

import numpy as np
from typing import Tuple, Optional, List


class CameraModel:
    """Pinhole camera with extrinsic parameters and ground-plane back-projection."""

    def __init__(
        self,
        img_w: int = 1280,
        img_h: int = 720,
        hfov_deg: float = 65.0,
        mount_height_m: float = 1.6,
        pitch_deg: float = 8.0,
        roll_deg: float = 0.0,
        yaw_deg: float = 0.0,
    ):
        """
        Args:
            img_w: Image width in pixels.
            img_h: Image height in pixels.
            hfov_deg: Horizontal Field of View in degrees.
            mount_height_m: Camera mounting height above ground in meters.
            pitch_deg: Downward pitch angle in degrees (positive = tilted down toward road).
            roll_deg: Roll angle in degrees (positive = tilted right).
            yaw_deg: Yaw angle in degrees (positive = panned left).
        """
        self.img_w = img_w
        self.img_h = img_h
        self.mount_height = float(mount_height_m)
        self.pitch = float(np.radians(pitch_deg))
        self.roll = float(np.radians(roll_deg))
        self.yaw = float(np.radians(yaw_deg))

        # 1. Compute Intrinsic Matrix K
        self.fx = (img_w / 2.0) / np.tan(np.radians(hfov_deg) / 2.0)
        self.fy = self.fx  # Square pixels assumption
        self.cx = img_w / 2.0
        self.cy = img_h / 2.0

        self.K = np.array([
            [self.fx, 0.0,     self.cx],
            [0.0,     self.fy, self.cy],
            [0.0,     0.0,     1.0]
        ], dtype=np.float64)
        self.K_inv = np.linalg.inv(self.K)

        # 2. Compute Rotation Matrix from Vehicle Frame to Camera Frame: R_c_v
        R_base = np.array([
            [ 0.0, -1.0,  0.0],
            [ 0.0,  0.0, -1.0],
            [ 1.0,  0.0,  0.0]
        ], dtype=np.float64)

        cp, sp = np.cos(self.pitch), np.sin(self.pitch)
        R_pitch = np.array([
            [1.0, 0.0,  0.0],
            [0.0,  cp,  sp],
            [0.0, -sp,  cp]
        ], dtype=np.float64)

        self.R_c_v = R_pitch @ R_base
        self.R_v_c = self.R_c_v.T

        # Camera optical center in vehicle coordinates
        self.cam_pos_v = np.array([0.0, 0.0, self.mount_height], dtype=np.float64)

        # Horizon line v_horizon
        fwd_c = self.R_c_v @ np.array([1.0, 0.0, 0.0])
        if fwd_c[2] > 1e-4:
            self.v_horizon = (self.fy * fwd_c[1] / fwd_c[2]) + self.cy
        else:
            self.v_horizon = 0.0

    def pixel_to_ray_cam(self, u: float, v: float) -> np.ndarray:
        ray_c = self.K_inv @ np.array([u, v, 1.0], dtype=np.float64)
        return ray_c / np.linalg.norm(ray_c)

    def pixel_to_ground_point(self, u: float, v: float) -> Optional[np.ndarray]:
        ray_c = self.K_inv @ np.array([u, v, 1.0], dtype=np.float64)
        ray_v = self.R_v_c @ ray_c

        if ray_v[2] >= -1e-5:
            return None

        lam = -self.mount_height / ray_v[2]
        if lam <= 0:
            return None

        pt_v = self.cam_pos_v + lam * ray_v
        pt_v[2] = 0.0
        return pt_v

    def point_vehicle_to_pixel(self, pt_v: np.ndarray) -> Optional[Tuple[float, float, float]]:
        vec_v = pt_v - self.cam_pos_v
        pt_c = self.R_c_v @ vec_v

        if pt_c[2] <= 0.1:
            return None

        u = (self.fx * pt_c[0] / pt_c[2]) + self.cx
        v = (self.fy * pt_c[1] / pt_c[2]) + self.cy
        return float(u), float(v), float(pt_c[2])

    def point_cam_to_pixel(self, pt_c: np.ndarray) -> Optional[Tuple[float, float, float]]:
        if pt_c[2] <= 0.1:
            return None
        u = (self.fx * pt_c[0] / pt_c[2]) + self.cx
        v = (self.fy * pt_c[1] / pt_c[2]) + self.cy
        return float(u), float(v), float(pt_c[2])
