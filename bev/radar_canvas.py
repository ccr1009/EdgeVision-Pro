"""
Bird's Eye View (BEV) Radar Canvas
===================================
Generates real-time top-down 2D radar grid visualization of surrounding
obstacles in ego-vehicle coordinates.
"""

import numpy as np
import cv2
from typing import List, Dict, Tuple, Optional


class BEVRadarCanvas:
    """Generates and overlays a top-down Bird's Eye View (BEV) radar map."""

    def __init__(
        self,
        canvas_size: int = 320,
        range_fwd_m: float = 50.0,
        range_lat_m: float = 16.0,
        bg_color: Tuple[int, int, int] = (20, 24, 28)
    ):
        """
        Args:
            canvas_size: Square canvas width & height in pixels.
            range_fwd_m: Maximum forward range in meters.
            range_lat_m: Maximum lateral range (+/- meters from center).
        """
        self.size = canvas_size
        self.range_fwd = float(range_fwd_m)
        self.range_lat = float(range_lat_m)
        self.bg_color = bg_color

        # Ego vehicle position on canvas (bottom center)
        self.ego_u = canvas_size // 2
        self.ego_v = canvas_size - 25

        # Scale factors: pixels per meter
        self.scale_fwd = (self.ego_v - 20) / self.range_fwd
        self.scale_lat = (canvas_size / 2.0 - 15) / self.range_lat

    def vehicle_to_canvas(self, X_v: float, Y_v: float) -> Tuple[int, int]:
        """
        Convert vehicle frame (X_v: fwd, Y_v: left) to canvas pixel (u, v).
        Left Y_v > 0 maps to u < ego_u (left).
        Right Y_v < 0 maps to u > ego_u (right).
        """
        u = int(round(self.ego_u - Y_v * self.scale_lat))
        v = int(round(self.ego_v - X_v * self.scale_fwd))
        return u, v

    def render_base_grid(self) -> np.ndarray:
        """Renders dark high-tech radar background with range rings and grid."""
        canvas = np.full((self.size, self.size, 3), self.bg_color, dtype=np.uint8)

        # 1. Concentric Distance Rings (10m, 20m, 30m, 40m, 50m)
        ring_color = (45, 55, 65)
        text_color = (120, 140, 160)
        for dist_m in [10.0, 20.0, 30.0, 40.0, 50.0]:
            if dist_m <= self.range_fwd:
                radius_px = int(round(dist_m * self.scale_fwd))
                cv2.circle(canvas, (self.ego_u, self.ego_v), radius_px, ring_color, 1, cv2.LINE_AA)
                label = f"{int(dist_m)}m"
                cv2.putText(
                    canvas, label, (self.ego_u + 6, self.ego_v - radius_px + 12),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.32, text_color, 1, cv2.LINE_AA
                )

        # 2. Lateral Grid Lines (e.g. -10m, -5m, 0m, +5m, +10m lane guides)
        grid_color = (35, 42, 50)
        for lat_m in [-10.0, -5.0, 0.0, 5.0, 10.0]:
            u, _ = self.vehicle_to_canvas(0.0, lat_m)
            if 10 <= u < self.size - 10:
                style = 2 if lat_m == 0.0 else 1
                color = (50, 65, 80) if lat_m == 0.0 else grid_color
                cv2.line(canvas, (u, 20), (u, self.ego_v), color, style)

        # 3. Camera FOV Cone Lines (~65 deg)
        fov_rad = np.radians(32.5)
        max_x = self.range_fwd
        max_y = max_x * np.tan(fov_rad)
        u_left, v_top = self.vehicle_to_canvas(max_x, max_y)
        u_right, _ = self.vehicle_to_canvas(max_x, -max_y)
        cv2.line(canvas, (self.ego_u, self.ego_v), (u_left, v_top), (40, 70, 90), 1, cv2.LINE_AA)
        cv2.line(canvas, (self.ego_u, self.ego_v), (u_right, v_top), (40, 70, 90), 1, cv2.LINE_AA)

        # 4. Ego Vehicle Icon (Arrow / Small Car)
        ego_color = (0, 220, 255)  # Cyan/Gold
        pts = np.array([
            [self.ego_u, self.ego_v - 12],
            [self.ego_u - 7, self.ego_v + 6],
            [self.ego_u, self.ego_v + 2],
            [self.ego_u + 7, self.ego_v + 6]
        ], np.int32)
        cv2.fillPoly(canvas, [pts], ego_color)

        # Title / Watermark
        cv2.putText(
            canvas, "BEV RADAR (ISO Ego)", (12, 18),
            cv2.FONT_HERSHEY_SIMPLEX, 0.42, (0, 255, 200), 1, cv2.LINE_AA
        )
        return canvas

    def render_obstacles(
        self,
        canvas: np.ndarray,
        tracked_3d_objects: List[Dict]
    ) -> np.ndarray:
        """
        Draws dynamic obstacles onto radar canvas.
        tracked_3d_objects item format:
        {
            "track_id": int,
            "class_name": str,
            "longitudinal_m": float,  # X_v
            "lateral_m": float,       # Y_v
            "dimensions": (L, W, H),
        }
        """
        for obj in tracked_3d_objects:
            X_v = obj.get("longitudinal_m", 0.0)
            Y_v = obj.get("lateral_m", 0.0)
            track_id = obj.get("track_id", -1)
            cls_name = obj.get("class_name", "car").lower()
            L, W, _ = obj.get("dimensions", (4.5, 1.8, 1.5))

            if X_v < 0.5 or X_v > self.range_fwd:
                continue

            u_center, v_center = self.vehicle_to_canvas(X_v, Y_v)
            if not (0 <= u_center < self.size and 0 <= v_center < self.size):
                continue

            # Convert dimensions to pixels
            half_w_px = max(2, int(round((W / 2.0) * self.scale_lat)))
            half_l_px = max(3, int(round((L / 2.0) * self.scale_fwd)))

            # Color by class
            if "car" in cls_name or "truck" in cls_name or "bus" in cls_name:
                fill_color = (255, 160, 40)   # Vivid Cyan-Blue (BGR)
                edge_color = (255, 220, 100)
            elif "person" in cls_name:
                fill_color = (40, 220, 255)   # Amber Yellow
                edge_color = (100, 255, 255)
            else:
                fill_color = (80, 220, 100)   # Green for bikes
                edge_color = (140, 255, 160)

            # Draw filled bounding rectangle on BEV
            pt1 = (u_center - half_w_px, v_center - half_l_px)
            pt2 = (u_center + half_w_px, v_center + half_l_px)
            cv2.rectangle(canvas, pt1, pt2, fill_color, -1)
            cv2.rectangle(canvas, pt1, pt2, edge_color, 1)

            # Draw Track ID and distance tag
            tag = f"#{track_id}:{X_v:.1f}m"
            cv2.putText(
                canvas, tag, (u_center + half_w_px + 3, v_center + 4),
                cv2.FONT_HERSHEY_SIMPLEX, 0.32, (240, 240, 240), 1, cv2.LINE_AA
            )

        return canvas

    def overlay_on_frame(
        self,
        frame: np.ndarray,
        canvas: np.ndarray,
        alpha: float = 0.88,
        margin: int = 16
    ) -> np.ndarray:
        """
        Overlays the BEV radar canvas on the top-right corner of the camera frame.
        """
        h_f, w_f = frame.shape[:2]
        h_c, w_c = canvas.shape[:2]

        # Target ROI in top-right corner
        x_start = w_f - w_c - margin
        y_start = margin
        x_end = x_start + w_c
        y_end = y_start + h_c

        if x_start < 0 or y_end > h_f:
            return frame

        roi = frame[y_start:y_end, x_start:x_end]
        blended = cv2.addWeighted(canvas, alpha, roi, 1.0 - alpha, 0)
        frame[y_start:y_end, x_start:x_end] = blended

        # Draw decorative outer border
        cv2.rectangle(frame, (x_start - 1, y_start - 1), (x_end + 1, y_end + 1), (0, 200, 255), 1)
        return frame
