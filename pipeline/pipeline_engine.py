# ==============================================================================
# EdgeVision-RK3588: Multi-Threaded Real-Time Video Analysis Pipeline
# ==============================================================================
import os
import cv2
import time
import threading
from queue import Queue, Empty, Full
from tabulate import tabulate
import numpy as np

from .latency_tracer import LatencyTracer
from .event_detector import EventDetector
from .backend.onnx_backend import ONNXRuntimeBackend
from track.byte_tracker import BYTETracker

CLASS_NAMES = ["person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck"]


class PipelineEngine:
    def __init__(self, video_src, backend_type="onnx", model_path=None, output_video=None, event_log="outputs/events.log"):
        self.video_src = video_src
        self.backend_type = backend_type
        self.output_video = output_video
        self.event_log = event_log

        if model_path is None:
            if backend_type == "onnx":
                model_path = "data/models/yolov8n.onnx"
            else:
                model_path = "data/models/yolov8n_rk3588_i8.rknn"
        self.model_path = model_path

        self.frame_queue = Queue(maxsize=5)
        self.detect_queue = Queue(maxsize=5)
        self.render_queue = Queue(maxsize=5)

        self.stop_event = threading.Event()
        self.tracer = LatencyTracer(window_size=100)
        self.event_detector = EventDetector(log_path=event_log, tripwire_y=380)

        print(f"[*] Initializing {backend_type.upper()} inference backend...")
        if backend_type == "onnx":
            self.backend = ONNXRuntimeBackend(self.model_path)
        else:
            from .backend.rknn_backend import RKNNBackend
            self.backend = RKNNBackend(self.model_path)

        self.tracker = BYTETracker(track_thresh=0.45, high_thresh=0.6, match_thresh=0.8)

    def _capture_worker(self):
        cap = cv2.VideoCapture(self.video_src)
        if not cap.isOpened():
            print(f"[-] Failed to open video source: {self.video_src}")
            self.stop_event.set()
            return

        frame_id = 0
        while not self.stop_event.is_set():
            t0 = time.time()
            ret, frame = cap.read()
            t_cap = (time.time() - t0) * 1000.0

            if not ret:
                print("[*] Capture reached EOF.")
                break

            frame_id += 1
            item = {
                "frame_id": frame_id,
                "frame": frame,
                "t_cap": t_cap
            }

            try:
                self.frame_queue.put(item, block=True, timeout=0.1)
            except Full:
                try:
                    _ = self.frame_queue.get_nowait()
                    self.frame_queue.put_nowait(item)
                except (Empty, Full):
                    pass

        cap.release()
        self.frame_queue.put(None)

    def _inference_worker(self):
        while not self.stop_event.is_set():
            try:
                item = self.frame_queue.get(timeout=0.2)
            except Empty:
                continue

            if item is None:
                self.detect_queue.put(None)
                break

            t0 = time.time()
            dets = self.backend.infer(item["frame"])
            t_infer = (time.time() - t0) * 1000.0

            item["dets"] = dets
            item["t_infer"] = t_infer

            try:
                self.detect_queue.put(item, block=True, timeout=0.5)
            except Full:
                pass

    def _tracking_event_worker(self):
        while not self.stop_event.is_set():
            try:
                item = self.detect_queue.get(timeout=0.2)
            except Empty:
                continue

            if item is None:
                self.render_queue.put(None)
                break

            t0 = time.time()
            tracks = self.tracker.update(item["dets"])
            t_track = (time.time() - t0) * 1000.0

            t0 = time.time()
            alerts = self.event_detector.update(tracks, item["frame_id"])
            t_event = (time.time() - t0) * 1000.0

            item["tracks"] = tracks
            item["alerts"] = alerts
            item["t_track"] = t_track
            item["t_event"] = t_event

            try:
                self.render_queue.put(item, block=True, timeout=0.5)
            except Full:
                pass

    def _render_output_worker(self):
        writer = None
        colors = [
            (255, 120, 0), (0, 200, 255), (100, 255, 100),
            (255, 50, 255), (255, 255, 0), (0, 150, 255)
        ]

        while not self.stop_event.is_set():
            try:
                item = self.render_queue.get(timeout=0.2)
            except Empty:
                continue

            if item is None:
                break

            t0 = time.time()
            frame = item["frame"]
            h, w = frame.shape[:2]

            if writer is None and self.output_video:
                fourcc = cv2.VideoWriter_fourcc(*"mp4v")
                os.makedirs(os.path.dirname(self.output_video), exist_ok=True)
                writer = cv2.VideoWriter(self.output_video, fourcc, 30.0, (w, h))

            cv2.line(frame, (0, 380), (w, 380), (0, 255, 255), 2)
            cv2.putText(frame, "VIRTUAL TRIPWIRE (Y=380)", (10, 370),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 255), 1)

            for track in item["tracks"]:
                tid = track.track_id
                tlbr = track.tlbr.astype(int)
                color = colors[tid % len(colors)]
                cv2.rectangle(frame, (tlbr[0], tlbr[1]), (tlbr[2], tlbr[3]), color, 2)

                cls_id = track.class_id
                cls_name = CLASS_NAMES[cls_id] if cls_id < len(CLASS_NAMES) else f"cls_{cls_id}"
                label = f"ID:{tid} {cls_name} {track.score:.2f}"
                cv2.putText(frame, label, (tlbr[0], max(15, tlbr[1] - 5)),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)

            if item["alerts"]:
                for alert in item["alerts"]:
                    cv2.putText(frame, f"[ALERT] TRACK #{alert['track_id']} CROSSED TRIPWIRE!",
                                (50, 70), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)

            t_encode = (time.time() - t0) * 1000.0

            self.tracer.record({
                "capture": item.get("t_cap", 0.0),
                "inference": item.get("t_infer", 0.0),
                "tracking": item.get("t_track", 0.0),
                "event": item.get("t_event", 0.0),
                "encode": t_encode
            })

            summary = self.tracer.get_summary()
            hud_text = f"FPS: {summary['fps']:.1f} | Infer: {summary['inference']['avg']:.1f}ms | Track: {summary['tracking']['avg']:.1f}ms"
            cv2.putText(frame, hud_text, (10, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)

            if writer:
                writer.write(frame)

        if writer:
            writer.release()
            print(f"[+] Annotated video saved to: {self.output_video}")

    def run(self):
        print("==================================================================")
        print("EdgeVision-RK3588: Starting Multi-Threaded Real-Time Pipeline")
        print(f"Video Source:    {self.video_src}")
        print(f"Backend:         {self.backend_type.upper()}")
        print(f"Model Path:      {self.model_path}")
        print(f"Output Video:    {self.output_video}")
        print(f"Event Log:       {self.event_log}")
        print("==================================================================")

        threads = [
            threading.Thread(target=self._capture_worker, name="CaptureThread"),
            threading.Thread(target=self._inference_worker, name="InferThread"),
            threading.Thread(target=self._tracking_event_worker, name="TrackEventThread"),
            threading.Thread(target=self._render_output_worker, name="RenderThread")
        ]

        t_start = time.time()
        for t in threads:
            t.start()

        for t in threads:
            t.join()

        total_time = time.time() - t_start
        print(f"\n[+] Pipeline completed in {total_time:.2f}s!")
        self.print_benchmark_report()

    def print_benchmark_report(self):
        summary = self.tracer.get_summary()
        headers = ["Stage", "Avg Latency (ms)", "p50 (ms)", "p95 (ms)", "p99 (ms)"]
        rows = [
            ["Video Capture", f"{summary['capture']['avg']:.2f}", f"{summary['capture']['p50']:.2f}", f"{summary['capture']['p95']:.2f}", f"{summary['capture']['p99']:.2f}"],
            ["YOLOv8 Inference", f"{summary['inference']['avg']:.2f}", f"{summary['inference']['p50']:.2f}", f"{summary['inference']['p95']:.2f}", f"{summary['inference']['p99']:.2f}"],
            ["ByteTrack Multi-Object", f"{summary['tracking']['avg']:.2f}", f"{summary['tracking']['p50']:.2f}", f"{summary['tracking']['p95']:.2f}", f"{summary['tracking']['p99']:.2f}"],
            ["Tripwire Event Detector", f"{summary['event']['avg']:.2f}", f"{summary['event']['p50']:.2f}", f"{summary['event']['p95']:.2f}", f"{summary['event']['p99']:.2f}"],
            ["Render & Video Encode", f"{summary['encode']['avg']:.2f}", f"{summary['encode']['p50']:.2f}", f"{summary['encode']['p95']:.2f}", f"{summary['encode']['p99']:.2f}"],
            ["End-to-End Total", f"{summary['total']['avg']:.2f}", f"{summary['total']['p50']:.2f}", f"{summary['total']['p95']:.2f}", f"{summary['total']['p99']:.2f}"]
        ]
        print("\n" + "="*66)
        print(f"PIPELINE BENCHMARK REPORT ({self.backend_type.upper()} Backend on x86 CPU)")
        print(f"Processed Frames: {summary['frames']} | Effective FPS: {summary['fps']:.1f}")
        print(tabulate(rows, headers=headers, tablefmt="github"))
        print("="*66)
