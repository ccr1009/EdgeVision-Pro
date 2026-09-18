# ==============================================================================
# EdgeVision-RK3588: ONNX to RKNN Converter (RK3588 NPU Target)
# Supports:
# 1. INT8 Quantization with dataset calibration (PTQ)
# 2. FP16 (Non-quantized) conversion for precision baseline comparison
# ==============================================================================
import os
import sys
import argparse
from rknn.api import RKNN

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
DEFAULT_ONNX = os.path.join(PROJECT_ROOT, 'data', 'models', 'yolov8n.onnx')
DEFAULT_CALIB = os.path.join(PROJECT_ROOT, 'data', 'calib', 'dataset.txt')
OUTPUT_DIR = os.path.join(PROJECT_ROOT, 'data', 'models')


def convert_onnx_to_rknn(onnx_path, output_path, target_platform='rk3588', do_quant=True, dataset=None):
    print("==================================================================")
    print(f"[*] Converting ONNX to RKNN: {onnx_path}")
    print(f"    Target Platform: {target_platform}")
    print(f"    Quantization:    {'INT8 (with calibration)' if do_quant else 'FP16'}")
    print(f"    Output Path:     {output_path}")
    print("==================================================================")

    rknn = RKNN(verbose=False)

    # 1. Config model
    # YOLO input is RGB normalized [0, 1] -> mean=0, std=255 converts [0, 255] uint8 image
    print("--> [1/4] Configuring model pre-processing & target platform...")
    rknn.config(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform=target_platform
    )
    print("    Configuration done.")

    # 2. Load ONNX model
    print(f"--> [2/4] Loading ONNX model from: {onnx_path}...")
    ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        print("[-] Load ONNX model failed!")
        sys.exit(ret)
    print("    Loading done.")

    # 3. Build model (quantization or fp16)
    print(f"--> [3/4] Building RKNN model (do_quantization={do_quant})...")
    if do_quant:
        if not dataset or not os.path.exists(dataset):
            raise FileNotFoundError(f"Calibration dataset not found: {dataset}")
        print(f"    Using calibration dataset: {dataset}")
        ret = rknn.build(do_quantization=True, dataset=dataset)
    else:
        ret = rknn.build(do_quantization=False)
    
    if ret != 0:
        print("[-] Build RKNN model failed!")
        sys.exit(ret)
    print("    Building done.")

    # 4. Export RKNN model
    print(f"--> [4/4] Exporting RKNN model to: {output_path}...")
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    ret = rknn.export_rknn(output_path)
    if ret != 0:
        print("[-] Export RKNN model failed!")
        sys.exit(ret)
    print(f"[+] RKNN model successfully exported: {output_path} ({os.path.getsize(output_path)} bytes)")

    # 5. Release
    rknn.release()
    return output_path


def main():
    parser = argparse.ArgumentParser(description="Convert YOLOv8 ONNX to RKNN for RK3588")
    parser.add_argument('--onnx', type=str, default=DEFAULT_ONNX, help="Path to input ONNX")
    parser.add_argument('--target', type=str, default='rk3588', help="NPU target platform (default: rk3588)")
    parser.add_argument('--calib', type=str, default=DEFAULT_CALIB, help="Path to dataset.txt calibration file")
    parser.add_argument('--build-both', action='store_true', default=True, help="Build both INT8 and FP16 models")
    args = parser.parse_args()

    if not os.path.exists(args.onnx):
        print(f"[*] ONNX model {args.onnx} not found, generating now...")
        import subprocess
        subprocess.run([sys.executable, os.path.join(PROJECT_ROOT, 'convert', 'export_onnx.py')], check=True)

    # 1. Build INT8 model
    int8_output = os.path.join(OUTPUT_DIR, 'yolov8n_rk3588_i8.rknn')
    convert_onnx_to_rknn(args.onnx, int8_output, target_platform=args.target, do_quant=True, dataset=args.calib)

    # 2. Build FP16 model (optional for precision comparison)
    if args.build_both:
        fp16_output = os.path.join(OUTPUT_DIR, 'yolov8n_rk3588_fp.rknn')
        convert_onnx_to_rknn(args.onnx, fp16_output, target_platform=args.target, do_quant=False)

    print("")
    print("[SUCCESS] All RKNN model conversions completed successfully!")


if __name__ == '__main__':
    main()
