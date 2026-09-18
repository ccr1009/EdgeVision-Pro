import os
import cv2
import numpy as np
import urllib.request
import sys

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DATA_DIR = os.path.join(PROJECT_ROOT, "data")
MODELS_DIR = os.path.join(DATA_DIR, "models")
VIDEOS_DIR = os.path.join(DATA_DIR, "videos")
CALIB_DIR = os.path.join(DATA_DIR, "calib")

os.makedirs(MODELS_DIR, exist_ok=True)
os.makedirs(VIDEOS_DIR, exist_ok=True)
os.makedirs(CALIB_DIR, exist_ok=True)


def download_file(url, target_path, desc="file"):
    print(f"[*] Downloading {desc} from: {url}")
    try:
        urllib.request.urlretrieve(url, target_path)
        print(f"[+] Downloaded to {target_path} ({os.path.getsize(target_path)} bytes)")
        return True
    except Exception as e:
        print(f"[-] Failed to download {desc}: {e}")
        return False


def get_yolov8n_weights():
    pt_path = os.path.join(MODELS_DIR, "yolov8n.pt")
    if os.path.exists(pt_path) and os.path.getsize(pt_path) > 1000000:
        print(f"[+] yolov8n.pt already exists ({os.path.getsize(pt_path)} bytes)")
        return pt_path

    url = "https://github.com/ultralytics/assets/releases/download/v8.2.0/yolov8n.pt"
    if not download_file(url, pt_path, "YOLOv8n weights"):
        from ultralytics import YOLO
        import shutil
        model = YOLO("yolov8n.pt")
        if os.path.exists("yolov8n.pt"):
            shutil.move("yolov8n.pt", pt_path)
    return pt_path


def get_test_video():
    test_video_path = os.path.join(VIDEOS_DIR, "test_surveillance.mp4")
    sample_url = "https://raw.githubusercontent.com/intel-iot-devkit/sample-videos/master/person-bicycle-car-detection.mp4"
    
    if os.path.exists(test_video_path) and os.path.getsize(test_video_path) > 1000000:
        print(f"[+] test_surveillance.mp4 ready ({os.path.getsize(test_video_path)} bytes)")
        return test_video_path

    # Try downloading real surveillance video clip
    if download_file(sample_url, test_video_path, "Intel IoT surveillance sample video"):
        return test_video_path

    # Fallback to synthetic video
    print("[*] Generating synthetic video fallback...")
    generate_synthetic_video(test_video_path)
    return test_video_path


def generate_synthetic_video(output_path, num_frames=300, width=640, height=640, fps=30):
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    out = cv2.VideoWriter(output_path, fourcc, fps, (width, height))

    objects = [
        {"x": 150.0, "y": 80.0, "w": 40, "h": 90, "vx": 0.8, "vy": 2.0, "color": (255, 100, 100), "name": "person", "cls": 0},
        {"x": 50.0, "y": 260.0, "w": 38, "h": 85, "vx": 1.8, "vy": 0.3, "color": (100, 255, 100), "name": "person", "cls": 0},
        {"x": 450.0, "y": 500.0, "w": 42, "h": 92, "vx": -0.6, "vy": -1.7, "color": (100, 100, 255), "name": "person", "cls": 0},
        {"x": 30.0, "y": 480.0, "w": 120, "h": 60, "vx": 3.5, "vy": 0.0, "color": (200, 200, 50), "name": "car", "cls": 2},
    ]

    for frame_idx in range(num_frames):
        frame = np.full((height, width, 3), 40, dtype=np.uint8)
        cv2.rectangle(frame, (0, 0), (width, 350), (60, 60, 65), -1)
        for x_dash in range(20, width, 60):
            cv2.line(frame, (x_dash, 460), (x_dash + 30, 460), (200, 200, 200), 2)
        cv2.line(frame, (0, 380), (width, 380), (0, 255, 255), 2)

        for obj in objects:
            obj["x"] += obj["vx"]
            obj["y"] += obj["vy"]
            if obj["x"] > width + 50: obj["x"] = -obj["w"]
            if obj["y"] > height + 50: obj["y"] = -obj["h"]
            x1, y1 = int(obj["x"]), int(obj["y"])
            x2, y2 = int(x1 + obj["w"]), int(y1 + obj["h"])
            cv2.rectangle(frame, (x1, y1), (x2, y2), obj["color"], -1)
            cv2.rectangle(frame, (x1, y1), (x2, y2), (255, 255, 255), 1)

        out.write(frame)
    out.release()


def prepare_calibration_dataset(video_path, num_samples=60):
    cap = cv2.VideoCapture(video_path)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    step = max(1, total_frames // num_samples)

    calib_list = []
    saved_count = 0
    frame_idx = 0

    while cap.isOpened() and saved_count < num_samples:
        ret, frame = cap.read()
        if not ret: break
        if frame_idx % step == 0:
            frame_resized = cv2.resize(frame, (640, 640))
            img_name = f"calib_{saved_count:03d}.jpg"
            img_path = os.path.join(CALIB_DIR, img_name)
            cv2.imwrite(img_path, frame_resized)
            calib_list.append(img_path)
            saved_count += 1
        frame_idx += 1

    cap.release()

    dataset_txt_path = os.path.join(CALIB_DIR, "dataset.txt")
    with open(dataset_txt_path, "w", encoding="utf-8") as f:
        for p in calib_list:
            f.write(p + "\n")
    return dataset_txt_path


def main():
    print("=== EdgeVision Asset Preparation ===")
    pt_path = get_yolov8n_weights()
    video_path = get_test_video()
    calib_txt = prepare_calibration_dataset(video_path, num_samples=60)
    print("=== Asset Preparation Completed! ===")


if __name__ == "__main__":
    main()
