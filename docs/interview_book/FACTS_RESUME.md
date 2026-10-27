# 简历对应章专用事实表（已逐条核对到源文件行号，禁止自行计算新数字）

> 本文件只给「第 16/17 章」撰写者用。所有数字必须**原样引用**，不得四舍五入、不得自己相减。
> 需要未在此表列出的数字时：**不写数字**，改写「实测有明显提升」。

## A. 视频与推理链路（真实实测）

| 事实 | 数值 | 出处 |
|---|---|---|
| 单核纯推理基线 | 37.19 ms / 26.9 FPS | docs/BENCHMARKS.md |
| DFL 图手术后纯推理 | 15.95 ms（耗时降 57%，等价提速 2.33×） | docs/BENCHMARKS.md |
| 单帧全链路（含采集解码推流） | 20.58 ms / 48.6 FPS（时延降 44.7%，吞吐 +81%） | docs/CURRENT_STATUS.md |
| 逐层剖析 | 125 个算子：NPU 115 个 19.23 ms（51.0%）+ CPU 回退 10 个 18.44 ms（49.0%） | docs/BENCHMARKS.md:128 |
| 三核并行 | 3 个独立 Context 绑 Core 0/1/2 → 59.3 FPS / 2.21× | docs/BENCHMARKS.md |
| 三核负载 | 各核约 49% 负载，**不是线性 3×**，别吹线性 | docs/AGENTS.md |
| MPP 硬解 | p50 1.03 ms vs OpenCV 软解 7.76 ms → 7.6× | docs/BENCHMARKS.md |
| MPP 坑 | 出厂预装动态库与 5.10 内核驱动接口协议不兼容 → `put_packet` 恒返回 **-1012**，源码编译移植静态库解决 | docs/TROUBLESHOOTING.md |
| 定长帧协议 | **248 字节**定长二进制帧，`#pragma pack(1)` | docs/BENCHMARKS.md |
| 上位机活流 | 稳定 **19.2 FPS**（复现修 ①结果队列 size>=2 丢最旧 ②建联清空存量） | docs/interview_book/src/07（已有） |

## B. 构建与二进制

| 事实 | 数值 | 出处 |
|---|---|---|
| 二进制体积 | 206,256 B → 75,688 B（**−63%**） | docs/BENCHMARKS.md:810-814 |
| 全链路（生产二进制，taskset 绑 A76） | 76.928 ms → 55.836 ms（**−27.4%**，1.378×，13.0→17.9 FPS） | docs/BENCHMARKS.md:855-872 |
| CSC 单层 | 52.016 ms → 33.584 ms（**1.549×**） | docs/BENCHMARKS.md:855 |
| 前提 | 此前 `CMAKE_BUILD_TYPE` 为空，一个 `-O` 都没有 | docs/AGENTS.md |
| 注意 | 「全链路 1.549×」是**错的**，1.549× 只属于 CSC 单层 | docs/BENCHMARKS.md:872,880 |

## C. 量化与精度

| 事实 | 数值 | 出处 |
|---|---|---|
| 校准集 | `data/calib_v2/` **170 张** = 50 张板端双摄实拍 + 100 张光照/对比度/伽马/噪声增强 + 20 张合成 | docs/BENCHMARKS.md:221 |
| 评测集 | COCO128：128 张图 / **929 个 GT 框** / 71 个类别 | docs/BENCHMARKS.md:293 |
| 真 mAP@0.5 | FP32 **0.6087** → INT8 **0.6002**，掉 **0.85 个百分点**（相对 −1.4%） | docs/CURRENT_STATUS.md:71 |
| 反面教材 | 合成色块校准集 → 分类头 Scale 严重偏移 → 输出塌缩为常数 **0.5000**（**−38.5%**） | docs/BENCHMARKS.md:232 |
| 假安全指标 | 特征图余弦相似度 **0.9965** 却检测不可用 → 余弦相似度会掩盖分类头失效 | docs/BENCHMARKS.md:20-22 |
| 模型体积（**重点，简历写错了**） | FP32 ONNX 12.2 MB →(量化) 6.22 MB →(图手术) 4.34 MB。BENCHMARKS.md:309 原话：「**不能合成一个减法对外说 −64%**」 | docs/BENCHMARKS.md:309 |

## D. 端侧大模型 / Agent

| 事实 | 数值 | 出处 |
|---|---|---|
| RKLLM | `rkllm-runtime v1.3.1`，Qwen3.5-2B，**W8A8**，模型 **2.8 GB** | docs/CURRENT_STATUS.md:115 |
| 内存 | 模型加载后系统空闲内存稳定 **6.1 GB** | docs/CURRENT_STATUS.md:115 |
| 工具数 | **5 类**硬件原子工具：get_system_health / get_led_status / control_led / get_vision_telemetry / get_patrol_events | docs/interview_book/src/11 |
| 多步耗时 | 106.5 / 112.2 / 123.9 s（三次独立测量），4~5 个不同工具 | docs/BENCHMARKS.md:713,723 |
| 协议 | 解析 Qwen 原生 XML `<tool_call>`，严格 JSON 数组回填 | docs/interview_book/src/11 |
| 快慢双系统 | C++ 快层 **5s** 周期状态机 + 迟滞判决，**不加载 LLM**（避免抢 NPU）；翻转才写 `llm_pending.jsonl` | docs/interview_book/src/12 |
| 闪灯验收 | **占空比 20.0% → 49.7%** | docs/interview_book/src/12 |
| 回归 | **12 项**自动化用例全绿（含故障注入 + 自动恢复） | docs/CURRENT_STATUS.md |
| 混叠坑 | 演示中 Agent 汇报与物理现象矛盾 → 周期性物理量单点采样撞在灭相上（混叠），教训：**先查感知层数据流，再调 Prompt** | docs/interview_book/src/12 |

## E. 外设与系统（简历 A 重点，书里原来缺）

| 事实 | 数值 | 出处 |
|---|---|---|
| 双摄 | V4L2 双路 **1280×720**；根因=源码未显式设帧率 → 驱动默认协商 **20fps**；修复后 **33.33 ms/帧（30.0 FPS）零抖动** | docs/BENCHMARKS.md:441,464-468,520-531 |
| 双麦 | ALSA `hw:5,0` 与 `hw:6,0`，**16 kHz S16_LE** 立体声，实测秒录 | docs/CURRENT_STATUS.md:28 |
| 控制面 | C++ RPC 服务 **端口 8088**（`lab/edgevision_agent_server.cpp`），数据面 TCP **9000** | docs/CURRENT_STATUS.md:113 |
| 8088 能力 | 7 温区监控、NPU 3 核负载、LED 物理灯控制、9000 目标检测采样 | docs/CURRENT_STATUS.md:113 |
| 服务化 | `daemon(0,0)` 会话迁移；systemd `Restart=always` **5 秒**自愈；`sigaction` 捕获 SIGINT/SIGTERM；全链路屏蔽 SIGPIPE | docs/interview_book/src/03 |

## F. 简历原文里「措辞有问题」的 6 处（第 16/17 章必须给出改法）

| 简历现有写法 | 问题 | 应该怎么说 |
|---|---|---|
| 「模型体积缩减 64%（12.2 MB → 4.34 MB）」 | 跨了量化和图手术两步，BENCHMARKS 明令不能说 −64% | 「INT8 口径 6.22 MB → 图手术后 4.34 MB（−30.2%）；FP32 ONNX 原图 12.2 MB」 |
| 「INT8 量化 mAP 仅微损 0.85%」 | 0.85 是**个百分点**，相对是 −1.4%；写「0.85%」会被追问 | 「mAP@0.5 0.6087 → 0.6002，掉 0.85 个百分点（相对 −1.4%）」 |
| 「提速 57%」 | 57% 是**耗时降幅**，提速倍数应是 2.33× | 「纯推理耗时 37.19 → 15.95 ms（−57%），等价吞吐 2.33×」 |
| 「CSC 52.02 → 33.58 ms（1.55 倍加速）… 生产二进制全链路耗时缩短 27.4%」 | 两句放一起容易被听成「全链路 1.55×」 | 明确「1.549× 只属于 CSC 单层；全链路是 −27.4%」 |
| 「较软解提速 7.6 倍」 | 7.76/1.03 = 7.53，说 7.6× 偏低但方向对；不可再往上加 | 「解码 p50 1.03 ms vs 软解 7.76 ms，约 7.5×」（7.6× 可以说，但不能说 8 倍） |
| 「系统可用内存冗余超 6.1 GB」 | 6.1 GB 是**加载模型后空闲内存**，不是「冗余」总容量 | 「模型加载后系统空闲内存维持 6.1 GB」 |

## G. 禁止事项（违反=返工）

1. RGA 硬件 CSC / DMA-BUF 零拷贝 / RTSP-WebRTC / H.264-H.265 硬解 / ByteTrack 进 C++ / 整机功耗 / ASR —— **全是未实现**，只能写「没做，但我评估过」。
2. 三核 = 2.21×，**不是 3×**。
3. 不得出现 `####`、`---`、图片、链接、嵌套列表、HTML、emoji。
