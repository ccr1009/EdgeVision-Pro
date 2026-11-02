# AGENTS.md — RK3588 项目总纲(每次新对话请先读这个)

> 目的:让新的 AI 对话**快速了解「我是谁、在做什么、进展到哪、下一步想干嘛」**,不用重复背景。
> 本项目根目录:`D:\a研究生\项目\rk3588项目`
>
> **四份核心文档,按这个顺序看**:
> - `edgevision-rk3588/docs/ARCHITECTURE.md` —— **项目架构总览**(三条技术线 + 共享资源争用 + 关键数字,**讲项目从这里开始**)
> - `edgevision-rk3588/lab/README.md` —— **源码在哪、哪个文件是活的、怎么上线**(改代码前先看)
> - `edgevision-rk3588/docs/OPEN_ISSUES.md` —— **未关闭问题清单**(按 A/B/C 分类 + 优先级)。改代码前先看这张表,别重复已有取舍
> - `edgevision-rk3588/docs/CURRENT_STATUS.md` —— 现在能干什么 + 下一步(**接手先看这份**)
> - `edgevision-rk3588/docs/BENCHMARKS.md` —— 性能/精度实测数据(含测量口径与局限)
> - `edgevision-rk3588/docs/TROUBLESHOOTING.md` —— 踩过的坑:现象 → 排查 → 根因 → 解法(面试可当素材)。**遇到新问题必须往这里追加,规则见下方「工作约定」**
>
> **专项技术笔记**(`docs/tech_notes/`,按需查阅):
> - `AGENT_TOOLS.md` —— Agent 7 个硬件工具的契约与设计取舍(**Agent 岗必读**)
> - `AGENT_MULTISTEP_DEMO.md` —— 两幕演示设计 + 3 个缺陷复盘
> - `AGENT_TEST_PLAN.md` —— 12 条用例 + 指标口径 + 故障注入
> - `VOICE_ASR_PLAN.md` —— 语音链路(ASR/TTS/唤醒/编排)完整实测
> - `PATROL_DAEMON_DESIGN.md` —— 快慢双系统,为什么快层不用 LLM
> - `BEV_GATEWAY_ARCHITECTURE.md` —— 视觉网关与 BEV 投影
> - `CAMERA_FPS_ROOT_CAUSE.md` —— 摄像头帧率根因分析
> - `NPUD_SERVICE_PLAN.md` —— NPU 服务化(未开工的计划)
>
> **面试准备**(`docs/interview/`):`INTERVIEW_PREP_PLAN.md`(总计划)、`W1-文件IO与进程` ~ `W6-端侧Agent`(分主题)、`高频八股速查.md`
>
> 学习路线:`edgevision-rk3588/docs/LEARNING_PLAN_4WEEKS.md`

---

## 📌 工作约定(AI 必须遵守)

### 1. 只记录「面试能讲」的问题 —— 这份文档是校招素材库

`docs/TROUBLESHOOTING.md` 的**唯一用途是面试素材**,不是流水账。记录前先过三问:

| 自检 | 要什么 |
|---|---|
| ① 根因能讲出**底层原理**吗? | 内存 / 并发 / 量化 / 访存 / 协议 / 文件系统… 讲不出原理 = 降级 |
| ② 排查过程有**方法论**吗? | 分层排除、对照实验、误差基准、二分定位 —— 不是「试了一下就好了」 |
| ③ 有**可量化证据**吗? | 具体数字、`max\|Δ\|`、耗时对比、可复现命令 |

- **三问全过** → 按「现象 → 排查 → 根因 → 解法」写全文(格式照抄已有条目)
- **过一两问** → 只在文末「速查清单」加一行,不展开
- **一问不过** → **不记**(改 PATH、装包、重启好了,这类没人会问)

**写的时候盯着这个**:面试官会从哪个角度追问?
- ✅ 好例子:「合成校准集 → cls 分支塌缩成常数 0.5,而余弦相似度还有 0.9965」
  → 反常识 + 有数字 + 能扯到量化 scale 定标原理,能扛住三轮追问。
- ❌ 坏例子:「换个镜像源终于把 numpy 装上了」→ 没人问,白占篇幅。

**要的是「能讲透的 5 条」,不是「记全的 50 条」**。文档臃肿比没文档更糟 —— 面试前你只会翻那几条真能讲的。

其他细则:
- **时机**:解决后立刻写(攒了就丢细节),但**先判断值不值得写**。
- **同一个坑合并**:换个地方又踩到,补进原条目标「第二处」,不要新开一条。
- **禁止写「就好了」**:没有根因、没有证据的记录,一律删掉。

### 2. 完成实质性工作后同步三份文档

| 文档 | 要补什么 | 备注 |
|---|---|---|
| `docs/CURRENT_STATUS.md` | 现在能干什么 + 下一步 | **最容易忘**;不更新会让下次按过时路线走 |
| `AGENTS.md` 第 3 节「已完成的事」 | 一条带证据的 ✅ | 新对话靠它快速接手 |
| `docs/BENCHMARKS.md` | 性能/精度数字 | 必须写清测量口径与局限,不同口径不能混在一张表里 |

### 3. 不许用假数据冒充结果

演示、汇报、截图里的任何数字,**必须来自真实链路**。禁止用 `frame_id % N` 之类的动态偏移、
硬编码置信度、`if (detections.empty()) 补一个框` 来「让画面好看」。
这类代码一旦存在,后面所有人都会以为它是真的(本项目已经踩过两次,见 TROUBLESHOOTING 第 6 条)。

---

## 0. 一句话
一名**广州大学物联网工程本科、南方科技大学电子信息研一硕士**，手里有一块 **RK3588 开发板**，研究与求职方向为 **Linux 应用开发 + 端侧推理部署 + Edge Agent 应用开发**，目标冲刺 **大湾区（大疆/华为/地平线/汇川/比亚迪/机器人新贵）端侧 AI 部署与嵌入式 Linux 研发岗**。

## 1. 目录结构(本项目根)
```
D:\a研究生\项目\rk3588项目\
├── README.md                  ← 工作区使用说明(入口,先看这个)
├── AGENTS.md                  ← 本文件(总纲)
├── edgevision-rk3588\         ← ★ 主项目,唯一 git 仓库(含 stereo3d 双目子目录)
├── rkllm_deploy\              ← 板上 /root/rkllm_deploy 的本地镜像(runtime + demo)
├── rk3588开发板配套资料\       ← 英码官方资料(SDK/镜像/工具/手册,27.5GB,只读)
├── _tools\                    ← 本地辅助脚本(体检/定量实验/排查复现链)
├── _workbench\                ← 临时与历史产物(探针/回归结果/会话转录/归档),不进 git
└── .workbuddy\                ← AI 会话记忆
```
要点:
- **双目项目已归一**:代码权威副本在 `edgevision-rk3588/stereo3d/`(已补 `evidence/` 与证据图);
  原根目录 `stereo3d-rk3588/` 已移入 `_workbench/stereo3d-rk3588-archive/`,只留标定原图与合成真值图。
- `_` 前缀 = 非源码、非交付物,清掉不影响项目运行。详见 `_workbench/README.md`、`_tools/README.md`。
- **根目录不是 git 仓库**,唯一仓库是 `edgevision-rk3588/`。

## 2. 硬件与环境
| 项 | 说明 |
|---|---|
| 开发板 | **EVM3588-B(英码科技 EMA)**,SOM3588-B 核心板,**8GB + 64GB eMMC** |
| 系统 | **Debian 11 (bullseye)**,内核 5.10.110,预装出货镜像(系统在 eMMC,不用刷) |
| 电源 | 12V DC 圆头(套件自带) |
| 网络 | **有线**(宿舍网口)→ 板子 IP **`<板子IP>`**;WiFi 已关闭 |
| SSH | `ssh root@<板子IP>` 密码 **`<板子密码>`**(`ema` 也是 `<板子密码>`) |
| 已知正常 | 内存 7.5Gi、eMMC 57.6G、**NPU v0.9.2 + 3核**,`librknnrt.so` 已装,RGA/MPP 正常 |

### 关键坑(别再踩/别再误判)
- **`/dev/rknpu` 不存在是正常的**:新版 NPU 驱动走 **DRM 接口** `/dev/dri/renderD129`,不是缺驱动。
- dmesg 里 **`__cfg80211_connect_result` WARNING** 是 **WiFi 驱动(aic8800)** 的,无害。
- **WiFi 老掉线**:宿舍公共网 + aic8800 驱动不稳 → 一律**用有线**;WiFi 已关(否则日志刷重连)。
- **交叉编译有 glibc 风险**(Ubuntu22 的 2.35 vs 板子 Debian11 的 2.31)→ **一律在板子上原生编译**。
- 刷机镜像 `.img` 是 **RKFW 整包**,不能直接拷进 TF 卡;板子系统已在 eMMC,**不需要刷机**。
- **实测 37ms/帧 ≠ NPU 慢**:**49% 的时间在 CPU 上**(YOLOv8 的 DFL 头 Transpose/Softmax/Reshape 回退),NPU 主体只占 19.23ms 且三核并行时每核仅 49% 负载 → **优化先改模型导出(砍 DFL 头),别一上来就做零拷贝**(实测喂数据方式对延迟无影响)。
- **Windows 上跑远程命令别用复杂内联**:`ssh root@<ip> 'a | b; c'` 里的引号会被 PowerShell/ssh 吃掉 → 一律**写成 .sh 脚本 scp 上去再 `bash` 执行**。也别用 PowerShell 读写含中文的 UTF-8 源文件(会把编码搞坏),用编辑器/工具直接改。
- **VSCode 连不上不一定是网络断了**:VSCode 换版本后要下新 commit 的 **CLI(11.5MB) + server(208MB)**,板子(校园网/IPv6 出口)下载会**断流**,留下**大小对但内容全是 `\0`** 的残包 → Remote-SSH 报 `Got bad result from install script`。解法:PC 侧 `curl` 下 `cli-linux-arm64` 和 `server-linux-arm64`,scp 上去手工布局(详见 `docs/TROUBLESHOOTING.md` 第 10 条);已在 PC `settings.json` 设 `"remote.SSH.localServerDownload": "always"` 从根上绕开。

## 3. 已完成的事(有证据)
- ✅ 板子自检全过(内存/存储/开机/NPU/驱动),脚本:`edgevision-rk3588/scripts/check_board.sh`(板上 `/root/check_board.sh`)。
- ✅ **远程开发环境**:有线 SSH 通,电脑可直接 `ssh root@<板子IP>` 粘贴命令。
- ✅ 板上装了 **g++ 10.2.1 / make / cmake**;项目源码传到 **`/root/edgevision/`**。
- ✅ **真实 NPU 推理跑通**:`edgevision_pro_rknn` 原生编译成功,加载 `yolov8n_rk3588_i8.rknn` 成功。注意它跑的是**合成输入仿真**(3 路 × 100 帧 = 300 帧,输入是整幅灰图),实测 **25.1 FPS 聚合**;纯推理边界的真实单核实测是 **37.19 ms/帧 ≈ 26.9 FPS**(见 `docs/BENCHMARKS.md` 2.2 节,含 p50/p95/p99)。README 声称 150+ 是理想值,**以实测为准**。
- ✅ 板载 **LED** 设为 `heartbeat` 呼吸闪 + **systemd 服务 `led-heartbeat`** 开机自启。
- ✅ **TF 卡**(64G 爱国者,V30,正品,读83/写39)挂到 **`/mnt/tf`**,已写 **`/etc/fstab` 开机自动挂载**(exFAT 走 `exfat-fuse`,已建 `/sbin/mount.exfat` 软链接)。
- ✅ 音频设备确认(es8388,card 1),测试音已播放。
- ✅ 一次「文件IO」教学跑通:读 `/sys/class/thermal/thermal_zone0/temp`(板上有 `/root/lab/`)。
- ✅ 文件 IO 进阶(学习 D2):`stat` / `open + lseek(SEEK_END)` 测 YOLO 模型大小,`perror` 排错。
- ✅ 外设控制与 ioctl(学习 D3):用 `write` 控制板载 LED 快闪与常亮,用 `ioctl` 读取终端尺寸。
- ✅ 进程生命周期与排错(学习 D4):`fork` / `execvp("uname")` / `waitpid`, 实操捕获并超度 `<defunct>` 僵尸进程与退出码。
- ✅ 目录遍历与 proc 探秘(学习 D5):`opendir` / `readdir` / `closedir`, 遍历 `/proc` 扫描板上 247 个进程名 (做成 Mini-ps)。
- ✅ 第1周综合大练兵(学习 D6):自动遍历 RK3588 全部 7 大温区(CPU/GPU/NPU)，格式化追加写入持久化日志 `thermal.log`。★ **第1周系统编程全面通关！**
- ✅ 多线程并发编程(学习 D8):`pthread_create` / `pthread_join`、`void*` 传参、主线程与子线程生命周期。
- ✅ 互斥锁与竞态复现(学习 D9):亲测 2 线程百万并发丢失数据(`1000093`)，深入理解 CPU 读-加-写非原子操作，用 `pthread_mutex_t` 锁住 200 万并理解死锁。
- ✅ 硬件级原子操作(学习 D12 顺手秒杀):`std::atomic` 无锁自增与面试选型对比。
- ✅ 条件变量与生产者消费者(学习 D10):`pthread_cond_wait/signal`，彻底吃透解锁-休眠-重新加锁三步与防虚假唤醒 `while` 循环，模拟摄像头与 NPU 流水线。
- ✅ 线程池与3核抢单(学习 D11):3个 NPU Worker 线程抢任务，亲测锁粒度最小化（抢完立即开门），实现 3 倍吞吐。
- ✅ 综合大实战·带队列任务池(学习 D13):`std::queue` 削峰填谷，彻底解耦摄像头拍照与 NPU 推理，吃透 RK3588 独家 `rknn_set_core_mask` 核心硬绑定！★ **第2周多线程并发全面通关！**
- ✅ TCP 服务端与跨机通信(学习 D15):原生 POSIX Socket 全流程(`socket/bind/listen/accept/recv/send`)，吃透双 FD 迎宾员-服务员模型，彻底搞清大端小端与 `htons`，电脑 Python 成功跨机呼叫板子 9000 端口并收到 Echo 回复。
- ✅ 结构体推流与TCP粘包处理(学习 D16/D17):掌握【定长包头+结构体包体】帧边界切分机制，板端 C++ 结构体直接内存映射直推，电脑 Python 成功解包行人与汽车坐标 BBox，吃透二进制直推 vs JSON 解析性能优势。
- ✅ UDP 广播与网络选型哲学(学习 D18):掌握 `SOCK_DGRAM` 与 `sendto` 无连接通信，吃透校园网/大网环境交换机拦截广播机制；深入理解无人机摇杆控制(UDP低延迟容忍丢包) vs 固件升级(TCP绝对可靠)的经典面试选型。
- ✅ 非阻塞IO与反例排错(学习 D19):用 `fcntl(O_NONBLOCK)` 解除 `accept()` 阻塞卡死，通过 A/B 对照实验亲眼见证阻塞反例如何瘫痪 AI 推理主循环，深入理解 `EAGAIN` 机制与 `epoll` 高性能架构方向。
- ✅ **D20 第三周大综合(全链路闭环大圆满)**:板上 `/root/lab/w3_d20_real_npu_gateway.cpp` 跑通！真正加载 6.2MB `yolov8n_rk3588_i8.rknn`，3 个物理 NPU 核心硬绑定真并行推理(实测稳态 40.8~42.7ms/帧)，通过条件变量安全队列无缝直通 TCP:9000 推流网关，电脑 Python 上位机实时解包 12 帧 BBox！★ **第 3 周网络与网关全链路大圆满通关！**
- ✅ **守护进程与开机自愈服务(学习 D22/D25)**:掌握 `daemon(0,0)` 摆脱 SIGHUP 终端挂断信号与会话迁移；编写 `/etc/systemd/system/edgevision.service`，实现插电开机自启与 `Restart=always` 突发异常 5 秒自动自愈。
- ✅ **信号优雅退出与断网防崩(学习 D23)**:掌握 `sigaction` 捕获 `SIGINT/SIGTERM`，实现相机硬件流与 NPU 上下文有序安全退火释放；屏蔽 `SIGPIPE` 彻底解决上位机客户端断开导致主程序闪退暴毙。
- ✅ **epoll 事件驱动高并发(学习 D24)**:掌握 `epoll` ET(边缘触发)单线程反应堆，彻底吃透红黑树挂载、就绪双向链表、$O(1)$ 精准唤醒与非阻塞 I/O EAGAIN 机制。★ **第 4 周系统服务化底座大满贯！**
- ✅ **第一阶段终局工业级边缘网关(EdgeVision V1.0)**:板端 `/root/lab/w4_edgevision_gateway.cpp` + 电脑 `pc_client_viewer.py` 全链路联调通关！真实 UVC 摄像头输入 + OpenCV 640x640 预处理 + RK3588 3核物理硬绑定 NPU 真机推理 + TCP 9000 结构体直推 + systemd 无人值守守护。实测稳态 **35.8 ~ 36.4 ms / 帧**，连续推流 **800+ 帧稳定 24.2 FPS**！★ **第一阶段（第 1~4 周）全线 100% 圆满通关！**
- ✅ **双目与双麦多模态传感器调通**:
  - 视觉:Cam 1 `/dev/video81`(已旋钮远焦对焦锐利)+ Cam 2 `/dev/video83`，吃透 CMOS 自动曝光(AE) 15 帧预热收敛原理。
  - 音频:两路独立 USB 麦克风 ALSA 节点 `hw:5,0` 与 `hw:6,0`，实测录制 16000Hz 16-bit PCM 立体声无暇，具备多模态 Edge Agent 视听双全基础！
- ✅ **板端性能实测与逐层剖析(2026-10-17)**:新增 `edgevision-rk3588/deploy/cpp/bench/`(`bench_npu.cpp` / `bench_multicore.cpp`)。实测:单核 **37.19ms / 26.9FPS**;3 个独立 context 绑 3 核真并行 **59.3FPS / 2.21x**(三核各仅 49% 负载);逐层剖析出 **49% 时间(18.44ms)耗在 CPU 回退的 YOLOv8 DFL 检测头**(Transpose 9.2ms + Softmax 4.5ms + Reshape 4.4ms),NPU 主体仅 19.23ms。数据与优化路线已回填 `docs/BENCHMARKS.md` 2.2 节。
- ✅ **免密 SSH**:`ssh root@<板子IP>` 不用输密码(密钥 `~/.ssh/id_ed25519_rk3588`,别名 `ssh rk3588`;配置脚本 `_tools/setup_board_ssh.py`)。
- 🚧 **第二阶段开工(2026-10-21):DFL-Free 模型改造 + 量化精度修复**
  - **ONNX 图手术砍掉 DFL 头**:`convert/export_dflfree_onnx.py` 把 `yolov8n.onnx`(262 节点, 1 输出 84ch) 切成 `yolov8n_dflfree.onnx`(228 节点, 2 输出: `raw_box` 64ch + `cls_sig` 80ch),整个 DFL 解码链(Transpose/Softmax/Reshape)从图里消失。
    - **数值等价已证明**:numpy 参考解码 vs 原模型输出,box max|Δ|=**0.000122 px**、cls max|Δ|=**0**(float32 精度极限)。
    - **踩坑记录**:DFL 的 16 bin 加权和是**格子单位**(0~15),必须乘 stride 才是像素;第一版漏了这步,结果整好差 8 倍。
  - **C++ 解码器就位**:`deploy/cpp/src/postprocess_dfl_free.cpp` + `include/postprocess_dfl_free.h` + `rknn_engine_dfl_free.{h,cpp}`,板端实测与 numpy 逐位一致(max|Δ|=6.1e-05 px),解码 **1.79 ms/帧**(优化前 3.19 ms,靠"类别外循环"改善 cache)。
  - **量化精度两个根因已定位**:① 旧校准集是**合成色块动画**抽帧(纯色矩形+纯灰底),与真实场景分布严重偏离 → 已重建 `data/calib_v2/`(**50 张板端双摄实拍 + 100 张光照/对比度/伽马/噪声增强 + 20 张合成 = 170 张**);② **toolkit 1.6.0 转的模型喂给 1.5.2 runtime**(版本倒挂,官方不支持)→ 已升级板端 `librknnrt.so` 到 **1.6.0**(旧版备份 `/root/librknnrt_152.bak.so`),升级后旧模型基线复测 **37.07 ms** 无回退。
  - ✅ **转换在板子上完成了(不需要 WSL/PC)**:翻 rknn-toolkit2 2.x 的 `packages/arm64/` 发现**有完整 toolkit wheel**(不是 lite2),板子 Debian 11 自带 py3.9.2 正好 cp39 → 整个转换在板端跑通。
    - 环境:venv 在 `/root/rknn_env`,toolkit **2.3.0**;依赖必须精确(numpy 1.26.4 / onnx 1.14.1 / protobuf 3.20.3 / onnxruntime 1.16.0 / scipy 1.13.1 / opencv 4.9.0.80)。
    - 三个关键坑:① 板子无 pip(手动解压 pip wheel) ② `rknn.api` 顶层 import torch,aarch64 torch 1GB+ → 用 numpy 支撑的最小 stub 顶上(`--stub-torch`) ③ `quantized_iteration` 是 1.x 参数 2.x 已删;`dataset.txt` 相对路径按清单所在目录解析,必须写绝对路径。
    - 板端 runtime 一并升到 **2.3.0**(`/root/librknnrt_230.so`,1.6.0 在 `/root/librknnrt_160.so`,1.5.2 在 `/root/librknnrt_152.bak.so`),实测在 NPU 驱动 0.9.2 上可用。
  - ✅ **性能实测(200 次,EVM3588-B)**:纯推理 **37.19 ms → 15.95 ms(−57%)**;IO 周期 17.48 ms;DFL 解码+NMS **3.09 ms**;**单帧全链路 20.58 ms / 48.6 FPS(原 37.19 ms / 26.9 FPS,−44.7% / +81%)**;模型 6.22 MB → **4.34 MB(−30%)**。工具 `deploy/cpp/bench/bench_dflfree.cpp`。
  - ✅ **量化精度:对照实验证明"校准集是 cls 分支生死线"**。同一 ONNX/toolkit/算法,只换校准集,9 张真实图测 top-1:
    | 模型 | 检出 | 平均分数 | vs FP32 |
    |---|---:|---:|---:|
    | FP32 ONNX | 9/9 | 0.8125 | — |
    | **新真实校准集** | 9/9 | **0.8072** | −0.65% |
    | 旧合成校准集 | 9/9 | **0.5000** | −38.5% |
    - 旧组分数**全部精确等于 0.5000**(=sigmoid(0)),说明 cls 分支被量化整体塌缩成常数、分类头彻底失效;框还对是因为 box 分支没坏。
    - 旧 84ch 模型在 2.3.0 runtime 下**一张都检不出**(阈值降到 0.05 仍为 0),它同时叠了「合成校准集 + 版本倒挂」两个问题,不能当基线。
  - ✅ **真 mAP 实测(COCO128),用来替换"9 张太小"的质疑**:`convert/eval_map.py`(PC 端)+ `deploy/cpp/bench/eval_dump.cpp`(板端导预测),两侧口径完全一致(letterbox 640 / conf 0.01 / NMS IoU 0.45 / COCO 101 点插值)。
    - 数据集:128 张带标注 / **929 个 GT 框 / 71 个类别**;结果 **FP32 mAP@0.5 = 0.6087,INT8 = 0.6002,绝对掉点 0.85 个百分点(−1.4%)**。
    - 局限(引用时必须一起说):COCO128 是 YOLOv8n 的**训练子集**,绝对 mAP 偏乐观,不能对外说"在 COCO val2017 上 0.61";但 FP32/INT8 用同一批图,"见过"因素两边抵消,相对掉点成立。
    - 详见 `docs/BENCHMARKS.md` **2.4 节**(含数据集来源、口径、复现命令)。
  - ✅ **真实检测框网页预览**:`lab/vision/w4_edgevision_preview_real.cpp` 取代原来画假框的 `w4_edgevision_preview.cpp`(旧版那两个 "Person 96%" / "Object 91%" 是硬编码装饰,且模型跑完 `want_float=0` 拿完就 release,检测结果从未被使用)。
    - HTTP 路由:`GET /` 网页外壳(深色、LIVE 指示灯、断流自动重连)、`GET /stream` MJPEG 流、`GET /snapshot` 单帧 JPEG;浏览器直接开 **`http://<板子IP>:8080`**。
    - 预处理改成 **letterbox**,HUD 实时显示 `NPU xx ms + Decode xx ms + FPS + obj 数`;离线验证工具 `deploy/cpp/bench/preview_detect.cpp`。
    - 实测 HUD:`NPU 15.1 ms + Decode 1.2 ms | 20.0 FPS`(~34ms 耗在 MJPG 解码/缩放/画图/JPEG 编码,**瓶颈已不在 NPU**)。
  - ✅ **9000 网关工业级彻底闭环(2026-10-23)**:将 `RKNNEngineDflFree` 3核硬绑定完整接进 `w4_edgevision_gateway.cpp`,彻底铲除假数据!
    - 预处理重构为工业级等比 `letterbox` 灰边填充,并将 BBox 逆变换精准映射回摄像头 1280x720 原始分辨率。
    - 推流协议升级为 248 字节定长帧(`DetectionHeader` 8B + 10x24B `BBox`),上位机 `pc_client_viewer.py` 联动对齐 COCO 80 类解析与坐标映射。
    - 根治背压堆积缺陷:推流队列加 `size>=2` 丢旧与建联瞬间清空存量,杜绝重连 10000+ FPS 历史积压雪崩。实测上位机稳定接收 **19.2 FPS** 纯真实活流。
  - ✅ **摄像头 20fps 之谜已破(2026-10-24)**:三层排查证明**根因是代码从没设 `cap.set(CAP_PROP_FPS,30)`**,V4L2 默认协商到 20fps,**不是硬件上限**。
    - 第 1 层:`v4l2-ctl --list-formats-ext` 显示 720p MJPG **明确支持 30fps**;第 2 层:定位三处源码(`lab/vision/w4_edgevision_gateway.cpp:133-134`、`lab/vision/w4_edgevision_preview_real.cpp:259-260`、`lab/drafts/bench_cam_fps.cpp:11-13`)**都只设了 FOURCC+分辨率,没有一处设帧率**;第 3 层:补一行后做请求值/回读值/实测值三方对照。
    - **实测**:cam1 `/dev/video81` + cam2 `/dev/video83` 各跑 400 帧,**33.33 ms/帧 = 30.0 fps,p95 33.50ms,零抖动**(原 50.03ms / 20.0fps)。
    - ⚠️ **第二层坑**:`cap.get(CAP_PROP_FPS)` **回读值会骗人**——请求 25fps 回读 `25.00`,实测仍 20.0fps。回读是软件期望值,不是硬件节奏。
    - **连带推翻**:BENCHMARKS 2.5.8 的「物理上限 20fps」结论已标注修正(新增 2.5.9);**MPP 硬解/硬编优化因此重新成立**——处理 34ms vs 出帧周期 33.3ms 刚好卡临界,省 11ms 才能吃下 30fps。
    - 工具:`/root/lab/bench_cam_fps2.cpp`(带帧率参数)、`/root/lab/bench_cam_ts.cpp`(帧间隔分布);完整分析 `docs/tech_notes/CAMERA_FPS_ROOT_CAUSE.md` + TROUBLESHOOTING 第 15 条。
  - ✅ **MPP 硬解 MJPG 通关(2026-10-24)**:**硬解 p50 1.03ms vs OpenCV 软解 7.76ms = 7.6×**,200/200 有效帧,9 图 PSNR 49.9~77.2dB。
    - **核心坑**:系统预装 `librockchip_mpp.so`(2021-05-20)与内核驱动版本倒挂 → `decode_put_packet` 恒返回 **-1012 (MPP_ERR_BUFFER_FULL)**,任何参数无效;**用源码编译静态库一行不改直接通**(详见 TROUBLESHOOTING 第 17 条)。
    - **全链路真实闭环**:`edgevision_pro_main.cpp` 全真重写(MPP硬解→NV12→RGB888 CSC→letterbox→NPU→BEV投影),旧 `zero_copy_pipeline.hpp`(fd 硬编码假数据)已废弃铲除;实测 real_000.jpg 检出 `person 0.813 → dist 5.89m`、COCO 图 6 检出,分数与历史口径一致。
    - **新第一大瓶颈:CSC 51~65ms**——CPU 读 uncached DRM 内存(memcpy 仅 188MB/s);正解 = RGA 硬件转换(2.03ms 但本板 RGA 2.1.0 不支持 full CSC,颜色偏差 ±13% 待量化)+ CMake 加 Release(**✅ 2026-10-25 已实测:CSC 52.016→33.583ms = 1.549×,全链路 76.928→55.836ms = −27.4%**,见下方条目)。
    - **0 检出排查教训**(TROUBLESHOOTING 第 18 条):逐层回归验证(CSC 相关性 1.0/letterbox 0.9995/口径对照)后,发现是**测试图本身没目标**(最高分 0.044);检测链路排错必须用已知分数的锚点图。
    - 数据:BENCHMARKS **2.6 节**;组件:`deploy/cpp/src/mpp_jpeg_decoder.cpp` + `third_party/mpp`(源码编译库)。
  - ✅ **实测演示图真机重绘闭环(2026-10-26)**:彻底废除 README 顶部的旧 PC ONNX 仿真假图(`docs/images/bev_demo_snapshot.png`)，升级 `edgevision_pro_main.cpp` 支持 3D 棱边与 BEV 雷达小地图绘制，修复了 `bev_projector.hpp` 历史遗留的俯仰角射线符号偏差与 OpenCV BGR/RGB 颜色通道映射；在 EVM3588-B 真机上由 Release 二进制 `edgevision_pro_rknn` 原生跑通真实 1080p 道路图像，实测 **MPP 硬解 3.3ms + NPU 19.3ms + 7 辆真实车辆 3D 框/BEV 空间定位**，以 100% 真实板端 C++ 输出重绘并覆盖 README 旗舰演示图。
  - ⬜ **下一步**:**① ~~MPP 硬编 JPEG~~ ❌ 已实测否掉(见上方 2026-10-25 条目)② RGA CSC 颜色量化/升级 librga ← 现在 CPU 侧第一优先 ③ ~~CMAKE_BUILD_TYPE=Release~~ ✅ 已完成**。
- ✅ **车载道路实景 3D 实时看板通关(2026-10-26):板子循环读 mp4 → PC 浏览器活流(8090 端口)**
  - **需求**:用户要「板子循环读高速道路视频,浏览器里每帧 7~8 辆车的 3D 彩色立方体线框紧贴车身移动 + 右上角自车 BEV 雷达小地图实时闪烁」。
  - **交付 4 个文件**:`deploy/cpp/src/edgevision_dashboard_main.cpp`(783 行主程序)、`deploy/cpp/include/bev_render.hpp`(共享渲染层,header-only)、`deploy/cpp/web/dashboard.html`(1270 行单文件前端,**零外链**,唯一 `src` 取值 = `/stream`)、`scripts/verify_bev_dashboard.py`(997 行验收脚本,纯标准库 6 类检查)。CMake 新增 target `edgevision_dashboard`,产物 **163656 B**。
  - **实测(200 样本 `/api/stats` 直采)**:输出 **1280×720 / MJPEG(JPEG q80) / 144KB 每帧 / 带宽 ≈3.7MB/s**;**24~27 fps**(p50 **25.71**);单帧全链路 p50 **38.90 ms** / p95 42.03 / max 46.20;每帧真实 3D 目标 **4~11 个 p50 8 个**(正好是用户要的 7~8 辆,零补框);视频 267 帧/圈 **实测已跑 100+ 圈**。
  - **分段(真实打点)**:`capture 5.42 (mp4 解 H.264) + prep 7.84 (resize720p+letterbox+BGR2RGB+跨类别去重+BEV投影) + npu 16.39 (含 DFL 解码 NMS 0.69) + draw 1.58 (3D框+雷达+HUD) + encode 7.55 (imencode q80 720p) = 38.90 ms`。
  - **端到端验收 6/6 全绿**:页面 200/39481B、`/api/stats` `frame_id 1304→1330 Δ+26` fps 26.20 npu 16.42、`/snapshot` 144152B SOI/EOI 齐、`/stream` **271 帧/10.01s = 27.08fps** 无缺 CL 无非法 JPEG、并发 `/api/stats` **33.7ms**、**271 帧 → 271 个不同 crc32**。
  - **视觉确认**:本地 qwen2.5vl 读两帧快照,snapA「蓝框中银色 SUV / 黄框中灰轿车 / 绿框中白轿车 / 红框最右白公交车 + 雷达 #1 15m #2 11m #3 10m + HUD frame 18974 loop 71」,snapB(3s 后)「8 个框 + 雷达 #1 6m…#9 30m + 3D 9/9 frame 22769 loop 85」→ 帧号/圈数/目标数/距离全不同 = 活流。证据图 `docs/images/bev_dashboard_live_720p.jpg` + `_b.jpg`。
  - **三个关键判断(面试可讲)**:① **瓶颈不在 NPU**——NPU 16.39 只占 42%,CPU 侧 `prep 7.84 + encode 7.55 = 15.39` 占 40%,两者相加吃满 33.3ms 源帧周期,这就是到不了 30fps 的直接原因;② **看板比单张版快(38.90 vs 55.84ms)不算优化**——单张版在 1080p 全分辨率做 CSC(33.58ms),看板先 resize 720p 把这块整个砍掉,是分辨率选择的收益;③ **跨类别去重没塞进 `postprocess_dfl_free.cpp`**——那 NMS 是 class-aware,导致一辆车被同时判 truck/bus/car 出 3 个 IoU≈0.96 的重合框(真实样本 `truck 0.348 (977,603,1066,694)` / `bus 0.287 (975,601,1064,693)` / `car 0.266 (980,604,1066,693)`),但它是精度评估公共组件,COCO mAP 0.6087/0.6002 按它算,改它会让已有指标全废 → **口径隔离优先于代码复用**,去重放业务层(阈值 0.65,丢弃率 ≈0.18 框/帧)。
  - **新踩的坑**:① `bev_projector.hpp` 的 `Box3D/Point2D/Point3D` 在 `namespace edgevision`,`bev_render.hpp` 在 `namespace ev_render` 里直接用 → `'Box3D' does not name a type`,修法 = 三行 `using edgevision::Box3D;` 等;② Python `buffer += chunk` 让 `buffer` 在编译期被判成本函数局部变量 → `UnboundLocalError`,改 `buffer.extend(chunk)`(详见 TROUBLESHOOTING);③ **C++ 端 HTML 是运行时读文件不是编译期内联**,改完 HTML 只需 scp 不用重编。
  - **明确未实现(不要宣传)**:H.264 硬解(仍 libavcodec 软解)/ RGA CSC / DMA-BUF 零拷贝 / 目标跟踪(`#idx` 跨帧不连续)/ RTSP / 摄像头输入(当前只吃视频文件)/ 30fps(实际 24~27 = 原速 82~90%)。
  - 数据:`docs/BENCHMARKS.md` **2.10 节**(含口径声明、分段表、与单张版对照、为什么降 720p、去重实测、6 条局限、复现命令)。
- ✅ **第三阶段正式开工(2026-10-24):Edge AI Agent 具身智能体与硬件 Tool Calling 首发通关**
  - **板端控制面 C++ RPC 服务**:`lab/agent/edgevision_agent_server.cpp` 原生编译并在后台监听 **8088 端口**，解耦数据面(9000推流)与控制面(8088 RPC)，暴露三大底层硬件原子工具：
    - `GET /api/status`: 采集板载 7 大物理温区(`/sys/class/thermal/`)、3核 NPU 实时负载(`rknpu/load`)、系统内存；
    - `GET/POST /api/led?mode=xxx`: 物理控制板载 LED(`heartbeat`呼吸 / `alarm`100ms爆闪 / `on`常亮 / `off`常闭)；
    - `GET /api/vision`: 跨进程连接本地 9000 端口，抽取最新一帧 NPU 目标检测结构化数据(类别、置信度、坐标)。
  - **PC 端智能体客户端**:`lab/tests/edge_agent_client.py` 就位，定义符合大模型标准 JSON Schema 的工具说明书，内置 **ReAct 自主规划引擎(Thought -> Action -> Observation -> Final Answer)**，实测输入自然语言巡检指令，成功完成「查7温区(43.5°C) -> 查视觉(采样帧#3604检出马桶/牙刷) -> 研判无过热 -> 下发保持呼吸灯」多步自主硬件闭环！
  - ✅ **端侧大模型 RKLLM 原生推理首发通关(2026-10-24)**:
    - 成功部署 `rkllm-runtime v1.3.1` 原生 C++ 推理环境并打通 NPU 3核协同与 A76 大核绑定；
    - 成功加载 **Qwen3.5-2B (W8A8 高精度量化, 2.8GB)** 端侧大模型；
    - 板端原生编译 `/root/rkllm_deploy/llm_demo` 并实测推理通关，实测解答复杂逻辑与代数方程推导流畅输出，内存可用冗余高达 **6.1 GB**！端侧 AI 大脑底座完全确立！
  - ✅ **纯端侧原生具身智能体 (On-Device Embodied Agent) 闭环首发通关(2026-10-24)**:
    - 源码与程序:`/root/rkllm_deploy/rkllm_embodied_agent.cpp`，板载 NPU 纯单板离线自闭环（完全不依赖 PC 与云端网络）；
    - 突破点:解决 `rkllm_set_function_tools` 参数约束与 Tool Calling 格式，掌握 Qwen3.5 原生 XML 工具语法与 `[{"key": val}]` 严格 JSON 数组回填机制；
    - 真实硬件感知实测:自然语言问“开发板现在温度是多少”，模型在 NPU 上自主调用 `get_system_health`，C++ 拦截驱动读出 **SOC 34.23°C / NPU 35.15°C / NPU负载 65% / 可用内存 4427MB** 并流畅中文总结；
    - 真实执行器控制实测:自然语言指令“把板载指示灯调成急闪报警模式”，模型自主解析参数 `mode="alarm"` 并写入 `/sys/class/leds/work/`，板载硬件 LED 立刻以 100ms 爆闪报警；指令“恢复心跳呼吸灯”，模型自主调用 `mode="heartbeat"` 恢复呼吸闪烁！真正实现端侧「感知-决策-执行」三位一体具身闭环！
  - ✅ **具身 Agent 可靠性强化 + 回归测试全绿(2026-10-24 晚)**:
    - **P0 工具层修复**(`lab/agent/rkllm_embodied_agent.cpp` → 板上 `rkllm_embodied_agent_p0`,72608B):`control_led` 白名单+逐写入检查+回读比对;视觉工具收满 248B、区分离线/超时/半帧、双采样比 `frame_id` 判新鲜;工具轮数耗尽报未完成
    - **两个真实根因**(TROUBLESHOOTING 第 20~22 条):① 内核 5.10.110 无 `CONFIG_LEDS_TRIGGER_TIMER`,写 trigger=timer 是 EINVAL + 上层不验 = 假成功 → alarm 改用户态爆闪;② fork 不 exec 的子进程继承 NPU 内存映射会钉死 196MB CMA;③ 爆闪子进程继承 stdout 管道 → 命令卡死 270s,修法 = exec 前 0/1/2 重定向 `/dev/null`(修后 21s)
    - **板上回归实测 12/12 通过(100%)**,含故障注入(停 edgevision 验视觉离线 + finally 自动恢复 active ✔),单例 13.7~66.0s,全套 ~6.5min
    - 交付:`lab/tests/agent_regression_test.py`(12 用例,支持 `--only`/`--out-dir`)、`lab/tests/bench_contention_sampler.py`(600 样本:zone0 中位 33.31°C / NPU 中位 14% p90 63.5% / 内存 4.41~6.65GB)、`docs/tech_notes/AGENT_TEST_PLAN.md`
    - **快慢双系统巡检守护快层**:`lab/agent/edgevision_patrol_daemon.cpp`(5s 周期状态机+迟滞,快层不加载 LLM),dry-run 60s 12 周期全 vision=online 无翻转;`docs/tech_notes/PATROL_DAEMON_DESIGN.md`
    - **ASR 选型**:`docs/tech_notes/VOICE_ASR_PLAN.md`,主选 sherpa-onnx + SenseVoice Small int8(官方中文 CER 7.81%),待板端实测
  - ✅ **巡检守护真实闪灯验收 + 多步演示闭环(2026-10-25)**:
    - **守护 LED 假成功同坑修复**:守护原用 `trigger=timer`(本板内核没编 `CONFIG_LEDS_TRIGGER_TIMER` 必 EINVAL 且不查返回值) → 改用户态 100ms 爆闪;**告警真实闪灯验收通过**,判据用**占空比**而非"有没有在闪"
      - heartbeat 本身就在闪(实测基线占空比 **20.0%~25.3%**),所以"看到亮度跳变"是无效判据 → 改测 **告警期占空比 49.7%~50.0%,抬升 +24.7~29.7pp**,6s 内翻转 73 次
      - 采样间隔必须远小于方波半周期(爆闪 100ms 亮/100ms 灭,采样取 **0.02s**;若用 0.1s 会与方波同频**混叠**,误判成常亮/常灭)
    - **守护新增阈值可配置**(`--temp-enter/--temp-exit/--person-score` 等,默认仍 75/70°C 不为演示写死)+ 单实例 `flock` 防两个守护抢灯;证据 `lab/docs/patrol_led_evidence.md`
    - **多步演示闭环两幕 PASS**(`lab/tests/agent_multistep_demo.py`,一条自然语言跑完巡检→研判→执行→汇报):第一幕守护真实告警闪灯;第二幕端到端 **106.5~112.2s**、`get_system_health → get_patrol_events → get_led_status → control_led` **4~5 个不同工具**、Observation 20 条全合法 JSON、1350 字中文研判。文档 `docs/tech_notes/AGENT_MULTISTEP_DEMO.md`
    - **最有价值的发现——演示判 PASS 但翻 trace 抓到真缺陷**:Agent 汇报「LED 始终熄灭…导致过热保护逻辑被抑制」,而灯**正在爆闪**。根因 `get_led_current_mode()` 单点读 `trigger` 见 `none` 就读一次 `brightness`,撞上 100ms 爆闪灭相 → **周期性物理量用瞬时单点采样定性 = 混叠误判**;`--no-led` 下模型还把 4 种模式挨个试一遍(白跑 4 轮 NPU 推理)
      - 修法:①`led_user_blink_alive()` 查爆闪 pid + `/proc/<pid>/cmdline` 含 `brightness`(排除僵尸/pid 复用)→ 直接返回 `alarm_blink`;②兜底 12×15ms 多点采样;③`--no-led` 追加 sys_prompt + 拒绝返回 `hint` → `control_led` **4 次 → 1 次**
      - **方法论(面试可讲)**:Agent 给出离谱结论时,先分层定位「模型幻觉 vs 感知层喂错数据」,再谈调 prompt。本次是**模型忠实推理了错误输入**
    - 另修:回归脚本收尾"假恢复"(只写 trigger 不清爆闪子进程,且判据 `"heartbeat" in trigger` 对 `[none]` 也判真)→ 先停爆闪+清 pid、判据改匹配 `[heartbeat]` 生效标记 + `blink_left==0`
    - **第三个缺陷:模型缺领域常识 → 数据对、结论错**。LED 修好后汇报变成「SOC 长期偏高(>40°C)…存在硬件降频甚至**永久损坏**风险」,还臆测「若内部逻辑 0=off,1=heartbeat…」。根因不是幻觉:模型拿到了真数据(49°C、事件里写着"超过 20.00C 阈值"),但**不知道 RK3588 多少度才算热**,只能按"有没有告警"倒推 → 把演示阈值当成真实红线
      - 修法(sys_prompt 补世界知识):①温度常识「正常 30~65°C,>75°C 才算过热,85°C 以上才需担心降频」+「阈值明显偏低(如 20°C)= 验收演示,不要判定真过热」;②禁止臆测工具实现(`control_led` 枚举编码 / sysfs 数值含义),拿不准就说不确定
      - 修复后同一场景:「SOC 49.9°C ✅ 正常(30~65°C)」「**这些是低阈值演示事件,而非真实过热**」,臆测段落消失。**工程启示:不是让模型更聪明,是补上它缺的那块世界知识**
      - ⚠️ **2026-10-28 补记:这条修法有镜像坑** —— 补了 75°C 常识后,模型改用 75°C **去否定守护进程当前 20°C 阈值的真实告警**,答出「44°C 远低于 75°C,不会报警」。见下方「巡检守护运行态外化」。**补常识只能解决「不知道多少度算热」,解决不了「不知道现在生效的阈值是多少」——后者必须靠外化运行态**
    - **全量回归三次 12/12 通过**(`<工作区>/_workbench/test_results/20261025_131835/`、`…_133754/`、`…_135627/`);新增 TROUBLESHOOTING **第 23 条**;多步演示文档 `docs/tech_notes/AGENT_MULTISTEP_DEMO.md` 含 3.4/3.5 两节缺陷复盘 + 第 8 节实测数据表
  - ✅ **巡检守护运行态外化 + systemd 纳管(2026-10-28)**:
    - **问题**:问 Agent「什么时候报警闪烁」,它答「只有超过 75°C 才会报警,当前 44.4°C 远低于阈值,LED 常亮」,**同时出现 20°C 和 75°C 两个互相否定的数字**。上板实测:守护**根本没在运行**,灯是上次异常退出留的僵尸态(`trigger=none`+`brightness=255`)。「常亮」碰巧对,理由全错
    - **根因(可发现性缺陷,不是模型幻觉)**:告警阈值是守护的命令行参数=**进程内运行时状态**,除它自己没人读得到;Agent 只有「历史事件日志」+「写死的默认值 75」两个来源,分不清一条历史日志是正在生效还是三天前的。叠加 `get_system_health` 直接返回 `status: normal_optimal`(基于 75°C 判定)这种**结论字段**,模型连推理都省了
    - **改动**:①守护每周期原子写 `/root/patrol/patrol_state.json`(`.tmp`+`rename`,含 pid/running/阈值/state/led/updated_at/stale_after_s),启动时自检残留僵尸态恢复 heartbeat,退出写 `STOPPED`;②新增工具 `get_patrol_status`,失联判定**三条件与**(running + 时间戳未过期 + `kill(pid,0)==0`;只看时间戳漏「刚死未过期」,只看 pid 漏 pid 复用);③`get_system_health` **删掉 status 结论字段**(给事实不给判断);④修 `get_patrol_events` 丢弃 DAEMON_START 阈值的问题(该行无 summary 键,旧代码把 detail 对象整条拍平);⑤重写 sys_prompt「**告警阈值 ≠ 物理过热阈值**」,阈值偏低时要把「物理温度正常」+「按守护阈值已满足报警」分开讲;⑥`edgevision-patrol.service`(Restart=always + ExecStopPost 恢复 heartbeat + `SuccessExitStatus=0 3`)+ `/etc/default/edgevision-patrol` 走 `PATROL_ARGS`
    - **实测**:守护在跑(20°C)时快照 `THERMAL_ALERT/led=alarm`、LED 采样 `255 0 255 0 255 0 255 0`,Agent 首轮主动调 `get_patrol_status` 并答「**是的,现在会触发 LED 闪烁**……虽然 49°C 在物理安全范围,但守护阈值极低(20°C)」✅;守护已停时答「现在不会自动报警,守护未运行」✅
    - **方法论**:Agent 答错先问「**它有没有可能知道正确答案**」,别先调 prompt;工具返回结论字段(`status: normal_optimal`)是反模式,结论会过期、会基于错前提;判断进程存活要「文件说在跑 + 时间戳新鲜 + pid 活着」三者与
    - 新增 TROUBLESHOOTING **第 28 条**;证据 `lab/docs/patrol_led_evidence.md`、`lab/agent/verify_state.sh`、`lab/drafts/test_agent_v2.sh`
    - **同日第二轮:LED 状态查询的采样假象(补进 TROUBLESHOOTING 第 23 条「第二处」)**。问「查看当前警告灯状态」,Agent 答「亮度 0(关闭)」+「正常呼吸闪烁」自相矛盾。实测 `trigger=[heartbeat]` 占空比 **22%**(40 次采样亮 9 次)→ 单点读到 0 概率 **78%**,灯在呼吸,读到的是灭相
      - 根因是第 23 条同一坑的**另一半**:上次只修了「用户态爆闪」(判据=查爆闪 pid 文件),**没修「内核 heartbeat 触发器」——它没有 pid 文件,判据直接落空**
      - 修法:`get_led_status` 按驱动方式分流 —— 周期模式(`heartbeat`/`alarm`/`alarm_blink`)返回 **`brightness_duty_pct`**(20×60ms=1.2s),静态模式(`on`/`off`)才回瞬时 `brightness`;prompt 禁止把瞬时值当开关、要求问「警告灯」时同查 `get_patrol_status`;另禁「超过告警阈值 = 温度超出正常范围」的混淆说法
      - 实测:heartbeat →「亮度占用率约 25%,正常呼吸状态」✅;20°C 阈值爆闪 →「已触发温度告警,LED 进入报警闪烁」✅
      - **方法论**:修一个坑时要问「**这个判据依赖的前提,在其它分支上还成立吗**」;判据绑某种实现(如"有 pid 文件")换实现就静默失效;**优先纯观测量(占空比)而非溯源量(谁在驱动)**
    - **入口统一**:`rkllm_embodied_agent` 改为指向 `rkllm_embodied_agent_p0` 的**软链接**,只维护一份二进制(此前 `run_agent.sh` 调前者、交互会话跑后者,改完忘了同步,验证时跑了一次旧二进制)
  - ✅ **编译优化落地 + MPP 硬编评估（一正一负，2026-10-25）**：
    - **发现大坑**：项目跑了一个月，`build_native/CMakeCache.txt` 里 `CMAKE_BUILD_TYPE` **是空的**，`flags.make` 里 `CXX_FLAGS = -std=gnu++17` —— **一个 `-O` 都没有**。此前所有 CPU 侧数字（MJPG 软解 7.76ms / letterbox / DFL 解码 3.09ms / CSC 51~65ms）全是 -O0 跑出来的悲观值。CMake 在 build type 为空时**不报错不警告**，功能测试永远发现不了（TROUBLESHOOTING 第 24 条）
      - CMakeLists 改动：默认 `Release` + `-O3 -DNDEBUG` 兜底 + `-mcpu=cortex-a76.cortex-a55` 自动探测（失败降级 `-mtune`）+ `option(BUILD_BENCH OFF)` 统一 bench 口径 + `if(EXISTS src/mpp_jpeg_encoder.cpp)` 集成点 + **构建摘要自检 `-O` 缺失时 `message(WARNING)`**
      - **A/B 实测**（`taskset -c 6` 绑大核，6 轮交替）：纯推理 `rknn_run` 14.79~14.88（O0）vs 14.90~14.95（O3）**无变化 = 对照组证明测量没漂**；**DFL 解码+NMS 3.44~3.51ms → 0.62~0.63ms = 5.5×**；**全链路 18.91~19.06 → 16.21~16.26ms = −14.6%，52.6→61.7 FPS**；二进制 206,256 → 75,688 B（−63%）
      - **方法论收获**：不绑核时 O0 的 **p50 仍是 3.45ms 但 mean 被拉到 6.4~8.2ms、p95 冲到 17ms** —— 调度迁移的长尾污染均值，能把 5.5× 压成 2×。性能测量必须 `taskset`
      - **生产二进制全链路补测**（同图 `real_000.jpg` 同模型，O0/O3 各 5 次，`taskset -c 6`）：MPP 硬解 1.250→1.322ms（+5.8%，**对照组①** 硬件层不该变 ✔）、**CSC 52.016→33.583ms = 1.549×**、letterbox 1.184→1.222ms（+3.2%，**对照组②** OpenCV 预编译库动不了 ✔）、NPU+DFL 22.480→19.708ms（−2.77ms，与 bench 吻合）、**全链路 76.928→55.836ms = −27.4%（1.378×），13.0→17.9 FPS**。两个「不该变」的对照组同时成立 → 加速确属编译优化而非测量漂移；**口径澄清：「白捡 1.55×」是 CSC 单层，全链路是 −27.4%**
      - 数据：`docs/BENCHMARKS.md` **2.8 节**（含 6 轮原始数据、CMake 变量名大小写坑、局限）
    - **MPP 硬编 JPEG：走通了但不更快（负结论，已从路线图划掉）**：`deploy/cpp/{include/mpp_jpeg_encoder.h, src/mpp_jpeg_encoder.cpp, bench/bench_jpeg_encoder.cpp}` 实现并验证正确（NV12 输入 → 标准 baseline JPEG，9 图 Y-PSNR **42.46dB** / 720p **49.27dB**），但 1280×720 mean **7.24 vs 6.65ms（1.07~1.09× 更慢）**、640×640 1.13~1.20×、**分辨率 64×64~960×540 全部档位都更慢**、4 路并行软编快 **1.88×**
      - **唯一优势**：CPU 占用 **8.2% vs 99.7%**（每帧 0.59ms vs 6.64ms），但单路 30fps 场景 CPU 不是瓶颈 → 不划算
      - **根因（延迟模型）**：硬编 = **0.35ms 固定开销 + 7.0e-6×像素**，软编 = 0 + 7.5e-6×像素 → 每像素算力打平，硬编白付 ioctl+中断往返；小分辨率固定开销占比更高
      - **"真走硬件"的实证**：`fdba0000.jpege-core` 四核中断 +3553/+703/+387/+167，其余 VPU 核零变化（起初误判为软回退，是这个证据推翻的）
      - 踩坑 6 条（最坑：FIXQP 下 `fps_in_denom=0` → `rc_frm_check_drop` 断言直接 Segfault；`cv::cvtColor` 出 limited-range 而 JPEG 要 full-range，PSNR 35.4→46.2dB；`prep:range` 键存在但被忽略=假成功）
      - 数据：`docs/BENCHMARKS.md` **2.9 节**；证据 `deploy/cpp/bench/mpp_jpeg_encoder_evidence.md`；新增 TROUBLESHOOTING **第 24 条**（构建无 -O）与**第 25 条**（硬件加速未必更快）
      - **教训：负结论也是结论**——用数据把路线图上的假设否掉，比硬凑一个"快了 5%"的数字有价值。CPU 侧优化转向 **RGA CSC**
  - ✅ **全仓库「真值清扫」+ 8 天工作首次入库（2026-10-25 晚）**：用户原话「做有必要做的，没必要就不做了，我只是找个实习」→ 判断标准从"性能再榨一点"切到**真实性优先**，RGA CSC 决定不做（纯性能优化、本板 RGA 2.1.0 还不支持 full CSC）。
    - **扫出并修掉 6 份文档的「目标值当实测值」**（都是板子到货前的设计估算被写成了既成事实）：
      | 位置 | 原来写的 | 真实 |
      |---|---|---|
      | `README.md`（GitHub 首页，面试官第一眼） | 单帧 <6.0ms / 端到端 <18ms / 三核 150+ FPS / 功耗 9.5~11.5W / 余弦 0.99651 | 15.95ms / 20.58ms / 59.3 FPS(2.21×) / **未测** / 该指标已失效 |
      | `docs/archive/ONBOARD_QUICKSTART.md:96-104` | 同上那套「预期实测硬指标」表 | 整表换成真机数字 |
      | `docs/tech_notes/BEV_GATEWAY_ARCHITECTURE.md` | H.264/265 硬解、RTSP、RGA CSC、DMA-BUF 零拷贝、MPP 推流<80ms、ByteTrack、近乎线性加速 | 只 MJPEG / 无 RTSP / **CPU CSC** / 无 DMA-BUF / 硬编已否掉 / 只 Python 仿真 / 2.21× |
      | `docs/BENCHMARKS.md:19` | 余弦相似度 0.99651 状态 **PASS** | cls 分支已塌缩成常数 0.5，**假安全** |
      | `docs/interview/` 两份 | 把 0.99651 当"量化代价"成绩 | 真 mAP 0.6087→0.6002（−1.4%） |
      | `demo/index.html` + 演示看板 + `demo/README.md` | 32.8 FPS / 18.2ms / 6 辆跟踪 | **PC 侧仿真数字**，页面顶部加黄色声明条 |
      | `gateway/zero_copy_pipeline.py` | 文件头称实现 MPP→RGA→RKNN 零拷贝 | `fd = 1000 + buf_id` 是**模拟 fd** |
    - **处理原则（重要，以后照做）**：①假数据不删了事，**README 新增「明确未实现（不要对外宣传）」7 行表**——主动写"没做"比被问穿强；②余弦相似度那条**保留下来改造成反面教材**（「我一开始拿它当精度指标，0.9965 就 PASS 了；后来发现特征图整体相似不影响单个分支塌缩，真精度得跑 mAP」）——**自打脸的过程比漂亮数字更值钱**；③`docs/README.md` + `BEV_GATEWAY_ARCHITECTURE.md` 文首加"这是早期设计稿"声明 + 逐项对照表，不销毁设计思路。
    - **git 首次系统化入库**：此前仓库最后 commit 停在 10-18，**10-21 之后 8 天的活（DFL 图手术/量化修复/MPP 硬解/RKLLM Agent/巡检守护/回归体系/编译优化）一个都没提交**。补了 5 个 commit：`8f6b648`（88 文件 +16139/−327 主提交）、`de1dc2f`… 等，见 `git log`。
    - **purge 掉公开仓库里的机密**：仓库是 **public**，历史 commit 的 `AGENTS.md`/`CURRENT_STATUS.md`/`LEARNING_PLAN_4WEEKS.md` 里写着**板子 root 密码**与内网 IP → 两次 `git filter-branch` 改写未推送历史 + 清理 reflog 与悬空对象；终验 `git grep <板子密码> $(git rev-list main)` = CLEAN、IP CLEAN、连 `REDACTED` 占位符都清干净。已推送的 `origin/main` 经查本就不含，实际暴露面为零。
    - **代码改用环境变量传板 IP**：`lab/{agent_multistep_demo,agent_regression_test,bench_contention_sampler}.py` 的 `--host` 默认值改为 `os.environ.get("EDGEVISION_BOARD_HOST","")` + 缺省时 `exit 2` 并提示；`lab/tests/run_agent_test.py` 的 SSH key 也改 `EDGEVISION_SSH_KEY`。**换台机器 clone 下来不会连错板子，也不会把内网 IP 泄到公开仓库**。
    - **踩坑（可复用）**：`Out-File -Encoding UTF8` 会给 commit message 加 **BOM**，导致 `git log` 标题变乱码 → 必须用 `[System.Text.UTF8Encoding]::new($false)` 写文件再 `git commit -F`；`core.filemode=true` + `core.autocrlf=true` 会让 15 个文件显示"已修改"而 `diff --numstat` 全是 `0 0`（纯 mode 变化）→ `git config core.filemode false` 立刻清爽。
    - ⚠️ **剩一步没做**：`git push` 卡在凭据（本地两个 SSH key 都不被 GitHub 认可、无 gh CLI/PAT）→ 需用户自己加 deploy key 或 PAT 后 `git push origin main`（当前 ahead 10）。
  - ✅ **面试冲刺材料补齐（2026-10-25 晚，用户指令「停下来准备面试」→ 项目开发线收尾，转入面试准备）**
    - **判断**：用户原话「做有必要做的，没必要就不做了，我只是找个实习」→ 从此**不再开新功能**，所有精力转到「把已经做出来的东西讲清楚」。
    - **发现 3 处致命过期材料**（照旧背会在面试里主动自贬）：
      1. `docs/interview/README.md` 的「诚实边界」表 **4 条全是 8 天前的状态**（说「喂灰图 0 检出」「只按置信度过滤无 NMS」「没接 MPP」「只测推理边界」）——这些**早就做完了**
      2. `interview-recap/` **完全没有 W5（端侧推理部署）/ W6（端侧 Agent）**——恰好是 JD 明写「RK3588 + RKNN/RKLLM 优先」的核心两块，也是整个项目最值钱的部分
      3. `docs/resumes/archive/RESUME_校招_端侧AI部署.md` 还写着 **RKNN Runtime 1.5.2**（实际 2.3.0）、「DFL 头剥离**正在实施**」（实际已完成 −57%）、「**NPU 推理环节尚为桩实现**」（实际早已闭环）
    - **新增 3 份材料**：
      - `docs/interview/W5-端侧推理部署.md`（349 行）：RKNN 量化（校准集生死线 / 余弦陷阱 / 真 mAP）/ DFL 图手术（砍什么、等价证明 1.22e-4px、16 bin 格子单位乘 stride）/ 三核调度（mask −3.3% vs 3 context 2.21×、各核 49% 负载、并行效率约 73.5%）/ 逐层剖析 / **测量方法论（p50-p95-p99、taskset 绑核、对照组设计）** / 编译优化 / MPP 一正一负 / 边界 6 项
      - `docs/interview/W6-端侧Agent.md`（328 行）：RKLLM 部署 / Tool Calling（必须数组、XML 格式、ReAct、max_steps=5）/ 5 个硬件工具（**注：`get_system_health` 实读 zone0/1/2，7 温区全量遍历在 8088 RPC 服务里**）/ 快慢双系统 / **三个真缺陷（爆闪被读成熄灭、缺常识、--no-led 白跑 4 轮）** / 回归 12 用例 / 边界
      - `docs/interview/INTERVIEW_PREP_PLAN.md`：**7 天排期 + 两分钟项目自述逐字稿（约 110 秒）+ 五条杀手故事（各 90 秒）+ 红线表（9 条绝对不能说出口）+ 被问倒了的三步走 + 反问环节 + 投递前检查清单 + 心理准备**
    - **修正的材料**：`interview-recap/README.md`「诚实边界」4 条→**11 行真实状态表**（✅已实现 4 项 / ❌未实现 7 项）、必背数字 13 行→**A~E 五组 22 行**；`W4-服务化.md` 标「未实操」→**已全部实操**（补 4 项交付物 + 实测踩的坑：fd 继承卡死 270s / fork 钉 NPU 196MB / 唯一没做的是硬件看门狗）；`高频八股速查.md` W4 标注同步修正；`RESUME_INTERN_2026.md` + `RESUME_校招_端侧AI部署.md` 数字口径全部统一到最新实测
    - **五条杀手故事（面试最值钱的部分，已写进 INTERVIEW_PREP_PLAN 第三节）**：①官方 5.5ms vs 实测 37ms，同算子位置不同差 6 倍，归因不靠猜 ②余弦 0.9965 骗了我，cls 精确等于 0.5=sigmoid(0) ③**演示全绿但 Agent 在说谎 → 分层定位「模型幻觉 vs 感知层喂错数据」** ④模型不缺智商缺常识（49°C 判永久损坏）⑤**MPP 硬编走通了但我把它从路线图划掉**
    - **红线表**（绝对不能说出口）：端到端 18ms / 三核 150+ FPS / 功耗 9.5W / DMA-BUF 零拷贝 / RGA 硬件 CSC / RTSP-H.264 硬解 / 4 路 1080P / ByteTrack 进 C++ / 余弦 0.9965 当精度
    - **commit**：`c54f8d6 docs: 面试冲刺材料补齐(W5/W6 + 冲刺计划 + 诚实边界修正)`（6 文件）→ `576859f` CURRENT_STATUS 转面试状态 → `caf91f6` 统一跨文档口径(9 条矛盾) → `7350550` 补齐演示指标区间口径 → 仓库 **ahead 10**
  - ✅ **实习简历定稿双版本 v12 生成（2026-10-26，DOCX/PDF 双格式 Exactly 1 Page）**：
    - **端侧部署版**：`docs/陈创荣_端侧AI部署_研一实习简历_v12.docx` 与 `.pdf`（生成器 `docs/_build_resume_v12.py`），主打 DFL-Free 算子图手术、COCO128 INT8 真 mAP、MPP 硬解与 Qwen3.5-2B 单板 Tool Calling 具身 Agent。
    - **Linux应用开发版**：`docs/陈创荣_Linux应用开发_研一实习简历_v12.docx` 与 `.pdf`（生成器 `docs/_build_resume_linux_v12.py`），技能重排突出 C++/并发/Socket网络/系统服务/外设驱动/构建优化；项目语言转换为系统工程（有界队列0丢帧、定长帧二进制推流、背压防积压、CPU Cache 命中优化、CMake -O3 重构、驱动版本倒挂排错）；AI 压至异构计算壁垒加分项。
    - 严格单页 A4（Word COM 自动化核验两版均为 `Total Pages: 1`）。
  - ✅ **`rkllm_init failed` 根因定位+修复工具（2026-10-26 晚，用户报错驱动的排障）**：
    - **现象**：`/root/rkllm_deploy/rkllm_embodied_agent` 加载 Qwen3.5-2B 报 `failed to allocate handle, ret: -1, errno: 14, errstr: Bad address` / `failed to malloc npu memory, size: 205799424, flags: 0x2` / `rkllm_init failed: -1`，但同一二进制两天前能正常跑。
    - **根因**：**RK3588 NPU 的 IOMMU 只有 32 位（4 GiB = 4096 MiB）地址空间**（与物理内存 7.5GB 无关）。每个 rkllm 实例约占 **~1.95 GB IOVA**（RSS 2273756~2274104 kB）。驱动失败前打印 `used: 3873~4085 MiB`，加请求 size 恒 > 4096 MiB → `dma_map_sg` 返回 EFAULT(errno 14)。
    - **三配置定量实验**（`_tools/quantify_iommu_by_config.sh`）：完全干净 → **2 个 OK / 第 3 个 FAIL**（失败请求 41608320B，used 4075 MiB）；只开 gateway → **1 个 OK / 第 2 个 FAIL**；gateway+dashboard → **1 个 OK / 第 2 个 FAIL**，**逐字复现用户原始日志的 `205799424 / flags: 0x2 / errno: 14`**。
    - **证伪的四个假设**：❌ 驱动 0.9.2 太旧（软警告，且 Qwen3.5 需 runtime ≥1.3.0 不能降级）❌ CMA 不足（`CmaFree=0` 时也能成功；`flags 0x2`=CACHEABLE 走 dma-buf+IOMMU 不占 CMA）❌ dashboard 单独泄漏（180s 内反复加载全成功，RSS 稳 171MB，renderD129 fd 恒 0）❌ gateway 冲突（单跑 8s/68s 均成功，RSS 131MB）❌ kill -9 泄漏 IOVA（强杀后加载成功）。
    - **交付两个工具**（本地 `_tools/` ↔ 板上 `/root/`）：`npu_doctor.sh`（六节体检：驱动/频率/负载 → 谁在占 NPU → 残留 rkllm 实例 → systemd 自启 → 历史失败次数+used 值 → 结论与四步处理法）；`run_agent.sh`（清残留 → 停网关/看板 → 启动 Agent → `trap` 退出自动恢复 `edgevision.service`），**实测单条指令模式跑通**（自然语言问温度 → 自主调 `get_system_health` → 汇报 SOC 37°C / NPU 37.92°C / 可用 4808MB / LED heartbeat）。
    - **文档**：TROUBLESHOOTING **第 27 条**（含完整排查链路 + 5 条方法论 + 面试讲法）、速查清单新增 7 行、`CURRENT_STATUS.md` 顶部新增「⚠️ 关键运行约束:NPU IOMMU 只有 4 GiB」章节。
    - **方法论收获**：①「昨天好好的今天坏了」先查「机器上还有什么在跑」，不是先 `git diff` ②日志里相邻的 warning 和 error 不一定有因果 ③`size` 远小于 `MemAvailable` 却失败 → 限制不在物理内存那一层。
- 🚧 **第四阶段新项目开工(2026-10-24):stereo3d 双目 3D 感知 / 人脸 3D 定位**(代码已归一到 `edgevision-rk3588/stereo3d/`)
  - **项目定位**:补上单目检测只有 2D 框的短板 → 双 USB 摄像头三角测量,输出目标 **3D 坐标 (X,Y,Z) 米**。核心公式 `Z = fx·B/d`,误差传播 `ΔZ = Z²/(fx·B)·Δd`(3m 处 1px 视差 ≈ 27cm,双目只在 3m 内靠谱——面试镇场结论)。
  - **架构**:原生 C++(只用 OpenCV 4.5.1,无第三方库),`include/` 四个头文件是硬契约,4 个智能体并行开发(stereo_pipeline / face_3d / http_server / calib 工具链),板上原生编译零 error 零 warning。
  - ✅ **真实双摄全链路跑通(2026-10-24)**:`/root/stereo3d/build/stereo3d_demo`,双摄 1280×720 → 立体矫正 → SGBM 视差 → 深度 → Haar 人脸框内深度中位数 → 3D 反投影 → 网页 2×2 实时预览(端口 8081)。实测分段:cap 16.3 / rect 5.4 / **sgbm 46.1(瓶颈)** / depth 0.6 / face 20.6 / enc 7.2 ms,全链路 **9.7~11 FPS**(222 帧)。视觉已用本地 qwen2.5vl 模型确认 4 宫格画面真实。
  - ✅ **数学链正确性(合成真值自检)**:1/2/3m 层深中值误差 **3.94 / 21.99 / 6.14 mm**;21 项独立真值自测全 PASS(SGBM 视差 16.00px vs 真值、深度 1714mm 与理论逐位一致、P2 反算基线 0.0600m 自洽)。
  - ✅ **「为什么用中位数不用均值」量化证据**:框内深度注入 20% 噪声(10% 无效+10% 离群 600mm),均值偏 **−157mm(−7.9%)**,中位数误差 **0**。
  - ⚠️ **诚实状态**:当前**未标定**(理想平行双目假设 fx=1108/B=60mm),绝对距离不可信;标定工具链 `tools/calib/` 已交付并自测通过(合成真值:单目 RMS 0.043px / 双目 RMS 0.044px / 基线误差 0.003mm / 极线 y 差 1.023→0.029px 达标)。
  - ✅ **SGBM 提速消融完成(task-5,2026-10-24)**:先诊断推翻"单线程"假说(OpenCV 4.5.1 带 TBB,parallel_for_ 探针 4.10× 加速,46ms 已是并行态真实成本);再用 getenv 环境变量入口(STEREO_NUM_DISP/STEREO_BLOCK,不改 include/main)做 6 组×200 帧消融:**nd=64→32 只快 15.6%**(42.83→36.17ms,理论 O(W·H·N·D) 应省一半——DP 聚合访存与 D 无关,面试可讲"理论 vs 实测偏差");nd=32 有效视差占比反而最高(0.211 vs 0.178),代价最近可测距离 0.52→1.04m;block 5→7 无收益。**推荐 nd=32+block=5**;≤20ms 需降分辨率 480×270(后备手段暂不启用,Haar 在 270p 会变弱)。数据:`stereo3d/docs/BENCHMARKS.md` 第 5 节。
  - **踩坑已修**:① rectify 返回**单通道灰度**,main 直接拿去 hconcat 拼 BGR 视差图 → 类型断言崩溃,先 cvtColor 再拼;② OpenCV 4.5.1 `initUndistortRectifyMap(m1type=CV_16SC2)` 固定产出 **CV_16UC1** 的 map2(不存在 16SC2+32FC1 组合),6 字节/像素比 float 方案省 25% 访存;③ 摄像头被 `edgevision.service` 占用 → 测试前 `systemctl stop`,**测完必须 start 恢复**。
- ✅ **工作区系统性规范化 + 使用说明落地(2026-10-27)**
  - **根目录从 14 个散乱条目收敛到 8 个**:`README.md`(新) / `AGENTS.md` / `edgevision-rk3588/` / `rkllm_deploy/` / `rk3588开发板配套资料/` / `_tools/` / `_workbench/` / `.workbuddy/`。删掉 2 个空目录、`_probe_write/`、两份沙箱 ACL 残留(`_acl-recovery/`、`_acl_recovery/`)。
  - **新增根 `README.md`(9 节,使用说明正文)**:这是什么 / 目录地图(逐条「该不该动」) / 五分钟上手三条路径(只读书 40min、连板子、板端编译) / 端口表(8080·8081·8088·8090·9000) / **四条硬约束**(IOMMU 4GiB、只在板上编译、别内联 ssh、别用 PowerShell 写中文 UTF-8) / 文档地图 / 维护三条红线 + 命名约定 / `_workbench`·`_tools` 说明 / 当前状态与「明确未实现」清单。
  - **新增三份子目录 README**:`_workbench/README.md`(6 个子目录的来源·内容·能否删 + 4 条约定)、`_tools/README.md`(39 个脚本分 4 类:现役 5 / IOMMU 战役 20 / 性能测量 5 / D20 学习期 9)、`rkllm_deploy/README.md`(5 文件逐个说明 + 板上编译命令 + IOMMU 前置条件 + 为何不入库)。
  - **`_workbench/` 合流**:原 `_tmp/`(265 探针) + `_tmp_cmake_opt/`(62) → `probes/`;原根 `_test_results/`(54 次回归) → `test_results/`;根散落 `start_preview_real.sh` / `_p6_build.sh` / `_p6_t04.sh` → `board_scripts/`;两份会话转录(325KB+113KB,含手机号) + console log → `session_logs/`;`_tools` 清出的 20 项残留 + ACL 报告 + 会话备份 → `tools_residue/`;原根 `stereo3d-rk3588/` → `stereo3d-rk3588-archive/`。合计 6 子目录 570 文件约 60MB,**不在任何 git 仓库内**。
  - **引用修复(机器验证)**:`_test_results` → `_workbench/test_results` 共 17 处 6 文件;两个 Agent 脚本的默认输出目录改为 `<工作区>/_workbench/test_results`(3 层 dirname 上溯);`stereo3d-rk3588/docs/...` → `stereo3d/docs/...`。终验:**全工作区 md 相对链接 36 条 0 broken**、旧路径 grep 0 命中。
  - **`.gitignore` 个人材料护栏同步新路径**:`docs/resumes/` 改为**整目录屏蔽**(v3~v12 全部简历 docx/pdf/png + 生成器 + 两份他人简历 + Word 锁文件 + ACL 残留,任一被提交都会泄露手机号与 163 邮箱),另加 `docs/interview_book/RESUME_TEXT.md`、`.dsh-office-*/`、`_verify2/`。验收:三个敏感路径 `git check-ignore` 全部 IGNORED,`git add -A --dry-run` 敏感命中 0。
  - **补回搬迁丢失的两份文档**:`docs/archive/PATENT_DISCLOSURE_EQUIPDES.md`(5475B) 与 `docs/tech_notes/VOICE_ASR_PLAN.md`(24977B),均与 HEAD blob **逐字节等价**。
  - **双目归一**:代码权威副本 = `edgevision-rk3588/stereo3d/`,补入 `evidence/`(合成真值 161KB + 标定自测 log + 标定 yml) 与 6 张证据图 + `deploy/ab_test.sh`;独立目录降级进 `_workbench/stereo3d-rk3588-archive/`,只留 `corners_check/`(19MB) 与 `_selftest/pairs/`(20.9MB) 等重资产。
  - **6 个 commit 入库**(b56a324 / 2d87f1d / 53d087f / 3b94cc6 / d030873 / b3660e7),工作树干净;仓库 **ahead 7**(仍缺 GitHub 凭据,待用户 push)。
  - 方法沉淀:一次性搬迁用脚本 + 机器验收(链接检查、旧路径 grep、`git check-ignore`、逐字节 diff),比人工核对可靠;**迁目录后必须同时改引用与 .gitignore**,否则会出现「文档 404」与「个人简历被提交」两类隐性事故。
- ✅ **学习线 M1 IPC 实测(2026-10-29)**:8 机制 × 3 载荷,`lab/ipc/`。`shm_spin 0.583us` vs `shm_sem 8.17us`(**结构相同,差值纯是系统调用成本**);720p 整帧下**所有机制塌到同一水平**(≈6.5GB/s)→ **换机制省不了大帧带宽,只能减少拷贝**。坑:对照组设计错误 → **数据自相矛盾就说明设计错了**。
- ✅ **学习线 M2 语音链路实测(2026-10-29)**:ASR sherpa-onnx **static**(shared 需 GLIBCXX 3.4.29,板上只 3.4.28)+ Paraformer int8,RTF **0.023(快约 40×)**;TTS **4 线程最优 0.564**;常驻 `tts_daemon.cpp` 摊掉 **3.70s 加载**(**5.5s→1.8s**)。坑:ES8388 **不支持 16k、只收双声道、plughw 失效**。详见 `BENCHMARKS.md` **2.11**。
- 🚧 **NPUD 推理服务化 Phase 0 完成(2026-10-29)**:计划 `docs/tech_notes/NPUD_SERVICE_PLAN.md`。**推翻原立项前提**——`run_agent.sh` 称「gateway 常驻时起 rkllm 必失败」,三轮复现证明**三者共存正常**,失败只在**第 2 个 rkllm**。**方法论**:脚本注释是故障后的推断不是事实,**过度归因**是常见事故形态。**下一步 Phase 1**。

## 4. 目标与下一步(用户想干的事)
1. **校招项目(核心)** —— 锁定方向 **【端侧 AI 部署 / 边缘计算工程师】（第一主力冲刺）+ 【嵌入式 Linux / 机器人系统】（高确定性保底）**。核心突破点：NPU 3核异步调度 + INT8量化掉点控制 + RGA/DMA-BUF 零拷贝 + 真实吞吐/延迟指标。
2. **桌面信息屏** —— 用板子 HDMI 做;屏幕**还没买**(计划买 **微雪 7 寸 HDMI 触摸屏 1024x600,约130~180元**);**软件可以先做**(页面 + kiosk 全屏 + 开机自启)。
3. **12 周工程与学习路线（4 阶段融合版）** —— 见 `edgevision-rk3588/docs/LEARNING_PLAN_4WEEKS.md`：
   - **第一阶段（第 1～4 周）**：Linux C/C++ 系统编程与边缘服务底座（★ **100% 满贯收官！已交付无人值守 EdgeVision V1.0 边缘推理网关**）
   - **第二阶段（第 5～7 周）**：RK3588 端侧推理深度剖析与模型极致优化（**下一步主力冲刺！① 真实画面 OpenCV 画框显示 + YOLOv8 C++ 真实 NMS 后处理；② 改模型导出砍 DFL 头，36ms 打入 20ms 以内；③ RGA/DMA-BUF 显存零拷贝；④ FP16 vs INT8 量化精度与延迟对照实验**）
   - **第三阶段（第 8～10 周）**：Edge AI Agent 应用开发（Tool Calling 联动板端 C++ 服务、查温区/控外设/触发视觉检测、多步工作流与权限安全）
   - **第四阶段（第 11～12 周）**：系统混沌可靠性测试（GDB/strace/Valgrind/看门狗自愈）与完整工程交付
4. **板载与外接外设**:LED✅、音频/双麦克风✅、TF卡✅、双 USB 摄像头(UVC 双目硬件)✅;还没测:看门狗、ADC按键、GPIO。

## 5. 关键路径 / 命令
```bash
# 电脑侧
ssh root@<板子IP>                 # 已配免密(密钥 ~/.ssh/id_ed25519_rk3588),别名 ssh rk3588;备用密码 <板子密码>
# 板子侧
cd /root/edgevision && ./deploy/cpp/build_native/edgevision_pro_rknn   # 跑 NPU
cd /root/lab                          # C++ 练习
bash /root/check_board.sh             # 板卡自检
cat /sys/class/thermal/thermal_zone0/temp   # 温度
cat /sys/kernel/debug/rknpu/load      # NPU 三核负载
```
- 代码(电脑):`D:\a研究生\项目\rk3588项目\edgevision-rk3588`
- 板端源码:`/root/edgevision/deploy/cpp`(板端原生编译:`cmake .. -DCROSS_COMPILE_RK3588=ON && make`)
- 模型:`edgevision-rk3588/data/models/yolov8n_rk3588_i8.rknn`
- 配套资料:`D:\a研究生\项目\rk3588项目\rk3588开发板配套资料\EVM3588-B开发套件`

## 6. 相关文档
- `edgevision-rk3588/docs/LEARNING_PLAN_4WEEKS.md` —— 4周「边学边练」计划
- `edgevision-rk3588/docs/TROUBLESHOOTING.md` —— 踩坑记录(遇到新问题**必须**往这里追加,规则见开头「工作约定」)
- `edgevision-rk3588/docs/archive/CAREER_DIRECTIONS.md` —— 校招方向清单
- `edgevision-rk3588/docs/CURRENT_STATUS.md` —— 状态快照
- `edgevision-rk3588/README.md`、`docs/archive/ONBOARD_QUICKSTART.md`、`docs/archive/STUDY_GUIDE_PRO.md`

## 7. 协作偏好(重要)
- **说中文**;回答**简洁、直接、可操作**。
- 用户是**学生、嵌入式新手**:解释给**大白话+类比**,别甩术语。
- 用户喜欢**一步步来**,每一步做完**确认再下一步**。
- **学习场景**:让用户**自己动手/手敲**,AI 当「私教 + 陪 debug」——**不要直接替他把代码全写好**;多提问、多让他排错。
- 涉及**买硬件**:给**明确型号 + 价格 + 认准的接口**,别买错(MIPI 排线屏翻车)。
- 目标导向:**校招项目要「真实、可量化、能讲清」**,警惕过度吹嘘的数字。
