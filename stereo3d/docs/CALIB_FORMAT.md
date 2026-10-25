# 标定文件格式契约 `stereo_calib.yml`

> **这是 PC 端（生成方）和板端（消费方）之间唯一的硬契约。**
> 生成方：`tools/calib/stereo_calibrate.py`
> 消费方：`src/stereo_pipeline.cpp :: StereoPipeline::loadCalib()`

## 0. 一句话

用 OpenCV `FileStorage`（写出来就是普通 YAML），**所有矩阵用 `cv::Mat` 直接写**，
板端用 `cv::FileStorage(path, cv::FileStorage::READ)` 读，键名一字不许改。

## 1. 键名表

| 键 | 类型 | 形状 | 说明 |
|---|---|---|---|
| `image_width`  | int    | 标量 | 标定所用原始图宽，如 1280 |
| `image_height` | int    | 标量 | 标定所用原始图高，如 720 |
| `K_left`       | double | 3x3 | 左目内参 |
| `D_left`       | double | 1x5 或 1x8 | 左目畸变 (k1,k2,p1,p2,k3[,k4,k5,k6]) |
| `K_right`      | double | 3x3 | 右目内参 |
| `D_right`      | double | 1x5 或 1x8 | 右目畸变 |
| `R`            | double | 3x3 | 右目相对左目的旋转（`stereoCalibrate` 的第一个输出） |
| `T`            | double | 3x1 | 右目相对左目的平移，**单位与标定板方格边长一致（我们用米）** |
| `R1`           | double | 3x3 | 左目矫正旋转（`stereoRectify` 输出） |
| `R2`           | double | 3x3 | 右目矫正旋转 |
| `P1`           | double | 3x4 | 左目矫正后投影矩阵 |
| `P2`           | double | 3x4 | 右目矫正后投影矩阵 |
| `Q`            | double | 4x4 | 视差→深度矩阵（`reprojectImageTo3D` 用） |
| `rms`          | double | 标量 | 重投影 RMS，像素 |
| `baseline_m`   | double | 标量 | 基线，米（= `norm(T)`，两个来源可互校） |
| `calib_date`   | string | 标量 | ISO 时间戳，便于追溯 |

## 2. 关键换算（板端必须这么算，别自己发明）

```cpp
// 矫正后左目焦距（全分辨率，像素）
double fx = P1.at<double>(0,0);
double fy = P1.at<double>(1,1);
double cx = P1.at<double>(0,2);
double cy = P1.at<double>(1,2);

// 基线（米）——注意 P2 的平移单位跟 T 一致
double baseline_m = std::abs(P2.at<double>(0,3) / P2.at<double>(0,0));

// 处理尺度（例如 1280x720 -> 640x480 时 scale=0.5）
double scale = (double)proc_w / image_width;
double fx_proc = fx * scale;

// 视差(像素) -> 深度(米)
//   Z = fx * B / d      （全分辨率）
//   Z = fx_proc * B / d_proc
// ★ 缩放后基线不变、fx 同比例缩小，所以 Z 结果一致 —— 这一点必须靠实测验证
```

## 3. 生成方最小示例（Python）

```python
import cv2
fs = cv2.FileStorage("stereo_calib.yml", cv2.FILE_STORAGE_WRITE)
fs.write("image_width", 1280)
fs.write("image_height", 720)
fs.write("K_left", K1)        # numpy (3,3) float64 直接写
fs.write("D_left", D1)        # (1,5)
fs.write("K_right", K2)
fs.write("D_right", D2)
fs.write("R", R);  fs.write("T", T)
fs.write("R1", R1); fs.write("R2", R2)
fs.write("P1", P1); fs.write("P2", P2)
fs.write("Q", Q)
fs.write("rms", float(rms))
fs.write("baseline_m", float(np.linalg.norm(T)))
fs.write("calib_date", "2026-10-24T15:00:00+08:00")
fs.release()
```

## 4. 消费方最小示例（C++）

```cpp
cv::FileStorage fs(path, cv::FileStorage::READ);
if (!fs.isOpened()) return false;
fs["image_width"] >> calib.image_size.width;
fs["image_height"] >> calib.image_size.height;
fs["K_left"]  >> calib.K_L;   // cv::Mat 直接读
// ...
calib.fx = calib.P_L.at<double>(0,0);
calib.baseline_m = std::abs(calib.P_R.at<double>(0,3) / calib.P_R.at<double>(0,0));
calib.valid = true;
```

## 5. 没有标定文件时怎么办

`StereoPipeline::initIdeal()` 走「理想平行双目」兜底：
- 用 `hfov_deg` 推 `fx = (W/2) / tan(hfov/2)`，`cx=W/2, cy=H/2`
- 畸变全 0，`R=I`，`T=(-baseline,0,0)`，`R1=R2=I`，`P1=[K|0]`，`P2=[K|-fx*B,0,0]`
- `calib().source = "SYNTHETIC/UNCALIBRATED"`

⚠️ 这条路径下：
- **相对深度可信**（同一个物体远近对比是对的）
- **绝对距离不可信**（基线是猜的、镜头有畸变、两摄像头不同型号）
- 网页 HUD 必须显著提示 `UNCALIBRATED`
