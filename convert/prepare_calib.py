# ==============================================================================
# EdgeVision-RK3588: Calibration Dataset Preparation for Post-Training Quantization (PTQ)
# ==============================================================================
import os
import cv2
import sys

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
CALIB_DIR = os.path.join(PROJECT_ROOT, 'data', 'calib')
DATASET_TXT = os.path.join(CALIB_DIR, 'dataset.txt')


def verify_calibration_dataset():
    if not os.path.exists(DATASET_TXT):
        print(f"[-] Calibration list not found: {DATASET_TXT}")
        print("[*] Running asset preparation script...")
        import subprocess
        subprocess.run([sys.executable, os.path.join(PROJECT_ROOT, 'scripts', 'prepare_assets.py')], check=True)

    with open(DATASET_TXT, 'r', encoding='utf-8') as f:
        lines = [line.strip() for line in f if line.strip()]

    valid_images = []
    for img_p in lines:
        if os.path.exists(img_p):
            img = cv2.imread(img_p)
            if img is not None and img.shape[:2] == (640, 640):
                valid_images.append(img_p)

    print(f"[+] Verified {len(valid_images)} valid calibration images (640x640) in {DATASET_TXT}")
    if len(valid_images) < 20:
        raise ValueError(f"Calibration images too few ({len(valid_images)} < 20). Need at least 20-50 images.")
    return DATASET_TXT


if __name__ == '__main__':
    verify_calibration_dataset()
