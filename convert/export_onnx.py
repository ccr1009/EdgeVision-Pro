# ==============================================================================
# EdgeVision-RK3588: YOLOv8 ONNX Exporter
# - Exports PyTorch YOLOv8n to ONNX with opset 12 (Rockchip NPU recommended)
# - Validates exported ONNX with ONNXRuntime test inference
# ==============================================================================
import os
import sys
import argparse
import numpy as np
import onnx
import onnxruntime as ort
from ultralytics import YOLO

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
DEFAULT_PT = os.path.join(PROJECT_ROOT, 'data', 'models', 'yolov8n.pt')
DEFAULT_ONNX = os.path.join(PROJECT_ROOT, 'data', 'models', 'yolov8n.onnx')


def export_yolov8_onnx(pt_path, onnx_path, imgsz=640, opset=12):
    print(f"[*] Loading PyTorch weights from: {pt_path}")
    model = YOLO(pt_path)

    print(f"[*] Exporting to ONNX format (opset={opset}, imgsz={imgsz})...")
    # Ultralytics export puts onnx in the same folder as pt by default
    exported_path = model.export(
        format='onnx',
        imgsz=imgsz,
        opset=opset,
        simplify=False,     # Keep graph transparent without onnxslim conflicts
        dynamic=False,      # NPU requires static shapes
    )
    
    # Ensure destination is onnx_path
    if os.path.abspath(exported_path) != os.path.abspath(onnx_path):
        import shutil
        shutil.copyfile(exported_path, onnx_path)
    
    print(f"[+] ONNX model exported to: {onnx_path} ({os.path.getsize(onnx_path)} bytes)")
    return onnx_path


def validate_onnx_model(onnx_path):
    print(f"[*] Validating ONNX graph: {onnx_path}")
    model_proto = onnx.load(onnx_path)
    onnx.checker.check_model(model_proto)
    print("[+] ONNX checker passed!")

    print("[*] Running test inference via ONNXRuntime...")
    session = ort.InferenceSession(onnx_path, providers=['CPUExecutionProvider'])
    input_name = session.get_inputs()[0].name
    input_shape = session.get_inputs()[0].shape
    print(f"    Input Name:  {input_name}")
    print(f"    Input Shape: {input_shape}")
    
    output_name = session.get_outputs()[0].name
    output_shape = session.get_outputs()[0].shape
    print(f"    Output Name:  {output_name}")
    print(f"    Output Shape: {output_shape}")

    # Dummy inference test
    dummy_input = np.random.uniform(0.0, 1.0, (1, 3, 640, 640)).astype(np.float32)
    outputs = session.run([output_name], {input_name: dummy_input})
    print(f"[+] Test inference output shape: {outputs[0].shape}")
    assert outputs[0].shape == (1, 84, 8400), f"Unexpected shape {outputs[0].shape}"
    print("[SUCCESS] YOLOv8n ONNX export and verification completed successfully!")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Export YOLOv8 PyTorch to ONNX")
    parser.add_argument('--weights', type=str, default=DEFAULT_PT, help="Path to .pt weights")
    parser.add_argument('--output', type=str, default=DEFAULT_ONNX, help="Path to output .onnx")
    parser.add_argument('--opset', type=int, default=12, help="ONNX opset (default: 12)")
    args = parser.parse_args()

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    export_yolov8_onnx(args.weights, args.output, opset=args.opset)
    validate_onnx_model(args.output)
