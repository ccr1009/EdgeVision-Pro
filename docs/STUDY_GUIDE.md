# EdgeVision-RK3588: 电子信息研究生开发与学习映射指南

本指南专为电子信息专业研一同学编写，将计算机视觉与嵌入式端侧 AI 部署的核心理论映射到本项目的具体代码实现中。

---

## 1. 训练后量化 (Post-Training Quantization, PTQ)
### 理论概念
在端侧边缘芯片（如 RK3588 NPU）上，INT8 整型计算的能效比与乘加单元（MAC）吞吐量通常是 FP32 的 4~8 倍。
PTQ（训练后量化）通过非对称仿射映射（Asymmetric Affine）将浮点张量映射为 8 位整型：
$$X_{	ext{int8}} = 	ext{round}\left(rac{X_{	ext{float}}}{S}ight) + Z$$
其中 $S$ 为比例因子（Scale），$Z$ 为零点偏移（Zero Point）。为了确定最优的 $S$ 与 $Z$，需要使用小批量未打标的代表性校准图像（50~100 张）统计各层激活张量的直方图分布（Min-Max 或 KL 散度截断）。

### 代码映射
- **校准集准备**: `convert/prepare_calib.py` 与 `scripts/prepare_assets.py` 中的 `prepare_calibration_dataset()`。
- **量化配置与构建**: `convert/rknn_convert.py`:
  - `rknn.config(mean_values=[[0,0,0]], std_values=[[255,255,255]], target_platform='rk3588')`: 将输入归一化固化在 NPU 前处理硬件单元中。
  - `rknn.build(do_quantization=True, dataset='dataset.txt')`: 执行直方图统计并生成 INT8 权重与偏置。

---

## 2. YOLOv8 解耦头与嵌入式 NPU 友好性
### 理论概念
原生 YOLOv8 采用了 Distribution Focal Loss (DFL)，其回归输出是一个概率分布，需要进行 Softmax 积分求期望。在 GPU 上，这个操作几乎零开销；但在 NPU 等固定架构算子库中，直接包含复杂 Softmax 积分会导致子图无法融合，退化回 CPU 计算或引入巨大的内存换入换出开销。

### 代码映射
- **ONNX 规范导出**: `convert/export_onnx.py`。
- **C++ 高效解码**: `deploy/cpp/src/postprocess.cpp` 中的 `decode_yolov8_outputs()`：在 CPU 侧直接利用 SIMD / 循环展开进行线性回归与 NMS，保证 NPU 算子图全部为卷积和规整张量操作。

---

## 3. ByteTrack 多目标跟踪与两阶段数据关联
### 理论概念
传统 SORT/DeepSORT 丢弃了低置信度的检测框（认为它们是背景噪声）。然而在真实监控中，目标受到遮挡、运动模糊时，置信度会从 0.8 骤降至 0.2。
ByteTrack 的核心思想是 **"两阶段关联"**：
1. **第一阶段**：使用高置信度框（$\ge 0.45$）与已知轨迹做 IoU 关联。
2. **第二阶段**：将未匹配上的遗留轨迹，与低置信度框（$0.1 \le 	ext{score} < 0.45$）做二次关联，成功找回被遮挡目标，显著降低 ID Switch。

### 代码映射
- **卡尔曼滤波状态更新**: `track/kalman_filter.py`。
  - 8D 状态空间：$[c_x, c_y, a, h, v_x, v_y, v_a, v_h]$。
- **两阶段匹配引擎**: `track/byte_tracker.py` 中的 `BYTETracker.update()`。
- **单测场景覆盖**: `track/test_tracker.py`（平滑追踪、遮挡恢复、交叉无换 ID）。

---

## 4. 生产者-消费者多线程流水线与丢帧保时延设计
### 理论概念
在端侧监控系统中，如果推理速度瞬间落后于相机采集速度，无界队列会导致内存无限堆积（OOM）且导致视频呈现"几秒钟前的画面"（高时延）。
解决此工程问题的标准范式是：
1. **有界队列（Bounded Queue）**：容量限制为 3~5 帧。
2. **丢帧策略（Drop Policy）**：当输入队列满时，丢弃旧帧或跳过当前帧，优先确保当前处理帧具有最新时效性。

### 代码映射
- **有界队列与线程交互**: `pipeline/pipeline_engine.py` 中的 `_capture_worker`、`_inference_worker`、`_tracking_event_worker`、`_render_output_worker`。
- **丢帧保障实现**: `pipeline/pipeline_engine.py` 中 `frame_queue.put(item, block=True, timeout=0.1)` 发生 `Full` 时的弹性丢帧逻辑。
- **延迟打点分析**: `pipeline/latency_tracer.py`（微秒级计时分解与分位数统计）。

---

## 5. C++ 嵌入式工程与 RKNN Zero-Copy 内存概念
### 理论概念
在 Python 层调用驱动，每一次推理都会在用户空间与内核空间之间复制图像内存。在 C++ 部署中，通过 RKNN 提供的 Zero-Copy API（`rknn_create_mem` 配合 DRM/CMA 连续物理内存），可以让摄像头 ISP 直接将解码帧写入 NPU 的共享内存，实现真正的 **零内存拷贝**，极大释放 CPU 负荷。

### 代码映射
- **C++ 封装骨架**: `deploy/cpp/src/rknn_engine.cpp`。
- **跨平台条件编译**: `deploy/cpp/CMakeLists.txt`。
