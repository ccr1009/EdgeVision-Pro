# EdgeVision-RK3588: 性能基准与精度评估报告

> **重要声明**：
> 本项目严格遵循工程求真原则，所有数字都标注了**测量边界**（测的到底是什么）。
> - **x86 PC 段**（第 1、2.1 节）：ONNXRuntime CPU 端的参考值，**不是** NPU 数据；
> - **RK3588 板端段**（2.2 节）：2026-10-17 在 EVM3588-B 真机上实测，含原始测法与复现命令；
> - **端侧 Agent 段**（2.7 节）：2026-10-25 实测，**测的是自然语言任务端到端时延，不是 NPU 推理延迟**，两者不可比较；
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

---

### 2.3 DFL-Free 模型改造（2026-10-21，进行中）

#### 2.3.1 做了什么

| 步骤 | 产物 | 验证方式 | 结果 |
| :--- | :--- | :--- | :--- |
| ONNX 图手术砍 DFL 头 | `data/models/yolov8n_dflfree.onnx` | 节点 262 → 228；numpy 参考解码 vs 原模型输出逐元素对比 | box **max\|Δ\|=0.000122 px**，cls **max\|Δ\|=0**（float32 精度极限） |
| C++ 侧 DFL 解码器 | `deploy/cpp/src/postprocess_dfl_free.cpp` | 板端 g++ 编译，对比 numpy 参考结果 | 坐标 **max\|Δ\|=6.1e-05 px**，框数/类别完全一致 |
| 解码性能优化 | 同上 | 板端 20 次平均 | 3.19 ms → **1.79 ms/帧**（类别外循环改善 cache 命中，1.78x） |

**切点定义**（关键，换模型版本要对齐）：

| 输出 | 图内来源 | 形状 | 含义 |
| :--- | :--- | :--- | :--- |
| `raw_box` | `/model.22/Concat_output_0`（DFL 的 Reshape 节点输入） | (1, 64, 8400) | 3 尺度 box 分布 concat，**未 softmax** |
| `cls_sig` | `/model.22/Sigmoid_output_0` | (1, 80, 8400) | cls 概率（模型内已 sigmoid） |

**解码单位踩坑（务必记住）**：DFL 的 16 个 bin 加权和得到的是**格子单位**距离（0~15），
必须乘 stride 才是像素。第一版漏了这一步，解码结果整好差 8 倍（stride=8 那一路）。

#### 2.3.2 真机实测收益（200 次，EVM3588-B，合成灰图输入，与 2.2 同口径）

| 阶段 | 原模型 (84ch) | DFL-Free 模型 | 变化 |
| :--- | ---: | ---: | :--- |
| 纯推理 `rknn_run` | 37.19 ms | **15.95 ms** | **−57.1%** |
| IO 周期 (set+run+get) | 37.5 ms（估） | **17.48 ms** | −53% |
| CPU 后处理 | ~0.3 ms（84×8400） | **3.09 ms**（DFL 解码 + NMS） | +2.8 ms |
| **单帧全链路** | **37.19 ms / 26.9 FPS** | **20.58 ms / 48.6 FPS** | **−44.7% / +81%** |
| 模型体积 | 6.22 MB | **4.34 MB** | −30.2% |

延迟分解（新模型）：`15.95 (NPU) + 1.53 (IO 搬运) + 3.09 (DFL+NMS) = 20.58 ms`。

> 关键结论：**花的 3.09 ms 解码钱，换回了 21.24 ms 的 NPU 侧时间**。
> 原模型那 18.44 ms 的 CPU 回退不只是计算慢，它还卡在 NPU 的同步点上，
> 让整个 `rknn_run` 变成串行等待。砍掉后 NPU 主体从 37 ms 掉到 16 ms —— 
> **降幅 57% 大于 DFL 那 18.44 ms 本身**，说明原来的 DFL 回退还额外拖累了 NPU 的调度。

> 与官方理论 5.5~7 ms 仍有 2.3~2.9 倍差距，差距已不在 DFL，而在 2.2.3 指出的
> NPU 侧低 MAC 利用率（首层 3 通道卷积 2.33%、大量 Conv 5%~22%），这是下一阶段的事。

#### 2.3.3 量化精度：校准集是决定性主因（对照实验实证）

**实验设计**（干净对照，只改一个变量）：
同一份 `yolov8n_dflfree.onnx`、同一套 toolkit 2.3.0、同一个算法 `normal`、同一台板子，
**只换校准集**：

| 组 | 校准集 | 内容 |
| :--- | :--- | :--- |
| OLD | `data/calib/` 60 张 | 合成色块动画抽帧（纯色矩形 + 纯灰底） |
| NEW | `data/calib_v2/` 170 张 | 50 张板端双摄实拍 + 100 张光照/对比度/伽马/噪声增强 + 20 张合成 |

测试：`data/calib_v2/real_000/003/…/024` 共 9 张**真实宿舍场景**（cam1 原图，非增强变体），
同一张图先用 FP32 ONNX 出基准，再分别过两个 INT8 模型。

**结果：**

| 模型 | 检出率 | 平均 top-1 分数 | 与 FP32 偏差 |
| :--- | :---: | ---: | ---: |
| FP32 ONNX（基准） | 9/9 | **0.8125** | — |
| **NEW**（真实校准集） | 9/9 | **0.8072** | **−0.0053 (−0.65%)** |
| OLD（合成校准集） | 9/9 | **0.5000** | **−0.3125 (−38.5%)** |

**关键现象：OLD 组 9 张图的分数全部精确等于 0.5000。**
这不是巧合 —— `0.5 = sigmoid(0)`，说明 cls 分支的输出被量化后**整体塌缩到了 0 附近**（常数），
分类头事实上完全失效；框还能检出来，是因为 box 分支（回归）没坏，框坐标与 FP32 差 < 1 px。

这与「校准集是纯色块、里面根本没有人」完全吻合：cls 分支在标定时从没见过"有人"的激活值，
量化 scale 被定在一个极窄的区间，真实画面一进来就整体饱和。

**结论：**
1. **校准集决定了分类分支的死活**，比算法（normal vs mmse）重要得多。
   在本次对照里，换校准集把 top-1 分数从 0.5000 拉回到 0.8072（**+61%**），逼近 FP32 的 0.8125。
2. 之前那个 84ch 的旧模型 `yolov8n_rk3588_i8.rknn` 在 2.3.0 runtime 下**一张都检不出**
   （阈值降到 0.05 仍然 0 个），它同时叠了两个问题：合成校准集 + toolkit 1.6.0 与 runtime 2.3.0
   版本倒挂，输出已不可信 —— 不能拿它做基线。
3. INT8 的代价：平均分数 −0.65%，框坐标偏差 < 1 px。这个量级对边缘检测任务是可接受的。

> 测量边界：9 张静态图、单一场景（宿舍）、top-1 分数统计，**不是 mAP**。
> 要给出严格的 mAP 需要带标注的验证集，目前项目里没有，因此不写 mAP 数字。

#### 2.3.4 转换环境的意外解法：直接在板子上转（不需要 WSL / PC）
原判断「toolkit 只有 Linux x86_64 wheel、Windows 无版本」是对的，但漏了一层：
**rknn-toolkit2 2.x 的 `packages/arm64/` 下有完整的 toolkit2 wheel**（不是 lite2）：

```
rknn_toolkit2-2.3.0-cp39-cp39-manylinux_2_17_aarch64.manylinux2014_aarch64.whl
```

板子 Debian 11 自带 Python **3.9.2**，正好是 cp39。于是整个转换在板子上就完成了，
**与 WSL 是否被禁用无关**。这条路对读者也更友好：不用配交叉环境，一块板子自给自足。

实测踩坑清单（每一条都是真踩过的）：

| # | 坑 | 表现 | 解法 |
| :-- | :--- | :--- | :--- |
| 1 | 板子没有 pip | `No module named pip`，`ensurepip` 也被 Debian 剥离 | 手动下 pip wheel 解压到 site-packages（`bootstrap.pypa.io` 不通） |
| 2 | pip 源被限流 | 清华源返回封禁页（带 identifier） | 换阿里云 `mirrors.aliyun.com/pypi/simple` |
| 3 | `rknn.api` 顶层 import torch | aarch64 torch wheel 1GB+，200KB/s 下不现实 | 用 numpy 支撑的最小 torch stub（tk2 只用它生成索引常量，见 `--stub-torch`） |
| 4 | 依赖版本必须精确 | numpy 2.x / onnx 1.19 / protobuf 6.x 装上后运行即崩 | **numpy 1.26.4 / onnx 1.14.1 / protobuf 3.20.3 / onnxruntime 1.16.0 / scipy 1.13.1 / opencv 4.9.0.80** |
| 5 | opencv 5.x 与 numpy 1.26 冲突 | pip 在 5.0→4.14→4.13 间反复回退下载，浪费十几分钟 | 直接钉死 `opencv-python-headless==4.9.0.80` |
| 6 | `quantized_iteration` 已废弃 | 1.x 参数，2.x 传了直接 `ValueError` | 去掉 |
| 7 | dataset.txt 相对路径 | `calib/xxx.jpg` 被拼成 `.../calib/calib/xxx.jpg` | **必须写绝对路径**（相对路径按清单所在目录解析） |
| 8 | 2.3.0 模型要 2.3.0 runtime | 版本倒挂 | 板端 `librknnrt.so` 升 2.3.0；实测在 **NPU 驱动 0.9.2** 上可用，旧模型复测 36.87 ms 无回退 |

转换耗时：**约 3.5 分钟**（161 层 × 3 轮量化，A76 上单线程）。
产物 `yolov8n_dflfree_rk3588_i8.rknn` = **4.34 MB**，两个输出：
`out[0] (1,64,8400) raw_box`、`out[1] (1,80,8400) cls_sig`（RKNN 保持 NCHW，未 NHWC 化）。

---

### 2.4 量化掉点：COCO128 上的真 mAP@0.5（2026-10-21）

> **这一节是为了替换一个不存在的指标。** 简历 v10 曾写「自建 500 张实景验证集上 mAP@0.5 掉点 <0.8%」，
> 但项目里既没有标注文件、也没有 mAP 计算代码（`simulate/simulator_eval.py` 只算余弦/MaxAE/MAE/IoU），
> 本节把这件事**真正做出来**，用公开带标注数据集，随时可复现。

#### 2.4.1 评估口径

| 项 | 设置 |
| :--- | :--- |
| 数据集 | **COCO128**（ultralytics 官方发布，128 张 + YOLO 格式标注），本地 `data/coco128/` |
| 规模 | 128 张图 / **929 个 GT 框** / 71 个类别出现 |
| 预处理 | letterbox 到 640（等比缩放 + 灰边 114），坐标逆变换回原图 |
| 推理阈值 | conf=0.01（mAP 需保留低分预测）、NMS IoU=0.45、class-aware |
| AP 口径 | COCO 101 点插值，IoU 阈值 0.5 |

两侧口径**完全一致**：PC 用 `convert/eval_map.py`，板端用 `deploy/cpp/bench/eval_dump.cpp`
导出预测 txt，再回到同一个脚本算 mAP（同一个 GT、同一个 AP 实现）。

#### 2.4.2 结果

| 模型 | mAP@0.5 | 相对 FP32 | 说明 |
| :--- | ---: | ---: | :--- |
| FP32 ONNX（基准） | **0.6087** | — | ONNXRuntime CPU，PC 侧 |
| INT8 RKNN（板端） | **0.6002** | **−1.4%** | 绝对掉点 **0.85 个百分点** |

- 结论：**INT8 PTQ 的检测精度代价是 0.85 个 mAP 点（相对 −1.4%）**，属 INT8 后量化的正常水平（一般 1~3%）。
- 模型体积同时从 12.2 MB 降到 4.34 MB，单帧全链路 37.19 → 20.58 ms，即**用 1.4% 的精度换 45% 的延迟下降**。

#### 2.4.3 局限（必须一起讲，否则数字会被误读）

1. **COCO128 是 YOLOv8n 的训练子集**，模型见过这些图，所以绝对 mAP（0.61）偏乐观，
   不能当作「在 COCO val2017 上 0.61」来宣传（官方 val2017 mAP@0.5 ≈ 0.644，那是没见过的数据）。
2. 但对本节的目的（量化掉点）**影响很小**：FP32 与 INT8 用的是同一模型、同一批图，
   「见过」这一因素在两侧同时存在，相对掉点仍然成立。
3. 仍然是 128 张、单一公开集，**不是自建场景集**。若要评估宿舍/实验室真实场景的绝对精度，
   需要自己标注 200+ 张（工作量约一天），这是后续可做的事，不是现在能编的数。

#### 2.4.4 复现命令

```bash
# PC 侧：FP32 基准
python convert/eval_map.py --model data/models/yolov8n_dflfree.onnx --out data/fp32_preds.txt

# 板端：INT8 导出预测（要先把 data/coco128/images/train2017 传上去）
g++ -O2 -std=c++17 eval_dump.cpp postprocess_dfl_free.cpp \
    -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include -lrknnrt \
    `pkg-config --cflags --libs opencv4` -o eval_dump
./eval_dump /root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn \
            /root/edgevision/data/coco128/images/train2017 > int8_preds.txt

# 回到 PC 用同一份 GT 计算
python convert/eval_map.py --preds data/int8_preds.txt
```


---

### 2.5 预览链路分段剖析：瓶颈早就不在 NPU 了（2026-10-22）

> 之前只知道「除 NPU 外还有 34ms 花在图像处理」，但不知道具体是谁。
> 这次在主循环插了 6 个计时点，把每段真实耗时量出来。
> **结论：MJPG 解码一个人吃掉 22.6ms，比 NPU 还贵；NPU 只占 30%。**

#### 2.5.1 测试条件

| 项 | 说明 |
| :--- | :--- |
| 输入 | UVC `/dev/video81`，MJPG 1280×720 @30fps |
| 预处理 | letterbox → 640×640 → BGR2RGB |
| 模型 | `yolov8n_dflfree_rk3588_i8.rknn`（4.34 MB） |
| NPU | 单 context，`RKNN_NPU_CORE_AUTO`（未绑核） |
| 输出 | 画框 + `cv::imencode` JPEG(quality=80) + HTTP MJPEG |
| 采样 | 每 30 帧打印一次，取稳态后连续 12 次 |

#### 2.5.2 分段耗时（稳态）

| 阶段 | 耗时 (ms) | 占比 | 说明 |
| :--- | ---: | ---: | :--- |
| **采集 + MJPG 解码** | **22.6 ~ 23.3** | **46%** | `cap >> bgr`，OpenCV 纯 CPU libjpeg |
| 预处理 | 1.7 ~ 2.3 | 4% | letterbox + cvtColor |
| **NPU 推理** | **15.1 ~ 16.6** | **30%** | 与 2.3 节 15.95 ms 吻合 |
| DFL 解码 + NMS | 1.3 | 3% | 与 2.3 节 1.79 ms 口径一致 |
| 画框 + HUD | 1.5 ~ 1.6 | 3% | rectangle + putText |
| **JPEG 编码** | **7.1 ~ 7.2** | **14%** | `cv::imencode` quality=80 |
| **合计（1/FPS）** | **49.8 ~ 51.7** | 100% | 实测 HUD 约 20 FPS |

#### 2.5.3 三个结论

1. **瓶颈已从 NPU 转移到 CPU 图像编解码**：NPU 占 30%，
   而 MJPG 解码 + JPEG 编码合计 **29.8 ms（60%）**。
2. **「34ms 花在图像处理」方向对、粒度粗**：34ms 是「除 NPU+解码外的全部」，
   其中约 2/3 是 MJPG 解码，1/6 是编码，缩放+画框加起来不到 4ms。
3. **优化顺序要反过来**：不该先做「零拷贝喂 NPU」（NPU 只占 30%），
   而应先解决**取流解码**这一环。

#### 2.5.4 优化路线（**注意：本节收益估算已被 2.5.7 修正，勿直接引用**）

| 优先级 | 手段 | 原估收益 | 实际状态 |
| :-- | :--- | :--- | :--- |
| ~~P0~~ | ~~MPP 硬解 MJPG~~ | ~~22.6 → ~3ms~~ | ❌ **估算错误**，解码本身只有 7ms，见 2.5.7 |
| **P0（新）** | **查清摄像头为何只有 ~20fps**（AE 曝光 / USB 带宽 / V4L2 参数） | 未知，**可能是最大头** | 待做 |
| P1 | MPP 硬解 MJPG | 解码 7 → ~1.5ms，省 5~6ms | ⚠️ 代码已写，`get_frame` 拿不到帧，**未通** |
| P1 | MPP 硬编 JPEG | 编码 7.2 → ~2ms，省 5ms | 未做 |
| P2 | RGA 硬件缩放 | 2.0 → <1ms，收益小 | 未做 |
| — | DMA-BUF 零拷贝喂 NPU | 喂数据拷贝 <1ms，**收益接近 0** | 不建议做 |

> **P2 与「零拷贝」的陷阱**：网上常把「RGA + 零拷贝」当端侧优化头号手段，
> 那是**NPU 占大头**的场景。本项目 NPU 只占 30%、喂数据前的拷贝 <1ms，
> 做零拷贝是**优化了不存在的问题**。真瓶颈在 MJPG 解码，只能靠 MPP 解决。

#### 2.5.5 复现命令

```bash
cd /root/lab && g++ -O2 -DENABLE_RKNN w4_edgevision_preview_real.cpp \
    /root/edgevision/deploy/cpp/src/postprocess_dfl_free.cpp \
    -o w4_edgevision_preview_real \
    -I/usr/include/opencv4 -I/root/edgevision/deploy/cpp/include \
    -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include \
    -lopencv_core -lopencv_videoio -lopencv_imgproc -lopencv_imgcodecs \
    -lrknnrt -lpthread

timeout 20 ./w4_edgevision_preview_real /dev/video81 0.25 2>&1 | grep profile
```

#### 2.5.6 局限（必须一起引用）

1. 单次运行、单板、室内光照，**未做多次冷热启动对比**。
2. `cap >> bgr` 的 22.6ms **包含等待摄像头出帧的时间**（30fps = 33ms/帧），
   不能全算成解码开销。严格拆分要用 `v4l2` 直接 DQBUF + 单独计时解码。
   但即便只算一半，MJPG 解码仍是最大单项。
3. JPEG 编码固定 quality=80，改质量会改变该项耗时。
4. 单 context 未绑核；绑核后 NPU 项可能再降，但不影响「CPU 编解码是瓶颈」的结论。

#### 2.5.7 重要修正（同日 23:00）：MJPG 解码实测只有 7ms，2.5.2 的归因错了

`cap >> bgr` 那一栏 22.6ms 被我直接当成「MJPG 解码开销」，**这是个错误归因**。
用 `v4l2-ctl` 抓一帧真实 MJPG（40 KB），单独测纯解码：

```bash
v4l2-ctl -d /dev/video81 --set-fmt-video=width=1280,height=720,pixelformat=MJPG \
         --stream-mmap --stream-count=1 --stream-to=/root/lab/sample.mjpg
```

| 解码方式 | 平均耗时（30~100 次） | 输出 |
| :--- | ---: | :--- |
| OpenCV `cv::imdecode`（libjpeg 软解） | **7.04 ms** | 1280×720 BGR |
| MPP 硬解（`MPP_VIDEO_CodingMJPEG`） | **未通**（见下） | NV12 |

**修正后的归因**：

| 项 | 耗时 | 说明 |
| :--- | ---: | :--- |
| MJPG 软解码（真实开销） | **7.0 ms** | 单帧纯解码实测 |
| **等待摄像头出帧** | **~15.6 ms** | `cap >>` 阻塞到下一帧可用，**不是我们能优化的算力问题** |

**由此推出两件事**：

1. **真正的瓶颈是摄像头出帧速率，不是解码**。
   按「处理 34.4ms + 等待 15.6ms ≈ 50ms/帧」反推，**有效帧率约 20 fps，不是标称的 30 fps**。
   低光环境下 UVC 自动曝光（AE）自动延长曝光时间是常见原因，
   也可能与 USB 带宽、V4L2 buffer 数、`CAP_PROP_FPS` 未显式设置有关。
2. **MPP 硬解的理论收益只有 5~6ms**（7.0 → ~1.5ms），**不是原估的 19ms**。
   把「合计降到 30ms」当成目标是不成立的。

**MPP 硬解当前状态（未通，待排查）**：

- `mpp_create` / `mpp_init(MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG)` 均返回 OK
- `decode_put_packet` 返回 OK（未设 EOS 时会失败，说明 EOS 是必需的）
- **`decode_get_frame` 始终拿到 nullptr**
- 已排除：packet 长度未设（已显式 `mpp_packet_set_length`）、缺 buffer group（已 `mpp_buffer_group_get_internal`）、
  split mode（已置 0）、等待时间不足（已放宽到 1 秒）
- 下一步方向：用 MPP 官方 `mpi_dec_test` 二进制先验证板子能否硬解 MJPEG，
  再回头对 API 用法；或改用 `mpp_packet_init_with_buffer` 走 DMA-BUF 路径

**方法论收获（比数据更值钱）**：

> 一个耗时栏里塞了两个动作（等帧 + 解码）时，**不能整体归因给其中任何一个**。
> 必须拆开单独测。我第一版把 22.6ms 全算成解码开销，进而得出「MPP 能省 19ms、
> 性能翻倍」的结论——**这个结论如果写进简历，面试官只要问一句「你怎么测的解码耗时」
> 就会当场崩塌**。第二次用 `v4l2-ctl` 抓帧单独测，才发现解码只有 7ms。

#### 2.5.8 最终结论：瓶颈是摄像头只有 20 fps，不在代码里

> ⚠️ **本节结论已于 2026-10-24 被推翻，见 2.5.9。**
> 「20 fps 是物理上限」是错的：真因是代码从没设 `CAP_PROP_FPS`，
> 摄像头默认协商到 20fps。显式设 30fps 后实测 **33.33 ms/帧，零抖动**。
> 下面的内容和"方法论"保留作**反面教材**——它示范了「默认值当成上限」这个错误。
> 完整排查见 `docs/CAMERA_FPS_ROOT_CAUSE.md`。

`bench_cam_fps.cpp`（只采集 + 解码，不推理不编码）实测：

```
驱动报告的 FPS: 20.0
连续采集 100 帧: 50.02 ms/帧  =>  20.0 fps （含 MJPG 解码）
画面尺寸: 1280x720
```

**摄像头物理出帧率就是 20 fps**，而整条预览链路 50 ms/帧**恰好等于这个周期**。

**推论（推翻了 2.5 节开头的问题设定）**：

1. **不存在「处理太慢拖累帧率」这回事**——处理链路刚好跟得上摄像头，20 FPS 是上限不是下限。
2. **优化任何一环都不会提高 FPS**：MPP 硬解、RGA 缩放、零拷贝、JPEG 硬编，
   在 20fps 输入下全部无感。它们能降低的是**单帧延迟**和 **CPU 占用**，不是吞吐。
3. 真正的优化方向变成**让摄像头多出帧**：
   - 低光下 UVC 自动曝光（AE）延长曝光时间 → 自动降帧（**最可能**）
   - `v4l2-ctl --set-ctrl=exposure_auto=1 --set-ctrl=exposure_absolute=N` 锁定曝光
   - 降低分辨率（720p→480p）换帧率
   - 确认插的是 USB 3.0 口

**验证方法（明天第一件事）**：

```bash
# 1) 加光照，再跑一次
./bench_cam_fps /dev/video81

# 2) 看支持哪些曝光控制
v4l2-ctl -d /dev/video81 --list-ctrls

# 3) 锁定曝光后再测
v4l2-ctl -d /dev/video81 --set-ctrl=auto_exposure=1
v4l2-ctl -d /dev/video81 --set-ctrl=exposure_time_absolute=100
./bench_cam_fps /dev/video81
```

**方法论（今天最值钱的一条，比任何数字都重要）**：

> **先测端到端上限，再拆分段。**
> 我先拆了分段，看到 `cap >> bgr` 占 22.6ms 就认定它是瓶颈，还规划了 MPP 优化路线；
> 结果单独一测纯解码只有 7ms，再一测摄像头本身只有 20fps——**整晚的优化方向都是错的**。
> 正确顺序应该是：先用最简程序测出「这条链路最快能跑多快」，确认上限由谁决定，
> 再决定优化哪里。否则很容易把「等硬件出数据」误算成「软件算得慢」，
> 然后花一周去优化一个不存在的瓶颈。

#### 2.5.9 修正（2026-10-24）：20 fps 不是物理上限，是代码没开口要

**结论：上一节的「摄像头物理出帧率就是 20 fps」是错的。**
显式设置 `cap.set(cv::CAP_PROP_FPS, 30)` 后，两台摄像头都稳定跑 **30.0 fps**。

##### 2.5.9.1 三层排查

**第 1 层 —— 查硬件能力**（这一步上一节漏了）

```bash
v4l2-ctl -d /dev/video81 --list-formats-ext
```

1280x720 MJPG 明确列出 `Interval: Discrete 0.033s (30.000 fps)`。
**硬件支持 30fps，从未有过物理限制。**

**第 2 层 —— 查为什么协商成 20**

```bash
v4l2-ctl -d /dev/video81 --get-parm
# Frames per second: 20.000 (20/1)
```

源码里只设了 `FOURCC` 和分辨率，**没有任何一处设 `CAP_PROP_FPS`**：

| 文件 | 行号 | 设了什么 |
| :--- | :--- | :--- |
| `lab/w4_edgevision_gateway.cpp` | 133-134 | 只设 FOURCC + 分辨率 |
| `lab/w4_edgevision_preview_real.cpp` | 259-260 | 只设 FOURCC + 分辨率 |
| `lab/bench_cam_fps.cpp` | 11-13 | 只设 FOURCC + 分辨率 |

`bench_cam_fps.cpp:15` 的 `printf("驱动报告的 FPS")` 读的是**协商后的当前值 20**，
不是能力上限——这一行是误读的源头。

**第 3 层 —— 显式请求，看能不能兑现**

##### 2.5.9.2 帧率请求 → 实测 对照表（1280x720 MJPG）

| 请求 FPS | 回读 FPS | 实测 ms/帧 | 实测 FPS | 结论 |
| ---: | ---: | ---: | ---: | :--- |
| 不设置 | 20.00 | 50.03 | 20.0 | 复现旧结论 |
| **30** | 30.00 | **33.32** | **30.0** | ✅ **可用** |
| 20 | 20.00 | 50.00 | 20.0 | 可用 |
| 25 | 25.00 | 50.00 | 20.0 | ⚠️ 请求未被兑现 |
| 15 | 15.00 | 33.33 | 30.0 | ⚠️ 跳到 30 |
| 10 | 10.00 | — | 15.0 | ⚠️ 跳到 15 |
| 5 | 5.00 | — | 10.0 | ⚠️ 跳到 10 |

> **「回读值」会骗人**：请求 25fps 时 `cap.get(CAP_PROP_FPS)` 返回 `25.00`，
> 但实测仍是 20.0fps。**回读的是软件层期望值，不是硬件实际节奏。**
> 只信回读就会得出「25fps 能设」的错误结论。

##### 2.5.9.3 长时稳定性验证（400 帧 ≈ 13s，两台都测）

| 摄像头 | 帧数 | 均值 | min | 中位 | p95 | max | 间隔分布 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| cam2 `/dev/video83` | 399 | **33.33 ms** | 32.56 | 33.32 | 33.50 | 34.09 | 399 帧全在 30~40ms 档 |
| cam1 `/dev/video81` | 399 | **33.33 ms** | 33.19 | 33.36 | 33.39 | 33.43 | 399 帧全在 30~40ms 档 |

**零抖动**：最大偏差 0.76ms，是真正稳的 30fps，不是均值凑数。

##### 2.5.9.4 修复

```cpp
cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
cap.set(cv::CAP_PROP_FPS, 30);   // ★ 必须显式设置，否则默认协商到 20fps
```

##### 2.5.9.5 对整条链路的影响（**重要：不要过度乐观**）

| 项 | 值 | 说明 |
| :--- | ---: | :--- |
| 摄像头周期（30fps） | **33.3 ms** | 上限提高后 |
| 单帧处理耗时 | **~34 ms** | NPU 15.1 + 软解 7.0 + 编码 7.2 + 其它 ~4.5 |
| 新的端到端 FPS 预估 | **~20~28 fps** | 处理仍略慢于出帧，**实际需重测** |

**所以「改一行就翻倍」不成立**：处理链路本身需 ~34ms，30fps 下变成**刚好卡在临界点**。
真正的收益是：

1. 摄像头不再是硬上限，**处理链路成为新的瓶颈**；
2. **现在优化处理链路（MPP 硬解 7→1.5ms、硬编 7.2→2ms）第一次变得有意义**——
   在 20fps 上限下这些优化对 FPS 完全无感，现在能真正兑现成帧率提升。

> **这条修正反而让 2.5.4 的优化路线重新成立**：
> MPP 硬解 + 硬编合计可省 ~11ms，34ms → ~23ms，即可稳定吃下 30fps。
> 之前判「MPP 收益只有 5~6ms、不值得」是基于 20fps 上限的错误前提。

##### 2.5.9.6 局限（必须一起引用）

1. 只测「采集+解码」这一段，**接进 preview 后的真实 FPS 未复测**（下一步）。
2. 两台摄像头同型号（HDI Webcam USB），**结论不能外推到其他 UVC 摄像头**——
   换摄像头必须重跑 `--list-formats-ext`。
3. 15/25/10/5 档为何失效未深挖（固件 bug？USB 带宽协商？），只确认 20/30 可用。
4. 已用锁曝光（`exposure_auto=1, absolute=157`）复测，**与自动曝光结果一致**，
   排除 AE 干扰；但未在极暗/极亮环境测。

##### 2.5.9.7 方法论（这次最值钱的一条）

> **「默认值」不是「上限」。**
> 2.5.8 节用 `bench_cam_fps` 测出 20fps 就下结论「物理上限就是 20fps」，
> 而 `--list-formats-ext` 白纸黑字写着支持 30fps。
> 少做的一步是：**在宣布上限之前，先确认所有可调参数都已经调到最大**。
>
> 更隐蔽的是**回读值会骗人**：`cap.get(CAP_PROP_FPS)` 返回 25 时，硬件实际跑的还是 20fps。
> 软件层的「我以为」和硬件的「实际」之间，差了一个实测打点。
>
> **教训**：测系统上限时先问自己——「我测的是真顶到天花板了，
> 还是撞在某个没调开的默认值上？」

### 2.6 MPP 硬解 MJPG → NV12：实测 1.03ms，7.6× 于 OpenCV 软解（2026-10-24）

> 2.5 节定位出「MJPG 软解是第一大瓶颈」后，用 RK3588 MPP 硬件解码器替换。
> **结论先行：硬解 p50 1.03ms（软解 7.76ms，7.6×），并已接进全链路真实闭环验证通过。**

#### 2.6.1 测试条件

| 项 | 说明 |
| :--- | :--- |
| 硬件/系统 | EVM3588-B（RK3588），Debian 11，内核 5.10.110 |
| 输入 | 1280×720 MJPG 静态文件（`/tmp/mppo/a.jpg`，59,976 B），N=200 次 |
| MPP 接口 | `MPP_VIDEO_CodingMJPEG`，advanced 模式，split_parse=1，NV12 输出 |
| 对照组 | `cv::imdecode`（OpenCV 4.5.1 纯 CPU libjpeg，同 N 同图） |
| 计时口径 | **只含 decode（put_packet + get_frame）**，不含 CSC/letterbox/NPU |
| 库版本 | ⚠️ 系统预装 librockchip_mpp.so（2021-05-20）**不可用**（见 2.6.4），
        用 MPP 源码 `/tmp/mppsrc/mpp-develop` 编译的静态库 |

#### 2.6.2 延迟对比

| 实现 | p50 | p95 | p99 | 备注 |
| :--- | ---: | ---: | ---: | :--- |
| **MPP 硬解** | **1.027 ms** | 1.181 ms | — | 200/200 有效帧，978.5 FPS 等效 |
| OpenCV imdecode（软解） | **7.761 ms** | — | — | 同图同 N |
| **加速比** | **7.6×** | | | |

（put_packet 本体 p50=1.015ms，get_frame p50≈0.017ms——耗时几乎全在解码器内部排队）

#### 2.6.3 正确性验证

- **9 张真实图**（校准集 + 实拍）：MPP 输出 NV12 转 RGB 后与 `cv2.imdecode` 参考
  逐像素对比，**Y 通道 PSNR 49.9~77.2 dB，9/9 PASS**（PC 端 `cmp_final.py`）。
- **全链路闭环**：`edgevision_pro_rknn`（MPP硬解→NV12→RGB888 CSC→letterbox→
  RKNNEngineDflFree→BEV 地面投影）：
  - 校准图 real_000.jpg：**person 0.813**（历史 9 图口径 0.8072，一致），
    BEV 距离 5.89 m；全链路 90.8ms（decode 1.70 + CSC 64.7 + letterbox 1.25 + NPU 23.2）
  - COCO 000000000009.jpg：6 检出（0.871/0.803/0.447…），与 eval_dump 分数一致；
    全链路 67.1ms
- 中间结果逐层回归验证：CSC 输出 vs cv2 参考灰度相关性 **1.0000**；
  letterbox buffer 相关性 **0.9995**（通道序 RGB 正确）。

#### 2.6.4 关键坑：系统库与内核驱动版本倒挂（必读）

- 系统预装 `/usr/lib/aarch64-linux-gnu/librockchip_mpp.so.0`（2021-05-20）上，
  `decode_put_packet` 恒返回 **-1012（MPP_ERR_BUFFER_FULL）**，任何参数组合无效；
- 用源码编译库（静态链）**同一份代码一行不改直接通过** → 根因是库与 `mpp_service`
  内核驱动的 ioctl 语义不匹配，不是用法问题；
- 详细排查见 `TROUBLESHOOTING.md` 第 17 条。

#### 2.6.5 全链路耗时分解与剩余瓶颈

| 阶段 | 实测 | 说明 |
| :--- | ---: | :--- |
| MPP 硬解 | **1.0~2.1 ms** | 首帧含开销，稳态 p50 1.03 |
| NV12→RGB888 CSC | **51~65 ms** | ⚠️ 新第一大瓶颈：CPU 读 **uncached DRM 内存**
  （实测 memcpy 仅 188 MB/s）；无 O2 编译再慢 1.55× |
| letterbox 640 | 1.0~3.4 ms | |
| NPU 推理 | 22.5~23.3 ms | core_mask=AUTO |
| 合计 | **67~91 ms** | |

**下一步优化**：① CMake 强制 `CMAKE_BUILD_TYPE=Release`（白捡 1.55×）；
② CSC 走 **RGA 硬件转换**（读 DRM 无惩罚；实测本板 RGA 2.1.0 零拷贝 NV12→RGBA+缩放
2.03ms，但**不支持 full CSC**，颜色偏差 ±13% 待量化）；③ MPP 硬编 JPEG（省 7.2ms）。

#### 2.6.6 局限（必须一起引用）

1. 静态文件输入，**未接真实摄像头流复测**（UVC 出的 MJPG 帧理论上同构）。
2. 9 图 PSNR 验证覆盖校准集+实拍，但**未覆盖暗光/运动模糊**场景。
3. RGA full CSC 缺陷是**本板 2.1.0 用户态库特性**，升级 librga 可能解除。
4. 计时为单次运行稳态统计，无多次冷启动对比。

---

### 2.7 端侧具身 Agent 端到端时延（2026-10-25）

> ⚠️ **口径先讲清**：这一节测的**不是 NPU 推理延迟**，而是**一条自然语言任务的全链路耗时**——
> 包含 N 轮 LLM 推理 + 每轮工具执行（读 sysfs / 连 9000 端口取帧 / 读事件日志）+ 中文生成。
> 与 2.2 节的「单帧推理 15.95ms」**不是同一个量**，不可互相换算或比较。

#### 2.7.1 实测数据（EVM3588-B，Qwen3.5-2B W8A8，RKLLM 1.3.1）

| 任务类型 | 端到端耗时 | 涉及工具数 | 说明 |
| :--- | ---: | ---: | :--- |
| 单工具问答（查温度/查灯态） | **18~40 s** | 1 | 一轮推理 + 一次工具调用 |
| 双工具闭环（查状态→控灯） | **23~37 s** | 2 | 回归 T03/T04/T11 实测 |
| 条件决策（先看画面再决定） | **36~59 s** | 2 | 回归 T08，含视觉取帧 |
| **多步综合巡检**（①硬件状态②告警事件③研判） | **106.5 / 112.2 / 123.9 s** | 4~5 | 三次独立测量 |
| 长上下文边界（800 字指令） | **85~113 s** | 1~2 | 回归 T10 |
| 单轮 NPU 推理（含生成） | **≈10 s 量级** | — | 由总时长/轮数反推，**未独立打点** |

#### 2.7.2 时延构成（诚实说明能拆到什么程度）

- **已独立测量**：工具端单次开销——读 sysfs < 1 ms、读 9000 端口一帧 300 ms 超时上限、
  写 LED + 回读 < 10 ms。相比 LLM 推理可以忽略。
- **未独立打点**：单轮推理的 prefill / decode 分解。当前只有「总时长 ÷ 轮数」的粗估
  （≈10 s/轮），**这是估算不是实测**，引用时必须说明。
- **波动原因**：三次多步演示（106.5 / 112.2 / 123.9 s）差异来自模型每次生成的 token 数不同
  + 工具链长度不同（4 个 vs 5 个不同工具 → 轮数不同），**不是性能抖动**。

#### 2.7.3 局限（必须一起引用）

1. **端侧 2B 模型不适合低延迟交互**——单次任务 20 s 起，只适合「发一句等一会」的巡检场景，
   不适合对话式频繁交互。这是模型规模决定的，不是实现问题。
2. **未测并发**：同一时刻只跑一个 Agent 进程；与 YOLO 视觉服务并发时的时延未测
   （理论上 LLM 会与 9000 端口的 NPU 推理抢核，但当前视觉服务是独立进程常驻）。
3. **未测冷启动**：模型加载（2.8 GB）耗时未计入，本次均为模型已加载后的稳态。
4. 三次多步测量样本量小（n=3），仅作量级参考，不宜当性能指标对外宣传。

#### 2.7.4 复现命令

```bash
# 多步演示（含时延测量与完整 trace）
python edgevision-rk3588/lab/agent_multistep_demo.py --out-dir _test_results

# 单条用例（如条件决策 T08）
python edgevision-rk3588/lab/agent_regression_test.py --only T08 --out-dir _test_results
```

结果在 `_test_results/<时间戳>/trace.json` 的 `act2_agent.wall_s` 与 `results.json` 的
`elapsed_s` 字段。

---

### 2.8 编译优化（CMAKE_BUILD_TYPE=Release）：此前全部 CPU 数字都是无优化跑出来的（2026-10-25）

> ⚠️ **口径先讲清**：
> 1. 这节测的是 **CPU 侧代码**（DFL 解码 + NMS）。**NPU 推理时间不受主机编译优化影响**——这是对照组。
> 2. 2.8.3 测的是 `bench_dflfree`（纯推理 + IO 周期 + DFL 解码 + NMS），**不含 CSC / letterbox / JPEG 编解码**；
>    **2.8.7 补测了生产二进制全链路**（含 MPP 硬解 + CSC + letterbox + NPU），两层结论都在下面。
> 3. 全部 `taskset -c 6` **固定大核**测量。不绑核会得出错误结论，见 2.8.6。
>
> **一句话结论**：`bench` 层 DFL 解码 **5.5×**、全链路(②+③) **−14.6%**；
> 生产二进制全链路 **−27.4%（76.93 → 55.84 ms，13.0 → 17.9 FPS）**，其中 CSC 单层 **1.549×**。

#### 2.8.1 问题：构建系统从来没有加过 -O

```
$ grep CMAKE_BUILD_TYPE /root/edgevision/deploy/cpp/build_native/CMakeCache.txt
CMAKE_BUILD_TYPE:STRING=                      ← 空
$ grep CXX_FLAGS .../build_native/CMakeFiles/edgevision_pro_rknn.dir/flags.make
CXX_FLAGS = -std=gnu++17                      ← 没有任何 -O 标志，等效 -O0
```

**后果**：项目此前记录的全部 CPU 侧耗时（MJPG 软解 7.76 ms、letterbox、DFL 解码 3.09 ms、
CSC 51~65 ms）都是在**无优化构建**下测出来的，是悲观值。

#### 2.8.2 改动（`edgevision-rk3588/deploy/cpp/CMakeLists.txt`，222 行）

| 行号 | 改动 |
| :--- | :--- |
| 17-21 | 未指定 `CMAKE_BUILD_TYPE` 时默认 `Release` |
| 24-29 | 兜底 `CMAKE_CXX_FLAGS_RELEASE = "-O3 -DNDEBUG"`（`CACHE STRING FORCE`，命令行仍可覆盖） |
| 33 | 新增 `option(BUILD_BENCH "..." OFF)`，把 bench 工具纳入同一套编译标志 |
| 45-70 | `check_cxx_compiler_flag` 自动探测 `-mcpu=cortex-a76.cortex-a55` → 失败降级 `-mtune=cortex-a76` → 再降级通用；可用 `-DEVISION_ARCH_FLAGS=none` 关闭 |
| 102-108 | **[集成点]** `if(EXISTS src/mpp_jpeg_encoder.cpp)` 则加入 `COMMON_SOURCES` 并 `-DHAVE_MPP_JPEG_ENCODER`；文件不存在时只打一行 message 不报错 |
| 180-192 | `BUILD_BENCH=ON` 时用同一套标志编 `bench_dflfree` |
| 197-222 | 构建摘要，并在 flags 里没有 `-O` 时打 `CMake Warning` |

**实测生效**（`build_release/CMakeFiles/edgevision_pro_rknn.dir/flags.make`）：

```
CMAKE_BUILD_TYPE        = Release
CMAKE_CXX_FLAGS_RELEASE = -O3 -DNDEBUG
CXX_FLAGS               = -O3 -DNDEBUG -mcpu=cortex-a76.cortex-a55 -std=gnu++17
CXX_DEFINES             = -DENABLE_RKNN -DHAVE_MPP_JPEG_ENCODER -DHAVE_OPENCV
```

#### 2.8.3 A/B 实测（PIN=6 固定大核，6 轮交替 O0/O3，EVM3588-B）

| 口径 | O0（无优化） | O3（-O3 -DNDEBUG -mcpu=a76.a55） | 变化 |
| :--- | ---: | ---: | :--- |
| ① 纯推理 `rknn_run` | 14.79 ~ 14.88 ms | 14.90 ~ 14.95 ms | **−0.3% ~ 0%（对照组，符合预期）** |
| ② IO 周期（set+run+get） | 15.47 ~ 15.56 ms | 15.58 ~ 15.64 ms | 无变化 |
| ③ **DFL 解码 + NMS** | **3.44 ~ 3.51 ms** | **0.62 ~ 0.63 ms** | **5.5× 加速** |
| ④ **全链路（②+③）** | **18.91 ~ 19.06 ms** | **16.21 ~ 16.26 ms** | **省 2.78 ms（−14.6%），52.6 → 61.7 FPS（+17.3%）** |

- 每轮 200 次迭代，6 轮全部落在上表区间内，**波动 < 1%**，重复性很好。
- 6 轮检出框数均为 0（bench 输入是灰图，符合预期）。

#### 2.8.4 二进制体积

| 构建 | `edgevision_pro_rknn` | `edgevision_rknn` |
| :--- | ---: | ---: |
| 无优化（`build_native`，2026-10-24） | 206,256 B | 172,592 B |
| Release `-O3`（不含 MPP encoder） | 75,688 B | 65,792 B |
| Release `-O3`（含 MPP encoder 集成点） | 109,376 B | 99,672 B |

→ 纯优化带来的代码体积缩减 **−63%**（206,256 → 75,688 B）。

#### 2.8.5 为什么"对照组"是这次实验最重要的设计

第 ① 项（纯推理）在 O0 / O3 下差 **< 1%**，证明了两件事：

1. **NPU 时间与主机编译优化无关** —— 符合物理预期，说明两边测的确实是同一个模型同一条路径。
2. **测量本身没有漂移** —— 如果这一层也出现了 >5% 的变化，说明有负载/频率/热漂移干扰，那么第 ③④ 项的差异就不可信。

**方法论**：做 A/B 一定要先设一个**"理论上不该变"的对照组**，它稳了，其他项的变化才能归因。

#### 2.8.6 坑：不绑核时，O0 的 mean 会骗你（这里是 2 倍差异）

| 组 | DFL mean | DFL p50 | DFL p95 |
| :--- | ---: | ---: | ---: |
| **pinned** O0 | 3.44 ~ 3.51 ms | 3.44 ~ 3.48 | 3.48 ~ 3.71 |
| **pinned** O3 | 0.62 ~ 0.63 ms | 0.62 ~ 0.63 | 0.63 ~ 0.64 |
| **unpinned** O0 | **6.38 ~ 8.15 ms** | 3.44 ~ 3.47 | **15.78 ~ 17.20** |
| **unpinned** O3 | 2.71 ~ 2.93 ms | 2.71 ~ 2.89 | 2.73 ~ 3.16 |

unpinned 组 O0 的 **p50 只有 3.45 ms（与 pinned 完全一致），但 mean 被拉到 6.4~8.2 ms，p95 冲到 17 ms**。

**根因**：O0 代码体量大、执行时间长，被 CPU 调度器跨核迁移（以及大小核迁移）打断的次数显著更多；
O3 版执行短平快，受影响小。所以**mean 反映的是调度噪声，不是代码差异**。

**结论：性能测量必须 `taskset` 绑核**，否则长尾会污染均值，甚至把 5.5× 的差异压成 2×。

#### 2.8.7 生产二进制全链路 A/B：把"白捡 1.55×"验成了真数（2026-10-25 补充实测）

2.8.3 测的是 `bench_dflfree`（不含 CSC / letterbox / 图像 IO）。为了回答"**真实生产程序到底快多少**"，
又用**同一张真实 JPEG、同一个模型**，直接跑两个**生产二进制**各 5 次（`taskset -c 6` 绑核）：

```bash
# /root/edgevision/data/calib_v2/real_000.jpg (48220 B, 640x640)
# EV_MODEL=/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn
taskset -c 6 <build>/edgevision_pro_rknn $IMG
```

| 层 | O0 `build_native`（5 次） | O3 `build_release`（5 次） | 变化 | 判读 |
| :--- | ---: | ---: | :--- | :--- |
| MPP 硬解 decode | 1.108 / 1.279 / 1.279 / 1.302 / 1.280 ms<br>均值 **1.250** | 1.291 / 1.293 / 1.313 / 1.276 / 1.438 ms<br>均值 **1.322** | **+5.8%（无变化）** | **对照组①**：硬件解码，与主机编译优化无关 ✔ |
| **NV12→RGB888 CSC** | 53.143 / 51.747 / 51.569 / 51.564 / 52.058 ms<br>均值 **52.016** | 33.584 / 33.347 / 33.547 / 33.936 / 33.502 ms<br>均值 **33.583** | **1.549× 加速** ⭐ | 这是我们**自己写的定点转换循环**（`mpp_jpeg_decoder.cpp:149-173`），优化直接命中 |
| letterbox（`cv::resize`） | 1.096 / 1.124 / 1.303 / 1.184 / 1.212 ms<br>均值 **1.184** | 1.351 / 1.145 / 1.328 / 1.128 / 1.156 ms<br>均值 **1.222** | **+3.2%（无变化）** | **对照组②**：走 OpenCV 预编译库，我们编译优化动不了它 ✔ |
| NPU + DFL 解码（`run_inference`） | 22.77 / 22.29 / 22.27 / 22.42 / 22.65 ms<br>均值 **22.480** | 19.67 / 20.80 / 19.23 / 19.50 / 19.34 ms<br>均值 **19.708** | **−2.77 ms（−12.3%）** | 与 2.8.3 第 ③ 项的 −2.78 ms **完全吻合**，互相印证 |
| **全链路合计** | 78.11 / 76.44 / 76.42 / 76.47 / 77.20 ms<br>均值 **76.928** | 55.90 / 56.59 / 55.41 / 55.84 / 55.44 ms<br>均值 **55.836** | **省 21.09 ms（−27.4%），13.0 → 17.9 FPS（1.378×）** | |

**为什么这条实测比 bench 更有说服力 —— 两个"不该变"的对照组同时成立**：

| 对照组 | 为什么理论上不该变 | 实测 | 结论 |
| :--- | :--- | ---: | :--- |
| MPP 硬解 | 跑在 `rkvdec` 硬件上，CPU 只负责下发/取帧 | +5.8% | 无变化 ✔ |
| letterbox | `cv::resize` 在 OpenCV 的 `.so` 里，早就编译好 | +3.2% | 无变化 ✔ |

两个走"外部/硬件"的层纹丝不动，而**唯一的自有 CPU 热点（CSC）恰好提速 1.55×** —— 这说明加速
**不是**测量漂移、不是负载波动，而是 `-O3 + -mcpu=cortex-a76.cortex-a55` 真的作用在了我们的代码上。
（±5% 内视为无变化，样本 n=5，同图同模型，检出结果 5/5 完全一致：`person 0.813 → dist 5.89m`。）

> ⭐ **结论**：历史上文档里写的"CMake 加 Release 白捡 1.55×"**是对的**——
> 但成立的**不是全链路，而是 CSC 这一层**（1.549×）；全链路实际收益是 **−27.4%（1.378×）**。
> 引用时请说清层次，别说"全链路提速 1.55×"。

#### 2.8.8 局限（引用时必须一起说）

1. 2.8.7 的 A/B 只在 **640x640 的 `real_000.jpg`** 上做（该图 letterbox scale=1.000，无填充），
   1280x720 输入下 CSC 耗时约翻 2.25 倍，**绝对秒数不能直接外推到 720p**，但**倍率方向一致**。
2. 原始 2.8.3 的 bench 未测 preview 主程序；**2.8.7 已补测生产二进制**，两者结论一致。
   历史上对 preview 的"1.55×"估计现已被实测确认为 **CSC 单层 1.549×**。
3. 测量时 `w4_edgevision_gateway` 常驻（占 CPU ~55%），两组同一负载，**相对差成立**，
   但绝对耗时比空载偏高（空载下两组都会更快）。
4. `bench_dflfree` 的 DFL 解码与生产 `postprocess_dfl_free.cpp` 是同一份源码，
   但 bench 输入灰图（检出 0 框），**NMS 部分负载偏低**，所以第 ③ 项对真实场景是乐观值。
5. n=6（bench）/ n=5（生产 A/B）交替测量，未做统计显著性检验（但组内波动 <1%、差异量级远大于噪声）。
6. **CSC 仍然是全链路第一瓶颈**（O3 下 33.58 ms 占 60%）：编译优化只削掉了它 1/3 的时间，
   要真正打下去得换 **RGA 硬件转换**（见 2.6 节末尾和 TROUBLESHOOTING 第 24 条）。

#### 2.8.9 复现命令

```bash
# ① 全新目录验证默认 Release（不传任何 -DCMAKE_BUILD_TYPE）
rm -rf /root/lead_build_check && mkdir -p /root/lead_build_check
cd /root/lead_build_check
cmake /root/edgevision/deploy/cpp -DCROSS_COMPILE_RK3588=ON
make -j8
grep CXX_FLAGS CMakeFiles/edgevision_pro_rknn.dir/flags.make
grep CXX_DEFINES CMakeFiles/edgevision_pro_rknn.dir/flags.make

# ② 生产二进制全链路 O0 vs O3（2.8.7 那张表）
export EV_MODEL=/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn
export LD_LIBRARY_PATH=/root/edgevision/deploy/cpp/3rdparty/rknn_api/lib:/root/edgevision/third_party/mpp/lib
IMG=/root/edgevision/data/calib_v2/real_000.jpg
for i in 1 2 3 4 5; do
  taskset -c 6 /root/edgevision/deploy/cpp/build_native/edgevision_pro_rknn  $IMG | grep -E 'CSC|Letterbox|DONE'
  taskset -c 6 /root/edgevision/deploy/cpp/build_release/edgevision_pro_rknn $IMG | grep -E 'CSC|Letterbox|DONE'
done
```

---

### 2.9 MPP 硬件 JPEG 编码评估：结论是"本板不值得用"（负结论，2026-10-25）

> ⚠️ **这是一条负结论**。它的价值不在于"优化成功"，而在于**用数据把一个写在路线图上的
> 假设否掉了**，并且给出了可复现的量化依据。

#### 2.9.1 为什么要做

2.5 节实测：`cv::imencode(".jpg", ..., quality=80)` 耗时 **7.1~7.2 ms，占单帧 14%**
（`edgevision-rk3588/lab/w4_edgevision_preview_real.cpp:346-347`）。既然 MPP **硬解**能做到
p50 1.03 ms（7.6× 于软解，见 2.6 节），**硬编**理论上也该快——路线图里因此写了"MPP 硬编省 7.2ms"。

#### 2.9.2 结论先行

**MPP JPEG 硬编在本板比 OpenCV 软编更慢（1.07~1.16×）**，唯一真实优势是 CPU 占用降到约 1/12。
**建议：从路线图划掉"MPP 硬编省 7.2ms"，CPU 侧优化转去做 RGA CSC。**

#### 2.9.3 速度对照（1280×720，N=200，2 轮）

> 下表用 **Lead 独立复跑**的保守数字（首次报告为 7.72 vs 6.65 = 1.16×，复跑 7.24 vs 6.65 = 1.09×，
> 趋势一致，差异来自板上负载波动；对外引用取保守值）。

| 指标 | MPP 硬编 qf80 | OpenCV 软编 q80 | 硬/软 |
| :--- | ---: | ---: | ---: |
| mean | **7.24 / 7.20 ms** | **6.65 / 6.70 ms** | **1.07 ~ 1.09×（更慢）** |
| p95 | 9.62 / 10.82 ms | 6.72 / 6.71 ms | 1.43 ~ 1.61× |
| p99 | 11.09 / 11.46 ms | 6.74 / 6.72 ms | 1.65 ~ 1.71× |
| **CPU 占用** | **8.2 ~ 8.3%** | **99.7%** | **0.08×** |
| **每帧 CPU 时间** | **0.59 ms** | **6.64 ms** | **0.09×** |

其他口径：

| 场景 | 硬编 | 软编 | 结论 |
| :--- | ---: | ---: | :--- |
| 640×640 三轮 | 3.40 ~ 3.59 ms | 3.00 ms | 1.13 ~ 1.20×，硬编更慢 |
| 分辨率扫描 64×64 ~ 960×540 | — | — | **全部档位硬编更慢，没有赢的** |
| 4 路并行（NT=4） | 1.573 ms/帧（635.8 FPS） | **0.838 ms/帧（1192.9 FPS）** | **软编快 1.88×** |
| 端到端（输入为 BGR） | — | — | 硬编还要多付 BGR→NV12 转换 1.75~4.37 ms，整体慢 **1.42~1.81×** |

#### 2.9.4 为什么慢：延迟模型

对分辨率扫描做线性拟合：

```
硬编 t(ms) ≈ 0.35 + 7.0e-6 × 像素数
软编 t(ms) ≈ 0    + 7.5e-6 × 像素数
```

**每像素算力两者打平（硬编甚至略优），但硬编每帧固定多付 0.35 ms 的
ioctl 提交 + 中断往返开销。** 分辨率越小，这 0.35 ms 占比越高，所以 64×64 那档硬编输得最多；
1280×720 时固定开销被摊薄，差距缩到 1.07×。

4 路并行那档输得更惨（1.88×），因为**软编能吃满 8 个 A76/A55 核，硬编只有 4 个 jpege 硬件核**。

#### 2.9.5 正确性：合格的（如果将来场景需要，这套代码是可用的）

| 口径 | 硬编 qf80 | 软编 q80 | 差 |
| :--- | ---: | ---: | ---: |
| Y-PSNR（640×640，9 张真实图均值） | **42.46 dB** | 44.34 dB | −1.88 dB |
| BGR-PSNR（同上） | **44.19 dB** | — | — |
| Y-PSNR（1280×720） | **49.27 dB** | — | — |
| BGR-PSNR（1280×720） | **50.33 dB** | — | — |

- 9/9 张全部 ≥35 dB；逐图 BGR-PSNR 46.86 / 46.55 / 45.27 / 46.25 / **35.66** / **37.33** / 46.01 / 45.96 / 47.79 dB
  （后两张低是因为图本身高频，软编同期也只有 35.70 / 37.37 dB）。
- `file` 认证：`JPEG image data, JFIF standard 1.02, ..., baseline, precision 8, 640x640, components 3`；
  首字节 `FFD8 FFE0`、尾字节 `FFD9`。

**"真的走了硬件"的实证**（不是软回退）：

```
跑 bench 前  fdba0000.jpege-core 42/44/46/48 中断数 = 21485 / 2735 / 1566 / 820
跑 bench 后                                        = 25038 / 3438 / 1953 / 987
增量                                                = +3553 / +703 / +387 / +167
其余 jpege / rkvenc / rkvdec 中断数全部 0 变化
```

#### 2.9.6 CPU 优势在什么场景才有意义

单路 30 fps：软编每帧 6.64 ms，占一个核的 20%（6.64/33.3），**CPU 根本不是瓶颈**，
硬编省下来的 6 ms CPU 换不回 0.6 ms 的延迟损失。

多路场景才可能翻转：4 路并发时软编要吃满 4 个核，硬编只占 33% 的一核 + 4 个 jpege 硬件核。
**但前提是 CPU 已经被别的工作（CSC、letterbox、后处理）榨干到多核不够用**——
本项目单路/双路场景不成立。

#### 2.9.7 局限

1. 测量时 `w4_edgevision_gateway` 常驻占 CPU ~55%（任务约束不停服务）。硬编受 CPU 竞争影响**更大**
   → **真实空载下软编的优势只会更大**，这里的 1.07× 是偏保守的。
2. 输入是 NV12（与硬解输出对齐，方便零拷贝）；若从 BGR 开始算，硬编还要加转换税（见上表）。
3. 只测了 4 个 jpege 核；未测与 NPU / VPU 同时满载时的互相抢资源。
4. 未测长跑稳定性（>10 万帧）。

#### 2.9.8 复现命令

```bash
# 板上
cd /root/edgevision/deploy/cpp/bench
g++ -O2 -std=c++17 bench_jpeg_encoder.cpp ../src/mpp_jpeg_encoder.cpp \
    -I../include -I/root/edgevision/third_party/mpp/include \
    -L/root/edgevision/third_party/mpp/lib -lrockchip_mpp \
    -Wl,-rpath,/root/edgevision/third_party/mpp/lib \
    -I/usr/include/opencv4 -lopencv_core -lopencv_imgcodecs -lopencv_imgproc \
    -o bench_jpeg_encoder

# 参数: <图目录> <迭代次数>
./bench_jpeg_encoder /root/edgevision/data/calib_v2 200

# 验证"真的走硬件": 跑前后各看一次中断计数
cat /proc/interrupts | grep jpege
```

**代码位置**：`deploy/cpp/{include/mpp_jpeg_encoder.h, src/mpp_jpeg_encoder.cpp, bench/bench_jpeg_encoder.cpp}`，
完整证据 `deploy/cpp/bench/mpp_jpeg_encoder_evidence.md`。
