# 项目架构总览

> 一份文档讲清：这个项目由哪几条技术线组成、怎么协作、共享哪些资源、各自到什么程度。
> **面试开场用这份**，细节再跳到对应的专项文档。
>
> 相关：[CURRENT_STATUS.md](CURRENT_STATUS.md)（现在能干什么）｜[BENCHMARKS.md](BENCHMARKS.md)（所有数字的口径）｜[TROUBLESHOOTING.md](TROUBLESHOOTING.md)（34 条踩坑）

---

## 一句话

在 RK3588（3 核 NPU 6 TOPS / 4×A76 + 4×A55 / 8 GB）上，把「**视觉检测 → LLM 决策 → 硬件动作 → 语音播报**」串成一个**纯端侧、无云端**的具身智能体。

不是三个独立 Demo 拼在一起——它们共享同一块 NPU、同一块麦克风、同一套硬件访问路径，**资源争用是本项目最核心的工程约束**。

---

## 全景图

```
        ┌───────────────── 输入 ──────────────────┐
        │  双目 UVC 相机 (/dev/video81, /dev/video83) │
        │  板载麦克风 (hw:0,0, 16 kHz)               │
        └───────────┬──────────────────┬───────────┘
                    │                  │
        ┌───────────▼──────────┐   ┌───▼────────────────┐
        │ ①  视觉检测线         │   │ ③  语音交互线       │
        │  YOLOv8 → NPU 推理    │   │  VAD → Paraformer   │
        │  DFL 解码 / 3D 框 / BEV│   │  → 软唤醒 → vits TTS │
        │  MJPEG :8080          │   │  → 48 kHz 耳机播放   │
        │  TCP 定长帧 :9000     │   │                     │
        └───────────┬──────────┘   └───┬─────────────────┘
                    │ 检测结果           │ 识别文本
                    │                  │
                    └─────────┬────────┘
                    ┌─────────▼──────────┐
                    │ ②  Agent 决策线     │
                    │  Qwen3.5-2B W8A8   │
                    │  3 核 NPU / rkllm   │
                    │  6 个硬件工具        │
                    └─────────┬──────────┘
                              │ sysfs / DRM / POSIX socket
                    ┌─────────▼──────────┐
                    │  硬件执行           │
                    │  LED / 温度 / 巡检   │
                    └────────────────────┘
```

---

## 三条技术线

### ① 视觉检测线

| 项 | 内容 |
|---|---|
| 算子 | YOLOv8，RKNN 量化部署，跑 3 核 NPU |
| 关键实现 | 手写 DFL 解码（格子单位 → 像素必须乘 stride）、双目视差 → 深度、BEV 俯视投影 |
| 三个出口 | `:8080` MJPEG 实时预览（检测框 + 3D 棱边 + BEV 小地图）、`:9000` TCP 定长帧推流（248 B/帧）、`:8088` RPC 供 Agent 查询 |
| 实测 | 解码 3.19 ms → **1.79 ms**（访存顺序优化，跨 2.7 MB 数组 cache 全废） |
| 专题 | `tech_notes/BEV_GATEWAY_ARCHITECTURE.md`、`tech_notes/CAMERA_FPS_ROOT_CAUSE.md` |

**取舍**：`:8080` 与 `:9000` 抢同一个摄像头，不能同时开——这是设计选择，不是 bug（预览要低延迟，推流要稳定帧率）。

### ② Agent 决策线

| 项 | 内容 |
|---|---|
| 模型 | Qwen3.5-2B W8A8，`rkllm-runtime 1.3.1`，占 3 核 NPU 中全部 3 核 + 2.0 GB IOMMU |
| 循环 | ReAct 式：思考 → 选工具 → 执行 → 回填观测 → 再思考，最多 5 轮 |
| 工具 | 6 个：温度/NPU 负载、LED 状态、LED 控制、视觉遥测、巡检事件、巡检运行态（详见 [`tech_notes/AGENT_TOOLS.md`](tech_notes/AGENT_TOOLS.md)） |
| 关键约束 | 工具返回**只给事实不给结论**；阈值等运行态必须外化成可查询快照 |
| 实测 | 首轮预热后 **1 s**、后续每轮 **9~10 s**；工具轮数从 5 降到 1（prompt 分层约束） |
| 专题 | `tech_notes/AGENT_MULTISTEP_DEMO.md`、`tech_notes/PATROL_DAEMON_DESIGN.md` |

**取舍**：慢层（LLM）负责研判与决策，快层（巡检守护，纯 C 循环）负责毫秒级硬件响应——**不用 LLM 做实时控制**。

### ③ 语音交互线

| 项 | 内容 |
|---|---|
| 输入 | 板载麦克风 `hw:0,0`，16 kHz，VAD 用 silero（阈值 0.5 / 静音 0.5 s） |
| 识别 | Paraformer-zh-small int8（78 MB），**RTF 0.023**（比实时快 40×），支持方言与中英混说 |
| 唤醒 | **软唤醒**：ASR 文本匹配「小瑞」+ 5 个同音字，命中后 10 s 激活态 |
| 合成 | vits-zh-fanchen-C（116 MB），**常驻 daemon**，RTF 0.564（4 线程最优） |
| 输出 | 16 kHz 单声道 → 3 倍线性插值 48 kHz → 双声道 → `aplay hw:2,0` |
| 端到端 | 说完到出声约 **11 s**（VAD 0.5 + ASR 0.1 + Agent 10 + TTS 1） |
| 专题 | `tech_notes/VOICE_ASR_PLAN.md` 第 6 节 |

**取舍**：放弃 KWS 唤醒词模型（它与 ASR 抢麦克风，切换一次要重载 ASR 模型 4 s），代价是 ASR 常驻（CPU 约 3%）且未唤醒时仍在听。

---

## 共享资源与争用（本项目最真实的部分）

| 资源 | 谁抢 | 后果 | 解法 |
|---|---|---|---|
| **NPU IOMMU（4 GiB）** | Agent（2.0 GB）+ 视觉检测（~200 MB/实例） | 超过 4 GiB 就 `rkllm_init failed` / `errno 14` | 起 Agent 前必须清场；`kill -9` 有时不释放，只能重启 |
| **摄像头 `/dev/video81`** | `:8080` 预览 vs `:9000` 推流 | 同开会花屏/掉帧 | 设计上互斥，不同时开 |
| **麦克风 `hw:0,0`** | ASR vs KWS vs 录音 | 后来的实例 `exit 255` 无限重启 | 一设备一占用，切换前必须停 |
| **A76 大核（4 个）** | TTS 4 线程 vs 视觉 prep/encode | TTS 打满时视觉侧变慢 | 未做对照测量，已知局限 |
| **CPU 与 NPU 分工** | ASR/TTS 全在 CPU，LLM/视觉在 NPU | 互不干扰，但 CPU 侧会互相拖 | 见 `BENCHMARKS.md` 2.11 口径说明 |

---

## 端口与进程清单

| 端口 | 子系统 | 进程 | 是否常驻 |
|---|---|---|---|
| 8080 | 实时检测预览（MJPEG） | `edgevision_preview_real` | 手动 |
| 8081 | 双目 stereo3d | `stereo3d_demo` | 手动 |
| 8088 | Agent RPC 控制面 | `edgevision_agent_server` | 手动 |
| 8090 | 车辆实时检测看板 | **`edgevision-dashboard.service`**（systemd） | ✅ 自启，**可由 Agent 经 `control_service` 启停** |
| 9000 | TCP 推流网关（248 B 定长帧） | `edgevision.service`（systemd） | ✅ 自启 |
| — | 巡检守护（快层） | `edgevision-patrol.service` | ✅ 自启 |
| — | 语音编排网关 | `voice_gateway` + `tts_daemon` | 手动（systemd 单元已 enable 未切换） |
| — | Agent（慢层） | `rkllm_embodied_agent` | 随 voice_gateway 或手动 |

---

## 关键数字（面试直接引用，口径见 BENCHMARKS）

| 指标 | 数值 | 说明 |
|---|---|---|
| ASR | RTF **0.023** | 比实时快约 40×，7 条含方言/中英混说 |
| TTS | RTF **0.564** | 4 线程最优（6 线程反而退到 0.679） |
| Agent 首轮 | **1 s** | 预热后（原 28 s） |
| Agent 后续轮 | **9~10 s** | NPU 独占；与网关同跑劣化到 39 s |
| 播报首段出声 | **1 s** | TTS 常驻后（原 13 s） |
| 视觉解码 | **1.79 ms** | 优化前 3.19 ms |
| 端到端语音 | **~11 s** | 说完到出声 |

---

## 演进路径（讲项目时的叙事线）

```
① 视觉检测能跑            → 发现 DFL 量纲错、解码慢、帧率虚高
② 让 LLM 能操作硬件        → RKLLM Tool Calling 必须严谨 JSON 数组、阈值必须外化
③ 让 Agent 听人话          → ASR/TTS 上板、编排网关、设备争用
④ 让它反应快              → prompt 分层（68s→10s）、TTS 常驻（13s→1s）、预热（28s→1s）
⑤ 让它别瞎听              → 软唤醒门控
```

每一步都有对应的 TROUBLESHOOTING 条目和实测数据，**这是面试时最能体现"迭代能力"的部分**。
