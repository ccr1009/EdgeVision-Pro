# EdgeVision-Pro 演示套件 (Demo Suite)

本目录包含用于项目成果展示的交互式看板、视频与实时摄像头演示资源。

---

## 1. 电脑摄像头实时 3D 感知演示 (Live PC Webcam Demo) ⭐ 最震撼互动

使用笔记本自带摄像头（或外接 USB 摄像头），实时识别人体与物体，并在画面右上角绘制**实时桌面级 BEV 鸟瞰雷达地图与空间测距**：

### 运行方式（Windows）
- 进入项目目录 `demo/`，双击 **`运行摄像头DEMO.bat`**（或 `run_webcam_demo.bat`）；
- 或在命令行执行：
  ```cmd
  py -3.12 demo\webcam_demo.py
  ```

### 交互操作快捷键
- **`Q` 或 `ESC`**：优雅退出演示；
- **`S`**：截屏并将当前 3D+BEV 渲染画面保存至 `outputs/` 目录；
- **`空格键`**：暂停 / 继续实时画面。

---

## 2. 交互式可视化看板 (Interactive Web Dashboard)

使用任意现代浏览器（Chrome、Edge 等）直接双击打开本目录下的 **[`index.html`](index.html)**（或 **[`EdgeVision_演示看板.html`](EdgeVision_演示看板.html)**）：
- **全景仪表盘**：集成 4 大核心性能指标（32.8 FPS、18.2ms 全链路延迟、6.0 TOPS 3-Core NPU 算力、6 目标跟踪）；
- **主屏视口**：自由切换 **1080P 高清定格图（带贴合 3D 线框与雷达同心圆）** 与 **全景动态演示视频**；
- **目标雷达清单**：展示 `#02 (4.6m)`, `#03 (10.6m)`, `#09 (12.8m)` 等真实反投影测距数据；
- **底层架构解析**：直观展示 Linux DMA-BUF 零拷贝、单目 3D 逆透视（IPM）与 NPU 调度流。

---

## 3. 终端多车道流式推理 (Highway Traffic Pipeline)

在终端现场演示多车道高速公路场景的流式处理与 NPU 核心轮转调度：

```bash
cd ~/projects/edgevision-rk3588
.venv/bin/python scripts/run_bev_gateway_demo.py
```

终端将实时滚屏输出帧率、时延、NPU 核心调度及 3D 目标数量。

---

## 4. 量化精度评估仿真 (RKNN INT8 vs ONNX FP32)

在终端输出量化精度对齐表：

```bash
.venv/bin/python simulate/simulator_eval.py
```

逐样本评估余弦相似度（Cosine Similarity 均值 >= 0.985）与 MAE 误差。
