# VOICE_ASR_PLAN — RK3588 纯离线中文 ASR 选型与集成设计

> 作者: voice-researcher (task-4) · 日期: 2026-10-24
> 定位: **纯调研文档**——不上板、不写代码，为「按键录音 → VAD → 本地 ASR → 文本送 rkllm_embodied_agent」的语音入口做选型与集成设计。
> 板况: EVM3588-B (RK3588, 8GB, Debian 11 bullseye, glibc 2.31, aarch64, 板上 g++ 10.2 / make / cmake 已装，无 pip)。NPU 当前被 YOLO 网关 + RKLLM(Qwen3.5-2B) 共用，**ASR 优先走 CPU(NEON)**。
> 原则: 本文所有外部数字都带来源链接；查不到的写「待板端实测」，不编数。

---

## 1. 候选横评

两个引擎 × 四个候选模型。先给结论总表，再逐项说明。

| 候选 | aarch64+Debian11 可用性 | 模型体积(磁盘) | 运行时 RAM 估计 | 中文质量口碑 | 流式/非流式 | License | 集成难度 |
|---|---|---|---|---|---|---|---|
| sherpa-onnx + **SenseVoice Small int8** | ✅ 官方 prebuilt linux-aarch64 (v1.12.12 / v2.0.0)，manylinux 构建兼容 glibc 2.17+；⚠️ 动态库包需 GLIBCXX_3.4.29，见 §6 | 下载 tar ≈234MB，model.int8.onnx ≈239MB | ~512MB (发布方口径)；实际待板端实测 | **最强**: 官方 CER 7.81% (多域中文测试集) | 非流式 | 引擎 Apache-2.0；模型权重 FunASR 模型许可协议 | ⭐ 低——官方命令行二进制可直接验证，再接 C++ API |
| sherpa-onnx + **Paraformer-zh Small int8** | 同上 (同一 prebuilt 包) | 下载 tar 74.3MB (API 实测)，model.int8.onnx ≈79MB | 估计 ~250-350MB，待板端实测 | 中——小模型无官方 CER，社区口径"比 large 差，比 whisper tiny 的中文稳" | 非流式 | 同上 | ⭐ 低 |
| whisper.cpp + **tiny (q5_1 量化)** | ❌ 无官方 prebuilt (v1.9.5 release assets 为空)，须**板端源码编译** (g++10.2 可行) | tiny 75MiB / tiny-q5_1 31MiB | 实测 299MB peak RSS (RK3588S 级板子, t=8, 5min 英文长音频口径，见 §2) | 弱——多语种小模型，中文数字/专名易替换错 (社区反馈)，无权威中文 CER | 非流式 (v1.9.5 内置 VAD 但识别是离线) | whisper.cpp MIT；OpenAI whisper 模型权重 MIT | ⭐⭐ 中——要板端编译，且中文效果大概率要重新验证 |
| whisper.cpp + **base (q5_1 量化)** | 同上 | base 142MiB / base-q5_1 57MiB | 实测 410MB peak RSS (同上口径) | 弱-中——比 tiny 略好，中文仍无权威数据 | 非流式 | 同上 | ⭐⭐ 中 |

### 1.1 sherpa-onnx 可用性细节

- **预编译包确认存在**（GitHub API 逐资产核对，不是猜的）：
  - v1.12.12 (2025-09-10 发布): [`sherpa-onnx-v1.12.12-linux-aarch64-shared-cpu.tar.bz2`](https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.12.12/sherpa-onnx-v1.12.12-linux-aarch64-shared-cpu.tar.bz2) — 22,203,350 B ≈ 21.2MB, sha256 `71380442...`。同 tag 还有 `-static` 全静态包、`-jni`、`rknn-linux-aarch64-{shared,static}` (NPU 版)。见 [releases/tag/v1.12.12](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.12.12)。
  - v2.0.0 (2026-10-23 发布，很新): `sherpa-onnx-v2.0.0-linux-aarch64-shared-cpu.tar.bz2` ≈ 26.5MB，同构资产齐全。见 [releases/tag/v2.0.0](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v2.0.0)。
  - 包内含可直接跑的命令行二进制 (`sherpa-onnx-offline` 等) + 动态库 + 头文件，**先命令行验证再写 C++**，集成难度低。
- **glibc 2.31 兼容性**: sherpa-onnx 预编译在 manylinux Docker 里构建，官方工作流保证低版本 glibc 兼容（manylinux 基线 ≈ glibc 2.17，2.31 满足），见 [DeepWiki k2-fsa/sherpa-onnx 构建章节](https://deepwiki.com/k2-fsa/sherpa-onnx)。**但**有第三方项目文档指出其 prebuilt 需要 `GLIBCXX_3.4.29` (GCC 11 的 C++ 标准库符号)，见 [stablyai/orca linux-glibc-compatibility 文档](https://github.com/stablyai/orca/blob/main/docs/linux-glibc-compatibility.md)——Debian 11 自带 libstdc++ 是 GCC 10.2 世代，可能缺符号。**缓解**（按优先级）: ① 改用 `-static` 全静态包（已确认存在）；② 随包分发一份新版 `libstdc++.so.6` 用 RPATH 指向；③ 都不行就板端源码编译（板上 g++10.2/cmake 已装，参考本项目惯例"一律板端原生编译"）。
- **License**: 引擎 Apache-2.0 ([k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx))。模型权重走 [FunASR 模型许可协议](https://github.com/alibaba-damo-academy/FunASR/blob/main/MODEL_LICENSE)（允许自由使用/复制/修改/分享，非标准 OSI 协议；有开源项目明确提醒"不要拿工具链的 license 推断 checkpoint 的 license，锁定具体 revision"，见 [yazses model-strategy](https://github.com/MSKazemi/yazses/blob/9984646beaca8bf6f65a2e69c6c8c8260d03b37d/design/chinese/model-strategy.md)）。本项目内部使用无碍，**商用/对外发布前需复核**。
- **NPU 后手**: sherpa-onnx 官方有 RK3588 RKNN 模型路线（streaming zipformer 中英双语，需配套 librknnrt），见 [官方 RKNN 文档](https://k2-fsa.github.io/sherpa/onnx/rknn/models.html)。本项目 NPU 已被 YOLO+RKLLM 占用，列为 CPU 方案不达标时的后手。

### 1.2 模型质量口碑（带出处）

- **SenseVoice-Small**: FunASR 官方 benchmark，多域中文测试集 CER **7.81%**，GPU 批量 169.6× realtime；对照 Paraformer-Large CER 10.18% (119.6×)、whisper-large-v3 GPU 仅 13.4×——官方结论"CPU 批量转写推荐 SenseVoice-Small"，见 [FunASR benchmark](https://modelscope.github.io/FunASR/zh/benchmark.html) 与 [FunASR discussion #2947](https://github.com/modelscope/FunASR/discussions/2947)。注意这是服务器 GPU 口径的 CER，**端侧 int8 后的 CER 待板端实测**。
- **Paraformer-zh Small**: 小模型无官方 CER 数字（官方 benchmark 只给 large）。社区部署脚本口径"离线 Paraformer zh small int8 ~79-80MB"见 [mica-voice models README](https://gitee.com/yc-fork/mica-voice/blob/main/models/README.md)、[mica-voice 下载脚本](https://gitee.com/dreamlu/mica-voice/blob/97c7ae5eee6ad54f8c4125d80d3e4d8d0a7449c6/models/scripts/download-models.bat)。
- **whisper tiny/base 中文**: 多语种小模型，英文社区反馈 tiny 有词/数字替换问题（[Rhasspy 社区讨论](https://community.rhasspy.org/t/4410/18)）；FunASR benchmark 显示连 whisper-large-v3 的中文 CER 都不如 SenseVoice-Small，tiny/base 只会更差。**中文 CER 无权威数字，按 §5 协议实测后才有结论**。

### 1.3 流式/非流式

- SenseVoice Small、Paraformer-zh Small、whisper tiny/base 都是**非流式**（整段进、整段出）。本项目交互是"按住/按键录 3~5s → 一次性识别"，**非流式完全够用**，且非流式模型精度-速度比优于流式。
- 如未来要做"边说边出字"，sherpa-onnx 有流式 zipformer 中文模型（[asr-models tag 内 sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23.tar.bz2](https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23.tar.bz2)，74MB），本方案不引入。

---

## 2. 实测数据（社区，全部注明板子与口径）

| # | 板子 / 系统 | 引擎 / 模型 | 结果 | 口径与来源 |
|---|---|---|---|---|
| 1 | **RK3588** 8GB, Ubuntu 22.04, 内核 5.10 | sherpa-onnx 流式 Zipformer int8 中文 | 默认 **RTF 0.35**、首字延迟 620ms、CPU 占用 180%；优化后（CPU 亲和性+线程调优）**RTF 0.18**、首字 360ms | [CSDN: RK3588边缘AI实战——Sherpa-Onnx流式语音识别交叉编译与性能优化 (2026-09)](https://blog.csdn.net/weixin_31815843/article/details/166174457)。⚠️ 模型是流式 zipformer 小模型，非 SenseVoice；付费专栏正文未全读，数字以该文摘要为准 |
| 2 | **Turing Pi RK1 (RK3588 级 SoC)**, Ubuntu 22.04 | whisper.cpp 源码编译 (NEON)，`-t 8` | 5 分钟英文 wav: tiny **29.79s / RTF 0.10 / peak RSS 299MB**；base **48.57s / RTF 0.16 / 410MB**；small 141.86s / RTF 0.47 / 889MB。t=4→t=8: tiny +8.5%、base +23.7% 提升（big.LITTLE 非线性） | [Turing Pi 官方博客 (2026-05)](https://turingpi.com/whisper-cpp-piper-tts-arm64-turing-pi-rk3588/)。⚠️ **英文**音频、5min 长音频口径；短音频与中文表现待实测 |
| 3 | **RK3588**, Ubuntu 20.04 | whisper.cpp tiny.en，4 线程 | Load 234.14ms + Encode 681.53ms（2022 年 commit d458fcb，老版本） | [whisper.cpp bench 讨论 #89](https://github.com/ggml-org/whisper.cpp/discussions/89)（经 [Rhasspy 论坛转载页](https://community.rhasspy.org/t/4410/18) 收录）。⚠️ 英文模型、短音频编码单次耗时，不能直接换算 RTF |
| 4 | x86/其他端 (非 RK3588) | SenseVoice-Small int8 包 | 包体 155MB（另一镜像口径） | [otoji ASR bench (2026-05)](https://github.com/snomiao/otoji/blob/9982c23c26fb2b72d593d192c096be2f786c75f2/docs/2026-05-14-asr-bench.md)。⚠️ 仅作包体参考，**性能数字与本板无关** |

**结论性空档（诚实声明）**:SenseVoice / Paraformer 在 **RK3588 CPU** 上的直接实测（延迟、RTF、RSS）目前**没有找到可靠公开数据** → 按 §5 评估协议**待板端实测**。唯一的量级参照是 #1：同属 sherpa-onnx 体系、体量相近的流式 zipformer 在 RK3588 上调优后 RTF 0.18~0.35，据此**推断**（非实测）SenseVoice int8 非流式在 4×A76 上 RTF≤0.5 有较大把握，需实测证实。**禁止在板上测出数字前对外引用任何 SenseVoice/Paraformer RK3588 性能值。**

---

## 3. 推荐

### 3.1 主选: sherpa-onnx v1.12.12 + SenseVoice Small int8 (CPU)

| 项 | 内容 |
|---|---|
| 引擎版本 | **v1.12.12**（2025-09-10 发布，成熟稳定；v2.0.0 三天前刚发不首选，验证后可升） |
| 引擎下载 | https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.12.12/sherpa-onnx-v1.12.12-linux-aarch64-shared-cpu.tar.bz2 （21.2MB；若 GLIBCXX 报错改用同 tag `-static` 包） |
| 模型下载 | https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2 （官方示例脚本引用的 int8 独立包，见 [sherpa-onnx 官方示例](https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/master/python-api-examples/offline-sense-voice-ctc-decode-files.py) 与 [stablyai/orca 提交](https://github.com/stablyai/orca/commit/edc9799757fa6f1bd477a40713f484d7c0ee9789)）；HF 被墙时用镜像 https://hf-mirror.com/csukuangfj/ 同名仓库（镜像用法见 [halcom 文档](http://www.halcom.cn/archiver/?tid-56315.html)） |
| VAD 模型 | https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad_v5.onnx （2,313,101 B ≈ 2.2MB，API 实测） |
| 磁盘占用 | 引擎解压后 + 模型 tar ≈234MB ([pub.dev local_ai_core 引用该 tar 标 234MB](https://pub.dev/documentation/local_ai_core/latest/local_ai_core/Models/senseVoiceSmall-constant.html)；HF 页 [model.int8.onnx 标 239MB](https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/blob/main/model.int8.onnx)) + VAD 2.2MB ≈ **约 260~300MB**（引擎解压体积待实测）。板子 eMMC 57.6G，无压力 |
| 内存占用 | 发布方口径 minMemory 512MB；实际 RSS **待板端实测**（估计 400~600MB 量级：int8 权重 239MB 常驻 + onnxruntime arena + 特征缓冲） |
| HF 页对照 | 模型亦在 [csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17](https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/tree/main) |

**四个约束逐条落账**:
1. **glibc 2.31 能跑**: manylinux 构建基线 glibc 2.17 ✅；风险点只剩 GLIBCXX_3.4.29，有 `-static` 包兜底，最坏板端编译（见 §1.1）。
2. **模型 <200MB**: ⚠️ **不满足**——int8 包 ≈234MB，超 200MB 预算约 15%。保留主选的理由：磁盘实际富余 57G、超的这 30MB 换来官方 CER 7.81% 的中文质量档位（§1.2）；若该约束是硬性的，直接用备选（79MB）。
3. **中文效果好**: 官方 benchmark 中文 CER 7.81%，候选中唯一有权威中文数字的模型 ✅。
4. **CPU 延迟可接受**: 无直接实测（§2 空档）；量级参照（同引擎流式 zipformer RTF 0.18~0.35）+ 非流式一次 3~5s 音频的形态，推断 RTF≤0.5 可达，**待实测证实**。

### 3.2 备选: sherpa-onnx v1.12.12 + Paraformer-zh Small int8 (CPU)

| 项 | 内容 |
|---|---|
| 引擎 | 同主选（同一个 prebuilt 包，只是换模型目录） |
| 模型下载 | https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-paraformer-zh-small-2024-03-09.tar.bz2 （**74.3MB，GitHub API 实测 77,920,048 B**）；包内 model.int8.onnx ≈79MB（[mica-voice 口径](https://gitee.com/yc-fork/mica-voice/blob/main/models/README.md)） |
| 磁盘/内存 | 磁盘 ≈100MB 内；内存估计 ~250-350MB，待实测 |
| 四约束 | glibc 同主选 ✅；**模型 79MB 严格满足 <200MB** ✅；中文效果无权威 CER（比 large 差），质量档位存疑 ⚠️；CPU 延迟：模型更小，RTF 预期比主选更低，待实测 |

**为什么它排第二**: 体积/速度占优，但中文质量没有权威背书；而本场景（对 Agent 说指令）**识别准确率 > 一切**，宁可多花 150MB 磁盘。若主选实测 CER 达标即淘汰备选；若主选内存/RAM 压力大（与 RKLLM 2.8GB 共存吃紧）则启用备选。

**不推荐 whisper.cpp 作为本场景主力的理由**: 无 aarch64 prebuilt（要板端编译，工程量+glibc 风险反转到自身）；tiny/base 中文质量无权威数据且社区口碑偏差；唯一优势（MIT 纯净 license + 极小模型）对"中文指令识别"这个目标不构成决定性收益。留作 license 敏感场景的 B 计划。

---

## 4. 集成设计（按键 → ASR → Agent 全链路）

### 4.1 音频链路框图

```
[按键 / Web 按钮]                        (触发源可插拔: ADC 按键板端未测, 先用网页 8080 页面按钮)
      │ 按下触发
      ▼
┌─────────────────────┐  S16_LE / 16kHz / 单声道 PCM
│ arecord 录音 3~5s    │ ──────────────────────────────┐
│ -D hw:5,0            │                               │
│ -f S16_LE -r 16000   │                               ▼
│ -c 1                 │               ┌─────────────────────────────┐
└─────────────────────┘               │ VAD: silero-vad v5 onnx      │
                                       │ (sherpa-onnx VAD 模块加载)    │
                                       │ 截掉首尾静音, 拼有效语音段     │
                                       └──────────────┬──────────────┘
                                                      │ 有效段 PCM
                                                      ▼
                                       ┌─────────────────────────────┐
                                       │ ASR: sherpa-onnx-offline     │
                                       │ SenseVoice Small int8 (CPU)  │
                                       │ 输出: 中文文本                │
                                       └──────────────┬──────────────┘
                                                      │ 文本
                                                      ▼
                                       ┌─────────────────────────────┐
                                       │ 门禁: 非空 && 长度≥2 字       │
                                       │ 不过 → 显示"没听清", 绝不触发 │
                                       └──────────────┬──────────────┘
                                                      │ argv 传入
                                                      ▼
                                       ┌─────────────────────────────┐
                                       │ rkllm_embodied_agent          │
                                       │ (Qwen3.5-2B RKLLM ToolCall)  │
                                       │ 独立进程, 文本进/文本出        │
                                       └──────────────┬──────────────┘
                                                      │ 结果文本 (stdout/JSON)
                                                      ▼
                                       [结果展示: 先日志/stdout, 后接 7 寸信息屏]
```

### 4.2 录音命令示例（16k 16bit，板上已实测该规格可录）

```bash
# 按下按键后录 5 秒, 单声道 16kHz 16bit, 写临时 wav
arecord -D hw:5,0 -f S16_LE -r 16000 -c 1 -d 5 -t wav /tmp/voice_cmd.wav
```

- **`-c 1` 单声道的取舍**: ASR 模型输入就是单声道；本板两路 USB 麦是**两个独立 ALSA card** (`hw:5,0` / `hw:6,0`)，ALSA 层面无法聚合成一路立体声，`-c 1` 是唯一自然选择。双麦的价值不在带宽而在**选路**：哪路信噪比好用哪路（选路逻辑属后续优化，先固定 `hw:5,0`）。
- `hw:` vs `plughw:`: `hw:5,0` 直通设备、参数必须设备原生支持（16k/S16_LE 已实测 OK）；`plughw:` 允许 ALSA 插件做重采样转换但引入额外延迟。本场景固定规格，用 `hw:`。
- 录音时长 5s 上限：Agent 指令句长 2~4s 足够，硬上限防"忘停"。
- 常见坑（Windows→板子命令带引号会被吃）：复杂管道不要内联 SSH，写 .sh 上去执行（项目既有约定）。

### 4.3 VAD 选型

- **选 sherpa-onnx 自带 VAD + silero-vad v5 onnx**（`silero_vad_v5.onnx`，2.2MB，见 §3.1 下载）：与 ASR 同一个进程/同一套库加载，无新增依赖；silero-vad 是社区主流（sherpa-onnx 官方 asr-models tag 直发 v4/v5 两版）。
- 用法：录音整段喂 VAD → 输出"有语音的区间" → 只把有效段送 ASR。收益：① 3~5s 里有一半是静音时，ASR 计算量近似砍半；② 全静音段直接判"没听清"，不产生幻觉文本。
- 参数初值（silero 常规口径，板端按实测调）：start 阈值 ~0.5、end 静音窗 ~0.5s、min_speech ~0.25s。**待实测**。

### 4.4 与 Agent 的对接（进程边界清晰）

- **接口形态**: 识别文本作为 **argv** 传给已有 `/root/rkllm_deploy/rkllm_embodied_agent`：

  ```bash
  TEXT=$(sherpa-onnx-offline ... 2>/dev/null | 提取文本)
  if [ -n "$TEXT" ] && [ "${#TEXT}" -ge 2 ]; then
      /root/rkllm_deploy/rkllm_embodied_agent "$TEXT"
  else
      echo "[voice] 未识别到有效指令, 不触发 Agent"
  fi
  ```

- **进程边界**: 录音+VAD+ASR 是一个独立进程/脚本，Agent 是另一个进程，交接物只有一条 UTF-8 文本（argv）。C 侧正式实现用 `posix_spawn`/`execvp` 传 argv 数组，**不要用 `system()` 拼字符串**（中文与引号注入风险，项目已有"SSH 引号被吃"前科）。
- **安全门禁（硬规则）**: ASR 失败、空文本、长度 <2 字、纯标点——**一律不触发 Agent 执行器**。宁可"没听清"也不能把垃圾文本喂给 Tool Calling 让大模型猜着执行（灯控制/外设是真实硬件动作）。门禁放在 ASR 进程内，Agent 永远不处理空串。
- **结果显示**: 第一版 = stdout + systemd 日志（`journalctl -u voice-asr`）；第二版接入已规划的 7 寸信息屏（Web 页面已有 8080 预览基建可复用），显示「识别文本 + Agent 回复」。不做 TTS、不做唤醒词（本项目明确范围外）。

---

## 5. 评估协议（板端实测怎么做）

### 5.1 固定指令集（10 条，覆盖温度数字问句 / 灯控制 / 画面问句，全部对齐现有 Agent 工具）

| # | 指令 | 考察点 |
|---|---|---|
| 1 | 现在开发板温度是多少 | 基础问句 |
| 2 | CPU 温度超过四十五度了吗 | **数字**（四十五=45 转写一致性） |
| 3 | 把板载指示灯调成急闪报警模式 | 灯控制 + 长指令 |
| 4 | 恢复心跳呼吸灯 | 灯控制 + 复合词 |
| 5 | 把灯关掉 | 灯控制 + 短指令 |
| 6 | 画面里有几个人 | 视觉问句 + 量词 |
| 7 | 现在检测到什么物体 | 视觉问句 |
| 8 | 三分钟后提醒我喝水 | 数字 + 未实现工具（考察幻觉文本行为） |
| 9 | 打开摄像头预览 | 动词开头短指令 |
| 10 | 你叫什么名字 | 纯对话（无工具调用） |

每条安静室内录 1 遍（清晰度不足可重录但需标注），保存原始 wav 作回归集。

### 5.2 指标定义

- **CER**（字错误率）= (替换 S + 删除 D + 插入 I) / 参考文本字数 N，汉字序列上算编辑距离（Levenshtein），**先去掉标点与空格**再比对；数字统一按"汉字读法 vs 阿拉伯数字"两种口径各算一遍（指令 2/8 是主要差异源）。板端无 pip 装不了 jiwer → 用 PC 端 Python 脚本或 30 行 C 的 DP 小工具。
- **RTF**（实时率）= ASR 推理耗时 / 音频有效时长。**分两个口径记**：warm（模型已加载，纯推理，入门槛用这个）与 cold（含模型加载，用户首按体验用这个）。3~5s 短音频跑 20 次取 p50/p95，排除首跑。
- **内存**: `/proc/<pid>/status` 的 VmHWM（峰值 RSS），ASR 单进程与「ASR+RKLLM 同驻」两种工况各记一次。

### 5.3 通过门槛（初版，可调）

| 指标 | 门槛 | 说明 |
|---|---|---|
| CER（10 条平均） | ≤ **10%** | 参照主选模型官方档位（7.81%）与 int8 掉点余量定的工程门槛 |
| RTF（warm, p50） | ≤ **0.5** | 5s 音频 2.5s 内出文本即可接受 |
| 附加 | 10 条中 ≥9 条「文本可被 Agent 正确执行/回答」 | 端到端有效口径，比纯 CER 更贴目标 |
| 附加 | ASR 进程 RSS 峰值 ≤ 1GB | 给 RKLLM 2.8GB + YOLO 留余量 |

未达标处置：CER 差 → 查录音链路（底噪/距离）→ 换 Paraformer small 对照 → sherpa-onnx RKNN NPU 版后手；RTF 差 → 绑 4×A76 大核（`taskset`）→ 4 线程调优（参照 §2 #1 的调优手法）。

---

## 6. 风险表

| 风险 | 影响 | 缓解 |
|---|---|---|
| prebuilt 包缺 `GLIBCXX_3.4.29`（Debian11 libstdc++ 是 GCC10 世代） | 动态库版二进制起不来 | ① 同 tag `-static` 全静态包；② 随程序分发新版 `libstdc++.so.6` + RPATH；③ 板端源码编译兜底（板上 g++/cmake 已就绪） |
| 模型/引擎下载被墙（GitHub/HF 直连不稳） | 拿不到包 | PC 侧下载后 scp 上板；HF 走 hf-mirror.com 镜像；社区还有 gitee 下载脚本（mica-voice）可抄路径 |
| USB 麦克风底噪/环境噪声拉高 CER | 识别错字甚至空输出 | silero VAD 过滤静音段；近距离说话；`-c 1` 固定高信噪比那一路；评估协议里专门录"安静 vs 日常"两组对照 |
| CER 不达标 | 语音入口不可用 | 降级路径：保留文本输入（网页框/键盘）作为兜底交互；模型侧按 §5.3 处置链换档 |
| ASR 与 RKLLM 同驻内存压力 | 8GB 被 2.8GB LLM + YOLO + ASR 挤爆 | ASR 进程"录完即退出"或按需冷启动（cold load 计入口径）；RSS≤1GB 写进门槛；备选模型 79MB 内存减半 |
| NPU 争用（若走 sherpa-onnx RKNN 版） | 与 YOLO/RKLLM 抢 3 核 NPU + librknnrt 版本耦合 | 本方案默认全 CPU；RKNN 版只作后手，且板端 librknnrt 已升 2.3.0 需做兼容复测 |
| FunASR 模型许可协议非标准 OSI | 商用/对外发布合规风险 | 项目内部学习使用无碍；发布前复核 MODEL_LICENSE 原文并锁定模型 revision（yazses 的教训：不能拿工具链 license 推断 checkpoint） |
| whisper 路线中文数字替换错（若启用 B 计划） | 「四十五度」听成「15 度」类指令错执行 | whisper 仅作 B 计划；启用时数字类指令（§5.1 的 2/8 条）单独设 CER 门槛 |
| 付费专栏来源数字不可复核（§2 #1） | 引用数字失真 | 该条标注"摘要口径"，板端实测覆盖后才可对外引用 |

---

## 附: 与项目现状的衔接

- 音频硬件已就绪: 双 USB 麦 `hw:5,0`/`hw:6,0`，16000Hz/16-bit 实测录制正常（见 AGENTS.md「已完成的事」）。
- Agent 侧已就绪: `/root/rkllm_deploy/rkllm_embodied_agent`（Qwen3.5-2B Tool Calling）已跑通温度查询与 LED 控制（见 AGENTS.md 第三阶段）。
- 本文档只做选型与设计; 下一步实施顺序建议: ① PC 下载主选引擎+模型 scp 上板 → ② 命令行跑通 10 条指令 → ③ 按 §5 出实测数据回填 BENCHMARKS.md → ④ 再写 C++/脚本集成。
