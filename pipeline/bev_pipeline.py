"""
Unified BEV & Spatial Perception Pipeline
===========================================
End-to-end pipeline integrating:
1. Video capture / Frame ingestion
2. YOLOv8n inference (ONNX / RKNN Backend)
3. ByteTrack multi-target association
4. Monocular 3D distance and 3D wireframe box estimation
5. Real-time Top-Down BEV Radar Map synthesis and video compositing
"""

import time
import cv2
import numpy as np
from typing import Dict, List, Optional, Tuple

from pipeline.backend.base import BaseBackend
from track.byte_tracker import BYTETracker
from bev.camera_model import CameraModel
from bev.monocular_3d import Monocular3DProjector
from bev.radar_canvas import BEVRadarCanvas
from gateway.npu_scheduler import NPUCoreScheduler, RKNNCoreMask


class BEVPerceptionPipeline:
    """End-to-end perception and BEV radar compositing pipeline."""

    COCO_CLASSES = {
        0: "person", 1: "bicycle", 2: "car", 3: "motorcycle", 5: "bus", 7: "truck"
    }

    def __init__(
        self,
        backend: BaseBackend,
        img_w: int = 1280,
        img_h: int = 720,
        hfov_deg: float = 65.0,
        mount_height_m: float = 1.65,
        pitch_deg: float = 8.5,
        radar_size: int = 300,
        range_fwd_m: float = 60.0,
        range_lat_m: float = 18.0
    ):
        self.backend = backend
        self.img_w = img_w
        self.img_h = img_h

        # 1. Initialize BYTETracker
        self.tracker = BYTETracker(
            track_thresh=0.35,
            high_thresh=0.50,
            match_thresh=0.70
        )

        # 2. Initialize Camera Model & 3D Projector
        self.camera = CameraModel(
            img_w=img_w,
            img_h=img_h,
            hfov_deg=hfov_deg,
            mount_height_m=mount_height_m,
            pitch_deg=pitch_deg
        )
        self.projector = Monocular3DProjector(self.camera)

        # 3. Initialize BEV Radar Canvas
        self.radar = BEVRadarCanvas(
            canvas_size=radar_size,
            range_fwd_m=range_fwd_m,
            range_lat_m=range_lat_m
        )

        # 4. NPU Scheduler
        self.scheduler = NPUCoreScheduler(mode="round_robin")

        # Stats
        self.frame_idx = 0
        self.colors = [
            (255, 160, 40), (0, 220, 255), (80, 240, 100),
            (255, 60, 240), (240, 240, 40), (0, 180, 255)
        ]

    def process_frame(self, frame_bgr: np.ndarray, channel_id: int = 0) -> Tuple[np.ndarray, Dict]:
        """
        Executes complete perception + 3D + BEV step on a single frame.
        """
        t0 = time.perf_counter()
        self.frame_idx += 1
        h_orig, w_orig = frame_bgr.shape[:2]

        # 1. Acquire NPU Core & Infer
        core = self.scheduler.acquire_core(channel_id)
        t_before_infer = time.perf_counter()
        dets = self.backend.infer(frame_bgr)
        t_infer = (time.perf_counter() - t_before_infer) * 1000.0
        self.scheduler.release_core(core)

        # 2. Multi-Object Tracking (ByteTrack)
        t_before_track = time.perf_counter()
        tracks = self.tracker.update(dets)
        t_track = (time.perf_counter() - t_before_track) * 1000.0

        # 3. Monocular 3D Estimation & Wireframe Rendering
        annotated_frame = frame_bgr.copy()
        tracked_3d_objects = []

        for trk in tracks:
            tid = trk.track_id
            bbox = trk.tlbr
            cid = int(trk.class_id)
            cls_name = self.COCO_CLASSES.get(cid, "car")
            color = self.colors[tid % len(self.colors)]

            # Estimate 3D bounding box
            box_3d = self.projector.estimate_3d_box(bbox, cls_name)
            if box_3d is not None:
                # Draw 3D wireframe box
                self.projector.draw_3d_box(annotated_frame, box_3d, color=color, thickness=2)

                # Store for BEV radar
                tracked_3d_objects.append({
                    "track_id": tid,
                    "class_name": cls_name,
                    "longitudinal_m": box_3d["longitudinal_m"],
                    "lateral_m": box_3d["lateral_m"],
                    "dimensions": box_3d["dimensions"]
                })
            else:
                # Fallback: Draw 2D box if 3D is above horizon or invalid
                x1, y1, x2, y2 = [int(v) for v in bbox]
                cv2.rectangle(annotated_frame, (x1, y1), (x2, y2), color, 2)

            # Draw label tag
            x1, y1 = int(bbox[0]), int(bbox[1])
            tag = f"#{tid} {cls_name}"
            cv2.putText(
                annotated_frame, tag, (x1, max(18, y1 - 6)),
                cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1, cv2.LINE_AA
            )

        # 4. Render BEV Radar Map
        radar_bg = self.radar.render_base_grid()
        radar_full = self.radar.render_obstacles(radar_bg, tracked_3d_objects)
        annotated_frame = self.radar.overlay_on_frame(annotated_frame, radar_full, alpha=0.88)

        t_end = time.perf_counter()
        total_lat_ms = (t_end - t0) * 1000.0

        # 5. Draw HUD telemetry banner
        hud_text = (
            f"EdgeVision-Pro | RK3588 Tri-Core [{core.name}] | "
            f"Latency: {total_lat_ms:.1f}ms (Infer: {t_infer:.1f}ms) | "
            f"3D Targets: {len(tracked_3d_objects)}"
        )
        # Background box for HUD
        cv2.rectangle(annotated_frame, (10, 8), (w_orig - 10, 42), (18, 22, 26), -1)
        cv2.rectangle(annotated_frame, (10, 8), (w_orig - 10, 42), (45, 55, 65), 1)
        cv2.putText(
            annotated_frame, hud_text, (20, 31),
            cv2.FONT_HERSHEY_SIMPLEX, 0.50, (0, 255, 200), 1, cv2.LINE_AA
        )

        metrics = {
            "total_ms": total_lat_ms,
            "infer_ms": t_infer,
            "track_ms": t_track,
            "objects_3d": len(tracked_3d_objects),
            "core": core.name
        }
        return annotated_frame, metrics
