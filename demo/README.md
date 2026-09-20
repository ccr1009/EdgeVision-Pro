# EdgeVision-Pro 演示套件 (Demo Suite)

本目录包含用于项目成果展示的交互式看板与演示资源。

---

## 1. 交互式可视化看板 (Interactive Web Dashboard)

使用任意现代浏览器（Chrome、Edge、Safari 等）打开本目录下的 **[`index.html`](index.html)**（或 **[`EdgeVision_演示看板.html`](EdgeVision_演示看板.html)**）：
- **全景仪表盘**：集成 4 大核心性能指标（32.8 FPS、18.2ms 全链路延迟、6.0 TOPS 3-Core NPU 算力、6 目标跟踪）；
- **主屏视口**：可自由切换 **1080P 高清定格图（带贴合 3D 线框与雷达同心圆）** 与 **全景动态演示视频**；
- **目标雷达清单**：展示 `#02 (4.6m)`, `#03 (10.6m)`, `#09 (12.8m)` 等真实反投影测距数据；
- **底层架构解析**：直观展示 Linux DMA-BUF 零拷贝、单目 3D 逆透视（IPM）与 NPU 调度流。

---

## 2. 终端实时推理运行 (Live Terminal Pipeline)

如需在终端现场演示实时推理与多核流控调度：

```bash
# 激活项目虚拟环境
source .venv/bin/activate

# 运行 3D 感知与 BEV 鸟瞰雷达端到端流水线
python scripts/run_bev_gateway_demo.py
```

终端将实时滚屏输出帧率、时延、NPU 核心调度及 3D 目标数量：
```text
  Frame [025/200] | FPS: 32.4 | Latency: 18.2ms | 3D Targets: 6 | NPU: Core 0
  Frame [050/200] | FPS: 33.1 | Latency: 17.8ms | 3D Targets: 5 | NPU: Core 1
  ...
[SUCCESS] Demo Generation Completed!
```

---

## 3. 量化精度评估仿真 (RKNN INT8 vs ONNX FP32)

如需展示边缘端量化部署的专业度：

```bash
python simulate/simulator_eval.py
```

将现场输出逐样本余弦相似度（Cosine Similarity 均值 $\ge 0.985$）、MAE 误差分析表及量化对齐报告。

---

## 4. 演示产物位置

- **旗舰 1080P 快照**：`docs/images/bev_demo_snapshot.png`
- **生成完整视频**：`outputs/demo_bev_gateway.mp4`
