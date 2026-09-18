import argparse
import sys
import os

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, PROJECT_ROOT)

from pipeline.pipeline_engine import PipelineEngine


def main():
    parser = argparse.ArgumentParser(description="EdgeVision-RK3588 Real-Time Video Analysis Demo")
    parser.add_argument("--video", type=str, default="data/videos/test_surveillance.mp4", help="Video file or RTSP stream")
    parser.add_argument("--backend", type=str, default="onnx", choices=["onnx", "rknn"], help="Inference backend (onnx for x86)")
    parser.add_argument("--model", type=str, default=None, help="Path to model file (.onnx or .rknn)")
    parser.add_argument("--output", type=str, default="outputs/demo_annotated.mp4", help="Output annotated video path")
    parser.add_argument("--log", type=str, default="outputs/events.log", help="Output event alert log path")
    args = parser.parse_args()

    engine = PipelineEngine(
        video_src=args.video,
        backend_type=args.backend,
        model_path=args.model,
        output_video=args.output,
        event_log=args.log
    )
    engine.run()


if __name__ == "__main__":
    main()
