# EdgeVision-RK3588: 性能基准与精度评估报告

> **重要声明**：
> 本项目严格遵循工程求真原则，所有数字都标注了**测量边界**（测的到底是什么）。
> - **x86 PC 段**（第 1、2.1 节）：ONNXRuntime CPU 端的参考值，**不是** NPU 数据；
> - **RK3588 板端段**（2.2 节）：2026-10-17 在 EVM3588-B 真机上实测，含原始测法与复现命令；
> - **未测的项一律标「待测」**，不用理论值冒充实测值。官方理论值单列在 2.2.4，与本机实测对照。


---

## 1. 模型精度与量化损失对比 (x86 Simulator 实测)

基于 Rockchip RKNN-Toolkit2 (v1.6.0) 软件仿真器，在 60 张校准图像后量化对比：

| 测试项 | 浮点基线 (ONNX FP32) | 量化模型 (RKNN INT8 Sim) | 评估指标 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| **特征图余弦相似度** | 1.00000 | **0.99651** | >= 0.9800 | **PASS** |
| **平均绝对误差 (MAE)** | 0.00000 | **0.41800** | < 1.0000 | **PASS** |
| **目标检出框一致性** | 100% 对应 | 100% 对应 (低阈值边界微调) | 框数一致 | **PASS** |
| **模型文件大小** | 12.2 MB | **6.2 MB (-49.2%)** | 显著减少内存占用 | **PASS** |

---

## 2. 流水线延迟分解表 (实测 vs 板端占位)

### 2.1 x86 CPU 仿真端实测 (AMD Ryzen 9 8940HX / WSL2)
- **输入源**: 640x640 @ 30fps 视频流
- **推理后端**: ONNXRuntime (CPUExecutionProvider)
- **处理总帧数**: 300 帧

| 流水线阶段 | 平均耗时 (ms) | p50 耗时 (ms) | p95 耗时 (ms) | p99 耗时 (ms) | 占整体延迟比例 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1. 视频采集 (Capture)** | 0.74 | 0.38 | 1.84 | 4.87 | 1.4% |
| **2. YOLOv8 推理 (Infer)** | 52.59 | 52.23 | 55.61 | 64.97 | **98.0%** |
| **3. ByteTrack 多目标跟踪** | 0.11 | 0.10 | 0.13 | 0.19 | 0.2% |
| **4. 电子围栏越线告警** | 0.05 | 0.04 | 0.07 | 0.10 | 0.1% |
| **5. 画面渲染与视频编码** | 0.21 | 0.05 | 0.16 | 5.29 | 0.3% |
| **端到端单帧延迟** | **53.69 ms** | **52.87 ms** | **60.62 ms** | **67.15 ms** | 100% |
| **单路有效吞吐** | **19.0 FPS** | - | - | - | - |

---

### 2.2 RK3588 真机实测数据（EVM3588-B，2026-10-17）

#### 测试环境（实测采集，非宣称值）

| 项 | 实测值 | 采集方式 |
| :--- | :--- | :--- |
| 硬件 | EVM3588-B / SOM3588-B，8GB LPDDR5 | `free -h` |
| 系统 | Debian 11 (bullseye)，kernel **5.10.110** | `uname -r` |
| NPU 驱动 | **0.9.2** | `cat /sys/kernel/debug/rknpu/version` |
| RKNN 运行时 API | **1.5.2 (c6b7b351a@2023-08-23)** | `rknn_query(RKNN_QUERY_SDK_VERSION)` |
| NPU 主频 | **1.0 GHz**（满载，未降频） | `/sys/class/devfreq/fdab0000.npu/cur_freq` |
| DDR 主频 | 2112 MHz | `/sys/class/devfreq/dmc/cur_freq` |
| 模型 | `yolov8n_rk3588_i8.rknn`，6.22 MB | 文件系统 |
| 模型输入 | **1×640×640×3，INT8，NHWC，zp=−128，scale=1/255** | `rknn_query(INPUT_ATTR)` |
| 待机温度 | SoC 37.9°C / NPU 37.0°C | 7 个 thermal_zone |

> **测量边界（必须一起讲）**：以下 2.2.1 / 2.2.2 的延迟是**「推理引擎边界」**，即
> `rknn_inputs_set + rknn_run + rknn_outputs_get` 的全周期，输入为**合成的 640×640 灰图**。
> **不含**视频解码、RGA 缩放、后处理 NMS 与画面绘制；**不是**端到端视频链路延迟。
> 这段链路才是 2.1 节 x86 端 53.69 ms 的口径，两者**不可直接对比**。

测量工具（本次新增，源码在 `deploy/cpp/bench/`）：

```bash
# 板端原生编译（不要交叉编译）
B=/root/edgevision/deploy/cpp/3rdparty/rknn_api
g++ -O2 -std=c++17 bench_npu.cpp -I$B/include -L$B/lib -lrknnrt -o bench_npu
g++ -O2 -std=c++17 bench_multicore.cpp -I$B/include -L$B/lib -lrknnrt -pthread -o bench_multicore
export LD_LIBRARY_PATH=$B/lib
./bench_npu       /root/edgevision/data/models/yolov8n_rk3588_i8.rknn 200
./bench_npu       /root/edgevision/data/models/yolov8n_rk3588_i8.rknn 1 --perf   # 逐层剖析
./bench_multicore /root/edgevision/data/models/yolov8n_rk3588_i8.rknn 100
```

#### 2.2.1 核心绑定对比（单 context，200 次/组）

| 核心绑定 | mean (ms) | p50 (ms) | p95 (ms) | p99 (ms) | 单路 FPS |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 单核 Core0 | 37.19 | 37.07 | 37.30 | 40.28 | 26.9 |
| 单核 Core1 | 37.32 | 37.23 | 37.45 | 39.67 | 26.8 |
| 单核 Core2 | 37.36 | 37.24 | 37.63 | 40.50 | 26.8 |
| 三核 mask `CORE_0_1_2` | **35.97** | 35.86 | 36.04 | 39.85 | **27.8** |
| `RKNN_NPU_CORE_AUTO` | 37.27 | 37.12 | 37.40 | 40.33 | 26.8 |

**结论 1：`rknn_set_core_mask(CORE_0_1_2)` 对单次推理几乎无收益（仅 −3.3%）。**
三颗核轮流/分段跑同一个模型，但单帧内的计算依赖是串行的，核间调度收益被同步开销吃掉。
另外喂数据方式也**不影响**结果：INT8 原生直喂（`pass_through=1`）37.05 ms vs UINT8 转换 37.19 ms，
说明 **CPU 侧的输入格式转换不是瓶颈**（每帧仅 1.17 MB，可忽略）。

#### 2.2.2 三核真并行（3 个独立 context，每线程绑 1 核，各 100 次）

| 配置 | 单帧延迟 | 聚合吞吐 | 加速比 |
| :--- | ---: | ---: | ---: |
| 单线程基线（仅 Core0） | 37.27 ms | 26.8 FPS | 1.00x |
| 3 线程 / 3 context（Core0+1+2） | 40.8 ms/路 | **59.3 FPS** | **2.21x** |

- 墙钟 5058 ms 完成 300 帧；三路各自 4081~4089 ms，**负载几乎完全均衡**（差 8 ms）。
- 并行期间 `rknpu/load` 采样：**三核各 ~49%**（`Core0: 49%, Core1: 49%, Core2: 50%`）。

**结论 2：三核真并行的真实基线是 59.3 FPS（2.21x），不是线性 3x。**
而且三核跑满时每颗核只有 **49% 占用** —— 说明瓶颈不在 NPU 算力，NPU 有一半时间在等。

#### 2.2.3 逐层耗时剖析：49% 的时间根本没在 NPU 上

`./bench_npu <model> 1 --perf`（`RKNN_FLAG_COLLECT_PERF_MASK`）拆出 125 个算子：

| 执行单元 | 算子数 | 合计耗时 | 占比 |
| :--- | ---: | ---: | ---: |
| NPU | 115 | 19.23 ms | 51.0% |
| **CPU（回退）** | **10** | **18.44 ms** | **49.0%** |
| 合计 | 125 | 37.67 ms | 100% |

最慢的 8 个算子：

| 算子 | 执行单元 | 耗时 | 所属 |
| :--- | :--- | ---: | :--- |
| Transpose | **CPU** | 9181 us | `model.22/dfl/Transpose` |
| exSoftmax13 | **CPU** | 4467 us | `model.22/dfl/Softmax` |
| Reshape | **CPU** | 2660 us | `model.22/Concat_3_swap_concat_reshape` |
| ConvExSwish | NPU | 1857 us | `model.0/conv`（首层，3 通道） |
| Concat | NPU | 1687 us | `model.22/Concat_3` |
| Transpose | NPU | 1475 us | `model.22/dfl/Transpose_1` |
| Reshape | **CPU** | 941 us | `model.22/Concat_3_0_unsqueeze1` |
| Reshape | **CPU** | 765 us | `model.22/Concat_0_unsqueeze0` |

**结论 3：慢的根源是 YOLOv8 的 DFL 检测头没被 NPU 融合，回退到 CPU 上做
Transpose + Softmax + Reshape，单独吃掉 18.44 ms（49%），比整个 backbone 的 NPU 时间还接近。**

这解释了为什么单帧 37 ms 而不是官方理论 5~7 ms：**NPU 主体（19.23 ms）本身也偏低效
（首层 3 通道卷积 MAC 利用率仅 2.33%，大量 Conv 在 5%~22%），但主矛盾是那 49% 的 CPU 回退。**

#### 2.2.4 与官方理论值的差距（诚实对照）

| 指标 | 官方/理论 | 本机实测 | 差距原因 |
| :--- | :--- | :--- | :--- |
| 单核单帧 INT8 | 5.5~7.0 ms | 37.19 ms | 49% CPU 回退 + NPU 侧低 MAC 利用率 |
| 三核并行吞吐 | 120+ FPS | 59.3 FPS | 同上；且三核负载仅 49%，NPU 未被喂饱 |
| 4 路 1080P@30 实时 | 可满足 | **待测**（板上无可用视频解码链路） | 需先接 MPP 硬解码 + RGA 缩放 |
| 端到端视频延迟 | — | **待测** | 需真实 rtsp/mp4 链路 |

#### 2.2.5 下一步优化路线（按收益排序，均已定位到具体算子）

1. **改模型导出**（预计 −15 ms 以上）：导出 ONNX 时**砍掉 DFL/Concat 头**，只输出 3 路
   raw 特征图（stride 8/16/32，64 通道），DFL 解码 + NMS 放到 C++ 里做（8400×84 的向量化
   解码在 A76 上约 1~2 ms）。这一步直接把 18.44 ms 的 CPU 回退消掉。
2. **重转模型时开 `optimization_level` / 试 `quantized_algorithm`**，改善 NPU 侧低 MAC 利用率。
3. **修完上面两项再上 RGA/DMA-BUF 零拷贝**：目前输入转换不是瓶颈（见 2.2.1），
   先别在零拷贝上花时间——这是实测给出的优先级，与「听起来更高级」的直觉相反。
4. 补齐 4 路 1080P 压测：需要 MPP 硬解 + RGA 缩放的真实链路，届时再填 2.2.4 的空格。

