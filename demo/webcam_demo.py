"""
EdgeVision-Pro: Real-Time Live Webcam 3D & BEV Perception Demo
================================================================
Captures live frames from your computer's webcam (Camera 0), runs real-time
YOLOv8 + ByteTrack + Monocular 3D spatial box estimation, and renders an
automotive-style BEV top-down mini-radar canvas with real-time distance tracking.

Controls:
- [Q] or [ESC] : Exit Demo
- [S]           : Save current frame snapshot to outputs/webcam_snapshot.png
- [SPACE]       : Pause / Resume playback
"""

import os
import sys
import time
import cv2
import numpy as np

# Automatically resolve project root across both Windows & WSL environments
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

from pipeline.backend.onnx_backend import ONNXRuntimeBackend
from pipeline.bev_pipeline import BEVPerceptionPipeline


def find_model_path():
    candidates = [
        os.path.join(PROJECT_ROOT, "data", "models", "yolov8n.onnx"),
        r"\\wsl.localhost\Ubuntu\home\yankai\projects\RK3588 端侧多路视频分析盒子\edgevision-rk3588\data\models\yolov8n.onnx",
        r"\\wsl$\Ubuntu\home\yankai\projects\RK3588 端侧多路视频分析盒子\edgevision-rk3588\data\models\yolov8n.onnx",
    ]
    for p in candidates:
        if os.path.exists(p):
            return p
    raise FileNotFoundError(f"Could not locate yolov8n.onnx in candidate paths: {candidates}")


def main():
    print("==================================================================")
    print("      EdgeVision-Pro: Live PC Webcam 3D & BEV Perception Demo     ")
    print("               南科大 (SUSTech) · 陈创荣 (ccr1009)                 ")
    print("==================================================================")

    model_path = find_model_path()
    print(f"[*] Loading model on ONNX Runtime Backend: {model_path}")
    backend = ONNXRuntimeBackend(model_path, conf_thresh=0.30, iou_thresh=0.45)

    # Open Camera (Index 0 = Default Laptop Webcam)
    print("[*] Initializing computer webcam (Camera Index 0)...")
    cap = cv2.VideoCapture(0, cv2.CAP_DSHOW if sys.platform == "win32" else cv2.CAP_ANY)
    if not cap.isOpened():
        print("[!] Trying fallback DirectShow / default backend...")
        cap = cv2.VideoCapture(0)

    if not cap.isOpened():
        print("[-] Error: Unable to open webcam (Index 0). Please check camera privacy permissions.")
        return 1

    # Configure optimal camera resolution
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    cap.set(cv2.CAP_PROP_FPS, 30)

    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)) or 640
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)) or 480
    print(f"[+] Webcam connected successfully! Resolution: {w}x{h}")

    # Calibrate geometry for desktop / webcam use
    # Height: ~0.9m (desk), Pitch: 0.0 deg (horizontal facing), Range: 4.0m forward
    pipeline = BEVPerceptionPipeline(
        backend=backend,
        img_w=w,
        img_h=h,
        hfov_deg=65.0,
        mount_height_m=0.90,
        pitch_deg=0.0,
        radar_size=240,
        range_fwd_m=4.0,
        range_lat_m=2.5
    )

    window_name = "EdgeVision-Pro | Live Webcam 3D Perception Demo (Press Q to Exit)"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, 960, 720)

    fps_smooth = 0.0
    paused = False
    last_frame = None

    output_dir = os.path.join(PROJECT_ROOT, "outputs")
    os.makedirs(output_dir, exist_ok=True)

    print("\n[+] Demo is now RUNNING! Switch to the pop-up window to view.")
    print("    • Press 'Q' or 'ESC' to exit")
    print("    • Press 'S' to take a snapshot")
    print("    • Press 'SPACE' to pause/resume\n")

    while True:
        if not paused:
            t_start = time.perf_counter()
            ret, frame = cap.read()
            if not ret:
                print("[-] Failed to grab frame from camera.")
                break

            # Mirror horizontally for natural webcam selfie experience
            frame = cv2.flip(frame, 1)

            # Process frame through full 3D BEV pipeline
            annotated_frame, metrics = pipeline.process_frame(frame, channel_id=0)

            t_elapsed = time.perf_counter() - t_start
            cur_fps = 1.0 / max(1e-5, t_elapsed)
            fps_smooth = 0.85 * fps_smooth + 0.15 * cur_fps if fps_smooth > 0 else cur_fps

            # Overlay instruction hint bar at bottom
            h_out, w_out = annotated_frame.shape[:2]
            cv2.rectangle(annotated_frame, (0, h_out - 28), (w_out, h_out), (15, 18, 22), -1)
            hint_text = f"FPS: {fps_smooth:.1f} | Latency: {metrics['total_ms']:.1f}ms | [Q/ESC] Exit | [S] Snapshot | [SPACE] Pause"
            cv2.putText(
                annotated_frame, hint_text, (14, h_out - 9),
                cv2.FONT_HERSHEY_SIMPLEX, 0.42, (180, 200, 210), 1, cv2.LINE_AA
            )
            last_frame = annotated_frame
        else:
            annotated_frame = last_frame

        cv2.imshow(window_name, annotated_frame)
        key = cv2.waitKey(1) & 0xFF

        if key in [ord('q'), ord('Q'), 27]:  # Q or ESC
            print("[*] Exiting demo cleanly...")
            break
        elif key == ord(' '):  # SPACE
            paused = not paused
            print(f"[*] {'Paused' if paused else 'Resumed'}")
        elif key in [ord('s'), ord('S')]:  # S
            snap_path = os.path.join(output_dir, f"webcam_snapshot_{int(time.time())}.png")
            cv2.imwrite(snap_path, annotated_frame)
            print(f"[+] Saved snapshot to: {snap_path}")

    cap.release()
    cv2.destroyAllWindows()
    print("==================================================================")
    print("[SUCCESS] Live Webcam Demo Session Finished!")
    print("==================================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
