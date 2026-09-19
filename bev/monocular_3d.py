"""
Monocular 3D Bounding Box Projection & Distance Estimation
===========================================================
Estimates 3D spatial position [X_v, Y_v, Z_v] and 3D oriented bounding
box from 2D detection box and camera geometry.
"""

import numpy as np
import cv2
from typing import Dict, List, Optional, Tuple
from bev.camera_model import CameraModel


class Monocular3DProjector:
    """Estimates 3D bounding box from 2D detection and projects 3D wireframe."""

    # Prior dimensions (Length, Width, Height) in meters
    CLASS_DIMENSION_PRIORS = {
        "car":        (4.5, 1.8, 1.5),
        "truck":      (7.2, 2.5, 3.0),
        "bus":       (10.5, 2.6, 3.2),
        "van":        (5.2, 2.0, 2.1),
        "person":     (0.6, 0.6, 1.7),
        "bicycle":    (1.7, 0.6, 1.2),
        "motorcycle": (2.0, 0.8, 1.3),
    }
    DEFAULT_PRIOR = (2.5, 1.2, 1.5)

    def __init__(self, camera: CameraModel):
        self.camera = camera

    def estimate_3d_box(
        self,
        bbox_2d: List[float],
        class_name: str,
        yaw_rad: float = 0.0
    ) -> Optional[Dict]:
        """
        Given 2D bbox [x1, y1, x2, y2] and class_name:
        1. Finds bottom ground contact point (cx, y2).
        2. Back-projects to ground plane [X_v, Y_v, 0].
        3. Constructs 3D oriented bounding box corners (8 points).
        4. Projects 8 corners back to 2D image plane.
        """
        x1, y1, x2, y2 = bbox_2d
        u_contact = (x1 + x2) / 2.0
        v_contact = float(y2)

        ground_pt = self.camera.pixel_to_ground_point(u_contact, v_contact)
        if ground_pt is None:
            return None

        X_v, Y_v, _ = ground_pt
        if X_v < 0.5 or X_v > 120.0:  # Valid range: 0.5m ~ 120m
            return None

        # Look up 3D dimensions
        dim = self.CLASS_DIMENSION_PRIORS.get(class_name.lower(), self.DEFAULT_PRIOR)
        L, W, H = dim

        # Center of box in vehicle frame (elevated by H/2 above road)
        center_v = np.array([X_v, Y_v, H / 2.0], dtype=np.float64)

        # Compute 8 corners in box local frame:
        # X: along heading, Y: lateral left, Z: vertical up
        cos_y, sin_y = np.cos(yaw_rad), np.sin(yaw_rad)
        R_yaw = np.array([
            [cos_y, -sin_y, 0.0],
            [sin_y,  cos_y, 0.0],
            [0.0,    0.0,   1.0]
        ], dtype=np.float64)

        # 8 corners offsets
        dx = L / 2.0
        dy = W / 2.0
        dz = H / 2.0

        local_corners = np.array([
            [ dx,  dy, -dz],  # 0: Front-Left-Bottom
            [ dx, -dy, -dz],  # 1: Front-Right-Bottom
            [-dx, -dy, -dz],  # 2: Rear-Right-Bottom
            [-dx,  dy, -dz],  # 3: Rear-Left-Bottom
            [ dx,  dy,  dz],  # 4: Front-Left-Top
            [ dx, -dy,  dz],  # 5: Front-Right-Top
            [-dx, -dy,  dz],  # 6: Rear-Right-Top
            [-dx,  dy,  dz],  # 7: Rear-Left-Top
        ], dtype=np.float64)

        # Transform corners to vehicle frame: center_v + R_yaw @ local_corners
        corners_v = center_v + (local_corners @ R_yaw.T)

        # Project 8 corners to 2D image
        corners_2d = []
        depths = []
        for corner in corners_v:
            proj = self.camera.point_vehicle_to_pixel(corner)
            if proj is None:
                return None
            u, v, z_c = proj
            corners_2d.append((int(round(u)), int(round(v))))
            depths.append(z_c)

        return {
            "center_v": center_v,       # [X_v (fwd), Y_v (lat), Z_v (up)]
            "corners_v": corners_v,     # (8, 3) in vehicle frame
            "corners_2d": corners_2d,   # 8 points (u, v) on image
            "dimensions": (L, W, H),
            "distance_m": float(np.sqrt(X_v**2 + Y_v**2)),
            "longitudinal_m": float(X_v),
            "lateral_m": float(Y_v),
        }

    def draw_3d_box(
        self,
        img: np.ndarray,
        box_3d: Dict,
        color: Tuple[int, int, int] = (0, 255, 255),
        thickness: int = 2
    ) -> np.ndarray:
        """
        Draws 3D oriented wireframe box on image:
        - Bottom ring: 0-1-2-3
        - Top ring: 4-5-6-7
        - Vertical pillars: 0-4, 1-5, 2-6, 3-7
        - Front face cross: 0-5, 1-4 (to indicate heading direction)
        """
        c2d = box_3d["corners_2d"]
        if len(c2d) != 8:
            return img

        # 1. Draw bottom face (ground contact)
        for i in range(4):
            cv2.line(img, c2d[i], c2d[(i + 1) % 4], color, thickness)

        # 2. Draw top face
        for i in range(4):
            cv2.line(img, c2d[i + 4], c2d[((i + 1) % 4) + 4], color, thickness)

        # 3. Draw vertical pillars
        for i in range(4):
            cv2.line(img, c2d[i], c2d[i + 4], color, thickness)

        # 4. Front face highlighted in blue/red to show orientation
        front_color = (255, 128, 0)  # Bright cyan/orange for front face
        cv2.line(img, c2d[0], c2d[5], front_color, 1)
        cv2.line(img, c2d[1], c2d[4], front_color, 1)

        # 5. Draw 3D label with distance
        dist_str = f"{box_3d['longitudinal_m']:.1f}m"
        text_pos = (c2d[4][0], max(15, c2d[4][1] - 6))
        cv2.putText(
            img, dist_str, text_pos,
            cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 255), 2, cv2.LINE_AA
        )
        return img
