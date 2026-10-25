# stereo3d-rk3588 —— 双目 3D 感知 / 人脸 3D 定位

> RK3588 端侧**双目立体视觉**项目：双 USB 摄像头 → 立体矫正 → SGBM 稠密视差 → 深度图 → 人脸框内深度 → 人脸 3D 坐标。
> 原生 C++（无第三方库，只用 OpenCV），板端原生编译，浏览器 MJPEG 实时预览。

## 为什么做这个（校招叙事）

已有项目 `edgevision-rk3588` 是**单目 + NPU 目标检测**，只能给 2D 框。
本项目补上**深度/三维**这一环，形成完整链条：

| 能力 | edgevision-rk3588（单目） | stereo3d-rk3588（双目，本项目） |
|---|---|---|
| 目标位置 | 2D 框 (u,v,w,h) | **3D 坐标 (X,Y,Z) 米** |
| 测距原理 | 无（或框大小估算，不可靠） | **三角测量**：`Z = fx·B / d` |
| 数据规模 | 单张 2D 图 | **稠密点云**（每帧 ~10 万点） |
| 面试考点 | INT8 量化、NPU 调度、零拷贝 | **对极几何、立体矫正、SGBM 半全局匹配、三角测量误差传播** |

两句话讲清价值：
1. **测距精度可量化**：`ΔZ = Z²/(fx·B) · Δd` —— 视差差 1 像素，3 米误差多少厘米，能当场算。
2. **工程可落地**：RK3588 上真实双摄全链路（采集→矫正→SGBM→人脸3D→网页推流）已跑通，
   640×360 SGBM 实测 46ms（**已是 TBB 并行态真实成本**，非单线程；参数消融可降到 36.2ms），全链路 9.7~11 FPS。

## 硬件

| 项 | 值 |
|---|---|
| 板子 | EVM3588-B（RK3588，8 核 A76×4 + A55×4，8GB） |
| 系统 | Debian 11，OpenCV **4.5.1**（含 calib3d / objdetect），g++ 10.2.1 |
| 左目 | `/dev/video81`（HDI Webcam USB，MJPG 1280×720@30） |
| 右目 | `/dev/video83`（同型号） |
| SSH | `ssh root@<board-ip>`（免密；板子 IP 见本地私密记录，不入库） |

## 目录结构

```
stereo3d-rk3588/
├── CMakeLists.txt
├── include/
│   ├── stereo_types.h      # 全项目共享数据结构（契约）
│   ├── stereo_pipeline.h   # 矫正 + SGBM + 深度 + 点云
│   ├── face_3d.h           # Haar 人脸 + 框内深度 → 3D
│   ├── http_server.h       # MJPEG 网页预览
│   └── app_options.h
├── src/                    # 对应实现
├── tools/
│   ├── probe_dual_cam.cpp        # 可行性探针（先量数据再动手）
│   ├── gen_synthetic_stereo.cpp  # 合成双目 + 已知真值自检
│   ├── capture_pairs.cpp         # 板端双摄同步抓图
│   └── calib/                    # PC 端标定工具链
├── docs/
│   ├── CALIB_FORMAT.md     # stereo_calib.yml 键名硬契约
│   └── ...
└── calib/                  # 标定产物 stereo_calib.yml 放这里
```

## 快速开始

```bash
# 0) 板端编译
ssh root@<board-ip>
cd /root/stereo3d
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4

# 1) 没标定也能跑（理想平行双目兜底，只能看相对深度）
./build/stereo3d_demo --no-calib

# 2) 标定后跑（推荐）
./build/stereo3d_demo --calib calib/stereo_calib.yml

# 3) 浏览器打开
#    http://<board-ip>:8081
```

## 核心公式（面试要能推）

### 三角测量
```
Z = fx · B / d
  Z: 深度（米）    fx: 矫正后焦距（像素）
  B: 基线（米）    d: 视差（像素）
```

### 误差传播（一句话就能镇场）
对 `Z = fx·B/d` 求偏导，视差量化误差 `Δd = 1px` 时：
```
ΔZ = Z² / (fx · B) · Δd
```
代入 `fx=550px, B=0.06m`：

| 真实距离 Z | 1 像素视差误差对应的深度误差 |
|---|---|
| 1 m | 3.0 cm |
| 2 m | 12.1 cm |
| 3 m | 27.3 cm |
| 5 m | 75.8 cm |

**结论**：双目测距误差随距离**平方增长**，所以消费级双摄只在 3 米内靠谱。
这句话能把「为什么手机双摄只用来做人像虚化/近距测距」讲透。

## 实测数据（2026-10-24 真机，全链路分段计时）

| 阶段 | 耗时 | 备注 |
|---|---:|---|
| 双摄采集 1280×720 | 16.3 ms | 两颗先 grab 再 retrieve 压同时差 |
| 立体矫正 remap×2 | 5.4 ms | CV_16SC2+16UC1 定点映射表 |
| **SGBM 视差 640×360** | **46.1 ms** | **当前瓶颈**；已确认是 TBB 并行态真实成本（parallel_for_ 探针 4.10×），nd=32 可降到 36.2ms |
| 深度换算 | 0.6 ms | Z = fx·B/d |
| Haar 人脸+3D 定位 | 20.6 ms | 含直方图均衡 |
| 画图+JPEG 编码 | ~12.5 ms | 2×2 拼图 + q80 |
| **全链路** | **~102 ms ≈ 9.7~11 FPS** | 222 帧平均 9.68 FPS |

- 合成真值自检：1/2/3m 层深误差 3.94 / 21.99 / 6.14 mm（详见 `docs/BENCHMARKS.md`）。
- 人脸框内深度**中位数 vs 均值**对照：20% 噪声注入下均值偏 −157mm，中位数误差 0。
- 局限：当前未标定（理想平行双目假设），绝对距离不可信；标定工具链在 `tools/calib/`。

## 文档

- `docs/CALIB_FORMAT.md` —— 标定文件契约
- `docs/BENCHMARKS.md` —— 板端实测性能与精度
- `docs/PIPELINE.md` —— 数据流与线程模型
- `docs/LEARNING_NEXT.md` —— 你自己动手要补的知识点（学习向）

## 状态

⚠️ **诚实声明**：本项目当前的绝对测距精度依赖标定质量。
- **未标定**（`--no-calib`）：只能看**相对**深度，HUD 会显示 `UNCALIBRATED`，绝对距离不可信。
- **已标定**：需要用户自己打印棋盘格跑一遍 `tools/calib/`，RMS 与基线都打出来才算数。
