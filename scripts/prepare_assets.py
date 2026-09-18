import os
import cv2
import numpy as np
import urllib.request
import sys

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
DATA_DIR = os.path.join(PROJECT_ROOT, 'data')
MODELS_DIR = os.path.join(DATA_DIR, 'models')
VIDEOS_DIR = os.path.join(DATA_DIR, 'videos')
CALIB_DIR = os.path.join(DATA_DIR, 'calib')

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
    pt_path = os.path.join(MODELS_DIR, 'yolov8n.pt')
    if os.path.exists(pt_path) and os.path.getsize(pt_path) > 1000000:
        print(f"[+] yolov8n.pt already exists ({os.path.getsize(pt_path)} bytes)")
        return pt_path

    url = "https://github.com/ultralytics/assets/releases/download/v8.2.0/yolov8n.pt"
    if not download_file(url, pt_path, "YOLOv8n weights"):
        print("[*] Attempting fallback via ultralytics YOLO API...")
        from ultralytics import YOLO
        model = YOLO('yolov8n.pt')
        import shutil
        if os.path.exists('yolov8n.pt'):
            shutil.move('yolov8n.pt', pt_path)
    return pt_path


def generate_synthetic_video(output_path, num_frames=300, width=640, height=640, fps=30):
    """
    Generates a realistic synthetic test video simulating street surveillance:
    - Multiple pedestrians (moving vertically & horizontally)
    - Vehicles (cars moving along a road lane)
    - Occlusion / crossing scenarios to validate ByteTrack
    - Virtual alert line at y = 380 (Green line, turns Red when crossed)
    """
    print(f"[*] Generating synthetic test video: {output_path} ({num_frames} frames)...")
    fourcc = cv2.VideoWriter_fourcc(*'mp4v')
    out = cv2.VideoWriter(output_path, fourcc, fps, (width, height))

    # Define trackable synthetic objects: [x, y, w, h, vx, vy, color, label, class_id]
    objects = [
        # Person 1 walking down crossing the alert line
        {"x": 150.0, "y": 80.0, "w": 40, "h": 90, "vx": 0.8, "vy": 2.0, "color": (255, 100, 100), "name": "person", "cls": 0},
        # Person 2 walking across, occluding Person 1 around frame 100
        {"x": 50.0, "y": 260.0, "w": 38, "h": 85, "vx": 1.8, "vy": 0.3, "color": (100, 255, 100), "name": "person", "cls": 0},
        # Person 3 walking up
        {"x": 450.0, "y": 500.0, "w": 42, "h": 92, "vx": -0.6, "vy": -1.7, "color": (100, 100, 255), "name": "person", "cls": 0},
        # Car 1 driving fast rightwards across bottom
        {"x": 30.0, "y": 480.0, "w": 120, "h": 60, "vx": 3.5, "vy": 0.0, "color": (200, 200, 50), "name": "car", "cls": 2},
        # Car 2 driving leftwards
        {"x": 550.0, "y": 400.0, "w": 110, "h": 55, "vx": -2.8, "vy": 0.0, "color": (50, 200, 200), "name": "car", "cls": 2},
    ]

    for frame_idx in range(num_frames):
        # Create background (dark asphalt road and pavement)
        frame = np.full((height, width, 3), 40, dtype=np.uint8)
        
        # Pavement / sidewalk (top half)
        cv2.rectangle(frame, (0, 0), (width, 350), (60, 60, 65), -1)
        # Road lane markings
        for x_dash in range(20, width, 60):
            cv2.line(frame, (x_dash, 460), (x_dash + 30, 460), (200, 200, 200), 2)

        # Virtual Tripwire Line (y = 380)
        cv2.line(frame, (0, 380), (width, 380), (0, 255, 255), 2)
        cv2.putText(frame, "TRIPWIRE / INTRUSION LINE (Y=380)", (10, 370),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 255), 1, cv2.LINE_AA)

        # Render objects
        for obj in objects:
            # Update position
            obj["x"] += obj["vx"]
            obj["y"] += obj["vy"]

            # Boundary wrap-around
            if obj["x"] > width + 50: obj["x"] = -obj["w"]
            if obj["x"] < -obj["w"] - 50: obj["x"] = width
            if obj["y"] > height + 50: obj["y"] = -obj["h"]
            if obj["y"] < -obj["h"] - 50: obj["y"] = height

            x1 = int(obj["x"])
            y1 = int(obj["y"])
            x2 = int(x1 + obj["w"])
            y2 = int(y1 + obj["h"])

            # Draw body
            cv2.rectangle(frame, (x1, y1), (x2, y2), obj["color"], -1)
            # Add contrast border and head/details for detector realism
            cv2.rectangle(frame, (x1, y1), (x2, y2), (255, 255, 255), 1)
            if obj["name"] == "person":
                # Draw head circle
                head_center = (int(x1 + obj["w"]/2), int(y1 + 15))
                cv2.circle(frame, head_center, 10, (220, 200, 180), -1)
            elif obj["name"] == "car":
                # Draw wheels
                cv2.circle(frame, (int(x1 + 25), int(y2)), 8, (10, 10, 10), -1)
                cv2.circle(frame, (int(x2 - 25), int(y2)), 8, (10, 10, 10), -1)

        # Timestamp HUD
        cv2.putText(frame, f"Frame: {frame_idx:04d} | EdgeVision Synthetic Stream", (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1, cv2.LINE_AA)

        out.write(frame)

    out.release()
    print(f"[+] Synthetic video generated: {output_path} ({os.path.getsize(output_path)} bytes)")


def prepare_calibration_dataset(video_path, num_samples=60):
    """
    Extracts diverse frames from the test video to form the RKNN INT8 quantization calibration set.
    Writes image paths into data/calib/dataset.txt.
    """
    print(f"[*] Extracting {num_samples} calibration frames from {video_path}...")
    cap = cv2.VideoCapture(video_path)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    step = max(1, total_frames // num_samples)

    calib_list = []
    saved_count = 0
    frame_idx = 0

    while cap.isOpened() and saved_count < num_samples:
        ret, frame = cap.read()
        if not ret:
            break
        if frame_idx % step == 0:
            # Resize to 640x640 standard YOLO input
            frame_resized = cv2.resize(frame, (640, 640))
            img_name = f"calib_{saved_count:03d}.jpg"
            img_path = os.path.join(CALIB_DIR, img_name)
            cv2.imwrite(img_path, frame_resized)
            calib_list.append(img_path)
            saved_count += 1
        frame_idx += 1

    cap.release()

    dataset_txt_path = os.path.join(CALIB_DIR, 'dataset.txt')
    with open(dataset_txt_path, 'w', encoding='utf-8') as f:
        for p in calib_list:
            f.write(p + '\n')

    print(f"[+] Saved {saved_count} calibration images to {CALIB_DIR}")
    print(f"[+] Calibration list written to {dataset_txt_path}")
    return dataset_txt_path


def main():
    print("=== EdgeVision Asset Preparation ===")
    
    # 1. Weights
    pt_path = get_yolov8n_weights()
    print(f"[1/3] Weights ready: {pt_path}")

    # 2. Test Video
    test_video_path = os.path.join(VIDEOS_DIR, 'test_surveillance.mp4')
    if not os.path.exists(test_video_path):
        # Try downloading a public sample or fallback to synthetic
        sample_url = "https://raw.githubusercontent.com/airockchip/rknn_model_zoo/main/examples/yolov8/model/bus.jpg"
        # Always generate high quality test video with known tripwire & multi-object trajectory
        generate_synthetic_video(test_video_path, num_frames=300, width=640, height=640, fps=30)
    print(f"[2/3] Test video ready: {test_video_path}")

    # 3. Calibration Set
    dataset_txt = prepare_calibration_dataset(test_video_path, num_samples=60)
    print(f"[3/3] Calibration dataset ready: {dataset_txt}")
    print("=== Asset Preparation Completed! ===")


if __name__ == '__main__':
    main()
