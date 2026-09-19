"""
EdgeVision-Pro: 3D Perception & BEV Gateway Demo Runner
=========================================================
Runs full pipeline on test traffic video, rendering 3D bounding boxes,
ByteTrack IDs, and top-down BEV radar mini-map into an annotated MP4.
"""

import os
import sys
import time
import cv2
import numpy as np

from pipeline.backend.onnx_backend import ONNXRuntimeBackend
from pipeline.bev_pipeline import BEVPerceptionPipeline


def main():
    print("==================================================================")
    print("EdgeVision Pro: Multi-Camera BEV & 3D Spatial Perception Demo")
    print("==================================================================")

    model_path = "data/models/yolov8n.onnx"
    video_path = "data/videos/person-bicycle-car-detection.mp4"
    output_dir = "outputs"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "demo_bev_gateway.mp4")

    if not os.path.exists(model_path):
        print(f"[-] Model not found: {model_path}")
        return 1

    if not os.path.exists(video_path):
        print(f"[-] Video not found: {video_path}")
        return 1

    # 1. Initialize Backend
    print(f"[*] Loading model on ONNX Runtime Backend: {model_path}")
    backend = ONNXRuntimeBackend(model_path, conf_thresh=0.30, iou_thresh=0.45)

    # 2. Open Video
    cap = cv2.VideoCapture(video_path)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f"[+] Video opened: {w}x{h} @ {fps:.1f} FPS, total {total_frames} frames")

    # 3. Initialize Pipeline
    # Calibrated for traffic/vehicle front camera: height 1.65m, pitch 8.5 deg
    pipeline = BEVPerceptionPipeline(
        backend=backend,
        img_w=w,
        img_h=h,
        hfov_deg=68.0,
        mount_height_m=1.65,
        pitch_deg=8.5,
        radar_size=300,
        range_fwd_m=60.0,
        range_lat_m=18.0
    )

    # 4. Setup Video Writer
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    out = cv2.VideoWriter(output_path, fourcc, fps, (w, h))

    print(f"[*] Processing frames and generating 3D BEV video to: {output_path}")
    t_start = time.perf_counter()
    processed_count = 0
    total_infer_time = 0.0

    # Process 250 frames
    max_frames = min(250, total_frames)

    while cap.isOpened() and processed_count < max_frames:
        ret, frame = cap.read()
        if not ret:
            break

        annotated_frame, metrics = pipeline.process_frame(frame, channel_id=0)
        out.write(annotated_frame)

        processed_count += 1
        total_infer_time += metrics["infer_ms"]

        if processed_count % 25 == 0 or processed_count == max_frames:
            elapsed = time.perf_counter() - t_start
            cur_fps = processed_count / elapsed
            print(
                f"  Frame [{processed_count:03d}/{max_frames}] | "
                f"FPS: {cur_fps:.1f} | Latency: {metrics['total_ms']:.1f}ms | "
                f"3D Targets: {metrics['objects_3d']} | NPU: {metrics['core']}"
            )

    cap.release()
    out.release()

    total_time = time.perf_counter() - t_start
    avg_fps = processed_count / total_time
    avg_infer = total_infer_time / max(1, processed_count)

    print("==================================================================")
    print(f"[SUCCESS] Demo Generation Completed!")
    print(f"  Processed Frames: {processed_count}")
    print(f"  Average FPS:      {avg_fps:.1f} FPS")
    print(f"  Avg Infer Time:   {avg_infer:.1f} ms")
    print(f"  Output Video:     {output_path} ({os.path.getsize(output_path)} bytes)")
    print("==================================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
