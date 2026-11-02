# 当前状态快照(CURRENT_STATUS)

> 完整背景看项目根目录的 **`AGENTS.md`**(新对话会自动加载)。

## 一句话
RK3588 EVM3588-B 边缘AI校招项目 + Linux 应用层学习。

> 最后一次更新:**2026-10-30 晚 · 语音链路收尾**(软唤醒上线 / Agent 首轮预热 **28s→1s** / TTS 常驻实测 **5s→1s**)——详见下方 M2 小节。同日更早一次是语音链路实测结果归位(M2 小节 + `BENCHMARKS.md` 2.11 节);此前是 2026-10-28 的 Agent「报警阈值可发现性」缺陷修复;再往前是 2026-10-26 的 `rkllm_init failed`(NPU IOMMU 4 GiB 地址空间耗尽),见下方「关键运行约束」

## ⚠️ 关键运行约束:NPU IOMMU 只有 4 GiB(和物理内存是两回事)

RK3588 的 NPU 走 IOMMU,`rknpu` 给每次 `rknn_create_mem` 在 **32 位 IOVA 空间(4 GiB = 4096 MiB)** 里要一段连续地址。这个上限**与 MemTotal 7.5 GB 无关**,是硬约束。

**实测容量(2026-10-26,`_tools/quantify_iommu_by_config.sh`)**:

| 配置 | 最多能起几个 rkllm(Qwen3.5-2B,W8A8 2.98GB) |
|---|---|
| 完全干净 | **2 个**,第 3 个失败 |
| 只开 gateway(systemd `edgevision.service`) | **1 个**,第 2 个失败 |
| gateway + dashboard(8090) | **1 个**,第 2 个失败 |

一个 rkllm 实例常驻 RSS ≈ 2.17 GB、约占 **~1.95 GB IOVA**;2 × 1.95 ≈ 3.9 GiB 已逼近上限。

**超限报错长这样(完整四行)**——注意 `size` 只有 196 MB,远小于可用内存:
```
W rkllm: Warning: Your rknpu driver version is too low, please upgrade to 0.9.7   ← 软警告,与失败无因果
E RKNN: failed to allocate handle, ret: -1, errno: 14, errstr: Bad address
E RKNN: failed to malloc npu memory, size: 205799424, flags: 0x2
E rkllm: rkllm_init failed
```

**判据(一眼定性,别去升驱动)**:`dmesg | grep 'dma map'` 看 `used: N MiB`,`used + 请求 size > 4096` 即地址空间耗尽。errno 分诊:**14 = EFAULT 地址映射失败**、12 = 真 OOM、23/24 = fd 耗尽。

**正确跑法**——用包装脚本(自动让出 NPU,退出后自动恢复网关):
```bash
bash /root/run_agent.sh                    # 交互式
bash /root/run_agent.sh "查一下开发板温度"   # 单条指令
bash /root/run_agent.sh --no-restore       # 用完不自动恢复网关
```
脚本做的事:① 清残留 rkllm 实例 ② `systemctl stop edgevision.service` ③ 停 dashboard ④ 启动 Agent ⑤ 退出时 `trap` 自动 `systemctl start edgevision.service` 恢复现场。

**出问题时体检**:`bash /root/npu_doctor.sh`(六节:基础状态 / 谁在占 NPU / 残留 rkllm 实例 / systemd 自启 / 历史失败记录 / 结论与处理顺序)。

**最后手段**:`reboot`(冷重启后 IOMMU 地址空间彻底复位;`kill -9` 实测也能正常释放,不必重启)。

## 现在能干什么
- SSH 板子:`ssh root@<board-ip>`(**已配免密**;别名 `ssh rk3588`;有线。IP 与密码见本地私密记录,不入库)
- **工业级全链路服务**:`systemctl status edgevision`(开机自启、5秒自愈、3核 NPU、真实摄像头、TCP 9000 推流)
- **网页实时预览(带真实检测框)**:浏览器开 **`http://<board-ip>:8080`**
  - 启动:`cd /root/lab && ./w4_edgevision_preview_real /dev/video81 0.25`
  - 注意它和 9000 网关**抢同一个 `/dev/video81`**,同时跑必须错开
- **车载道路实景 3D 看板(2026-10-26 新增,8090 端口)**:浏览器开 **`http://<board-ip>:8090`**
  - 启动:`cd /root/edgevision && ./deploy/cpp/build_release/edgevision_dashboard --port 8090`
  - **读 mp4 循环(默认 `data/videos/highway_traffic.mp4`,267 帧/圈自动重头),不抢摄像头,可与 8080/9000 同时跑**
  - 每帧画 7~8 辆(box 4~11 个)真实车辆**彩色 3D 立方体线框** + 右上角**自车 BEV 雷达**(同心环 10~50m / FOV 锥 / 自车三角 / 目标方块带测距标签) + 顶部 HUD
  - 实测 **24~27 fps**(720p MJPEG q80,144KB/帧,带宽 ≈3.7MB/s),端到端验收 6/6 全绿
  - 端点:`/`(网页) `/api/stats`(JSON 结构化指标 + dets) `/snapshot`(单帧 JPEG) `/stream`(MJPEG)
- **新模型(DFL-Free)测速**:`cd /root/lab && ./bench_dflfree /root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn 200`
- **mAP 评估**:`python convert/eval_map.py --model data/models/yolov8n_dflfree.onnx`(PC)/ `--preds int8_preds.txt`(读板端结果)
- 跑 NPU:`cd /root/edgevision && ./deploy/cpp/build_native/edgevision_pro_rknn`
- 板端测速(板上 `/root/lab/`):`./bench_npu <model> 200` / `./bench_multicore <model> 100`
- 硬件状态:
  - 双目 UVC 摄像头:Cam 1 `/dev/video81`(已远焦对焦锐利) + Cam 2 `/dev/video83`
  - 双麦克风:ALSA `hw:5,0` 与 `hw:6,0`(16kHz S16_LE 立体声实测秒录)
  - 自检:`bash /root/check_board.sh`
  - TF 卡已挂 `/mnt/tf`(开机自动挂载)

## 进度总览

### 学习线第一阶段(第 1~4 周):【全部大满贯通关！】
- **第 1 周(系统文件IO与进程治理)**:通关 (sysfs/ioctl/fork/exec/waitpid/Mini-ps/7温区日志)
- **第 2 周(多线程与硬件并发)**:通关 (pthread/mutex/条件变量/3核物理硬绑定抢单队列)
- **第 3 周(网络与边缘推流网关)**:通关 (Socket/二进制内存直推/解决粘包/非阻塞IO/D20全链路闭环)
- **第 4 周(生产服务化与高并发)**:通关
  - D22 守护进程:掌握 `daemon(0,0)` 摆脱控制终端与会话迁移
  - D25 自愈服务:掌握 `/etc/systemd/system/edgevision.service`，实现插电自启与 5 秒崩溃自愈
  - D23 信号机制:掌握 `sigaction` 捕获 `SIGINT/SIGTERM` 优雅释放硬件显存，屏蔽 `SIGPIPE` 拔网线不死
  - D24 高并发网络:掌握 `epoll` ET(边缘触发)单线程反应堆，搞透红黑树、就绪链表与非阻塞 EAGAIN

### 阶段一交付物:【EdgeVision 工业级全链路边缘网关 V1.0】
- 源码:`/root/lab/w4_edgevision_gateway.cpp`(本地 `w4_edgevision_gateway.cpp`)
- 上位机:`D:\a研究生\项目\rk3588项目\edgevision-rk3588\lab\pc_client_viewer.py`（**在 lab 子目录里,不在项目根**）
- 实测指标:
  - 视觉输入:真实 USB 摄像头 `/dev/video81`(720P -> 640x640 预处理)
  - 核心算力:RK3588 3个物理 NPU 核心真硬绑定并发推理(`yolov8n_rk3588_i8.rknn`)
  - 延迟与吞吐:单帧推理稳态 **35.8 ~ 36.4 ms**，上位机连续推流 **800+ 帧稳居 24.2 FPS**

### ✅ 学习线第五阶段 M1(IPC)已完成实测(2026-10-29)
- **补的是第一阶段漏掉的整块**:第 1~4 周做了 文件IO/进程/线程/Socket/epoll,
  **唯独没做进程间通信**。面试出现频率与 epoll 同级。
- **交付**:`lab/ipc/{ipc_bench.cpp, spin_cost.cpp, README.md, results_20261029.txt, spin_cost_results.txt}`
  (板端 `/root/lab/ipc/`),8 种机制 × 3 种载荷 × 延迟/吞吐/CPU 三维实测。
- **核心数字**(248B 定长帧 RTT,`taskset -c 6,7`,后台服务已停):
  `shm_spin 0.583us` < `unix_stream 5.83us` < `pipe/fifo 7.0us` < `shm_sem 8.17us` < `tcp 13.1us`
  —— **`shm_spin` 比 `shm_sem` 快 14×,两者数据结构完全相同,差值纯是系统调用成本**(最干净的对照实验)。
- **决定性对照**:吞吐 921600B(720p 灰度整帧)时**所有机制塌到同一水平**(7~8K 条/s ≈ 6.5GB/s),
  瓶颈变成内存带宽 → **换 IPC 机制省不了大帧的带宽,只能减少拷贝次数**。
- **等待策略实测**(`spin_cost.cpp`):缓冲填满时 `spin` 让生产者 **CPU 100%**(1680ms CPU / 1680ms 墙钟),
  而 `sem` 只要 **0.27ms**、`adaptive` **0.44ms**;但缓冲足够大时**四者毫无差别**(0.03~0.04ms)
  → 工程最优是先加深队列,其次才是选 adaptive(真实 futex 自适应自旋的动机,自己量出来了)。
- **踩坑 3 条**(详见 `lab/ipc/README.md` 第 3.2 节与下文):
  ① 首版 SPSC 用「单槽 + 二值 flag」在单向灌模式下丢消息 → 板上留下两个 99.6% CPU 的孤儿进程,
  把 load 顶到 3.56 导致 SSH 断连;修法 = 环形缓冲 + `PR_SET_PDEATHSIG` + 自旋超时。
  ② SysV 信号量带 `SEM_UNDO` 时,每进程补偿表上限 32767 条(`SEMVMX`),
  单向 20 万条必然溢出 → `semop` 返回 ERANGE 静默失败。
  ③ **对照组设计错误**:首版 `spin` 生产者看 `tail`(消费者一读就推进)、
  `sem` 却等处理完才 post,语义不一致 → 出现「自旋比 sem 慢 2.9 倍」的自相矛盾数据。
  修法 = 三计数器 `head`/`tclaim`/`tdone`。**数据自相矛盾就说明设计错了,别急着归因**。
- **局限**:板上 `loadavg` 长期 ~1.0 降不下来(`tx_5v_power_present` D 状态内核线程),
  绝对数值偏保守,但同轮内相对比较有效。

### ✅ 学习线第五阶段 M2(语音:ASR + TTS + 编排网关)已上板实测(2026-10-29)
- **ASR(识别)**:sherpa-onnx **v1.12.12 static**(shared 包需 `GLIBCXX_3.4.29`,板上只有 3.4.28 → 必须 static)
  + **Paraformer-zh-small int8(78 MB)**,2 线程。7 条官方 test_wavs 实测 **RTF 0.023~0.025(比实时快约 40×)**,
  含四川/天津/河南话与中英混说,识别文本全部可读。
- **VAD 断句**:`sherpa-onnx-vad-alsa-offline-asr` + `silero_vad.onnx`(threshold 0.5 / min_silence 0.5s / min_speech 0.25s),
  2 分钟真实环境音切出 **26 段**。
- **TTS(合成)**:`vits-zh-fanchen-C`(116 MB),线程消融 1/2/**4**/6 → **4 线程最优 RTF 0.564**
  (A76 只有 4 个大核,6 线程反而退到 0.679)。**闭环验证**:TTS 生成的 wav 再喂 ASR 能识别回原文,只差两个同音字。
- **常驻 TTS 服务**:`lab/voice/tts_daemon.cpp`(AF_UNIX,协议 `[u32 len][utf8]` → `[i32 sr][i32 n][i16 PCM]`),
  把每次 `fork/exec` 的 **3.70 s 固定加载开销**摊掉 —— 同一句 15 字 **5.5 s → 1.8 s**。
- **编排网关**:`lab/voice/voice_gateway.cpp`,麦克风`hw:0,0` → VAD+ASR 子进程 → Agent 子进程 → 分句 TTS → 重采样 48k → `aplay hw:2,0`,
  用 `pipe+dup2` 重定向、`SIGSTOP/SIGCONT` 做半双工(播报时冻 ASR 防自激)、`SIGINT/SIGTERM` 有序回收子进程。
  启停脚本 `lab/voice/voice_start.sh`。
- **踩坑(板载 ES8388 三个硬约束)**:①**不支持 16 kHz**(必失败,44100/48000 才行)②**只收双声道**(单声道报
  `Channels count non available`)③**`plughw` 插件失效** → 只能手动重采样+复制声道。
  隐藏陷阱:**`PCM Volume` 150(−21 dB)实测完全听不到**,192(0 dB)才正常 → 极易误判成「链路不通」。
- **10-30 晚新增**:①**软唤醒门控已上线**——放弃 KWS(它与 ASR 抢麦克风,切换一次要重载 ASR 模型 4 s,
  喊完唤醒词还得干等),改常驻 ASR 文本匹配:关键词「小瑞」+ 5 个同音字容错(瑞/锐/晓瑞/睿/蕊),唤醒后 10 秒激活态。
  ②**Agent 首轮 prefill 预热**:`rkllm_clear_kv_cache(handle, 1, ...)` 保留 system prompt 的 KV,
  首轮 **28 s → 1 s**(代价:启动慢 24 s,挪到用户无感阶段)。③重采样已在 C++ 内实现
  (`wav_convert_to_spk`,16 kHz 单声道 → 48 kHz 双声道),不再依赖 PC 侧。④实测三段播报首段合成 **5 s → 1 s**。
  ⑤**Agent 可操作系统服务**:新增 `control_service` 工具,语音即可启停 看板(:8090)/推流(:9000)/巡检守护/语音自身;
  8090 看板已 systemd 化(`edgevision-dashboard.service`)。自我关闭用「延迟 3 秒 + `setsid()` 脱离会话」,
  保证先把话播完再退出。实测**开看板会让 decode 从 7 掉到 3 tok/s**(YOLOv8 抢 NPU)。
  ⑥**根治 IOVA 泄漏**:给 agent 装 SIGTERM handler(`sa_flags=0`,不设 `SA_RESTART`),
  主循环自然退出后执行 `rkllm_destroy()`;此前每次 kill 都要重启板子,现已验证「启→停→启」连续两轮正常。
  ⑦**perf 打点**:callback 里分离输出 prefill / generate 吞吐,实测 **prefill 90~130 tok/s、decode 10 tok/s**。
- **10-31 新增**:①**意图路由（快速通道）上线**——6 类高频问题绕开 LLM，
  直接读 sysfs / systemctl 回答:温度、LED 状态、LED 控制、服务启停、告警查询、时间。
  实测 **11 s → < 0.1 s**（服务启停 3 s，因为要等 `systemctl` 确认状态）。
  设计原则是**只匹配明确无歧义的说法**，"设备怎么样"这类一律交回 LLM——宁可慢，不可答错。
  ②**端到端自测跑通（全程无人）**：板子用扬声器说「小瑞温度多少」→ 自己麦克风听到 →
  唤醒 + 剥离唤醒词 → 快速通道 → 回「板子温度 47.153 度，正常」。
  ③**全链路服务化**：`voice-gateway.service` + 新建 `tts-daemon.service`，均 enabled、开机自启，
  不再依赖 SSH 会话。耳机开关与音量写进 `ExecStartPre`（否则重启后耳机无声）。
  ④**三把测试工具**：`bench_voice.sh`（语音命中率，实测 10/10）、`bench_params.sh`（参数扫描）、
  `test_e2e.sh`（端到端自测）。
  ⑤**参数扫描结论（负面但有用）**：`-O2 / -O3` × `ctx 8192 / 4096` 四组，decode **恒 7 tok/s**——
  瓶颈在 NPU 推理与内存带宽，CPU 侧编译优化无效，以后不必再试。
  ⑥**仍未解决**：ASR 在**实时链路**里识别不稳（VAD 把长句切碎成「小瑞关注」），
  而同一段音频**离线识别 100% 正确**。需调 `min_silence`（0.5 → 0.3 s）或加 `speech_pad`。
- **仍未做(别宣称语音助手成品)**:①`voice-gateway.service` 已写且 `verify` 零告警、已 enable,
  但未切换到托管(会和手动实例抢麦克风);②无外放场景的半双工验证(当前用耳机,声音不外泄所以没暴露自激);
  ③无打断(barge-in),播报期间完全不听;④上下文无主动管理,顶到 16K 上限的行为未测。
  详见 `docs/tech_notes/VOICE_ASR_PLAN.md` **第 6 节** 与 `TROUBLESHOOTING.md` **第 29~36 条**(含全部真机输出与口径)。

---

## 第二阶段进展(第 5~7 周):端侧推理深度剖析与模型优化 —— 核心已完成

### ✅ 1. 真实画框与后处理(所见即所得)—— 完成，且做成了网页实时预览
- `lab/vision/edgevision_preview_real.cpp`:用新模型 + `dflfree::decode` 画**真实检测框**。
  原来 `w4_edgevision_preview.cpp` 里那两个 "Person 96%" / "Object 91%" 是**写死的假框**，且模型跑完 `want_float=0` 拿完就 release，检测结果从未被使用。
- 浏览器直接看:`http://<board-ip>:8080`（`/` 网页外壳、`/stream` MJPEG、`/snapshot` 单帧）
- 预处理改为 letterbox；HUD 实时显示 `NPU xx ms + Decode xx ms + FPS + obj 数`
- 离线验证工具:`deploy/cpp/bench/preview_detect.cpp`

### ✅ 2. 改模型导出砍 DFL 头 —— 完成（原目标"36ms → 20ms 以内"，实际 20.58ms）
- 单帧全链路 **37.19 ms → 20.58 ms（-44.7%），48.6 FPS**（原 26.9 FPS）
- 纯推理 **37.19 → 15.95 ms（-57%）**；模型 **6.22 → 4.34 MB**
- 自研 C++ 无 DFL 解码器 **1.79 ms**，与 numpy 参考逐位一致（max|Δ| = 6.1e-05 px）
- 顺带把转换环境搬到**板子上直跑**（rknn-toolkit2 2.x 有 aarch64 wheel），不依赖 PC / WSL
- 详细数据:`docs/BENCHMARKS.md` 2.3 节

### ✅ 3. 量化精度验证 —— 补上了真 mAP（不再是"9 张 top-1"）
- COCO128（128 张带标注 / 929 个 GT / 71 类）:**FP32 mAP@0.5 = 0.6087 → INT8 = 0.6002，掉点 0.85 个百分点（-1.4%）**
- 对照实验:合成色块校准集会让 cls 分支塌缩成常数 0.5（-38.5%），换实拍增强样本恢复正常
- 工具:`convert/eval_map.py`（PC）+ `deploy/cpp/bench/eval_dump.cpp`（板端导预测）
- 详细口径与局限:`docs/BENCHMARKS.md` 2.4 节

### ✅ 4. 把新模型接进 9000 网关（工业级闭环）—— 完成（2026-10-23）
- **真数据全面就位**：废除旧 84ch 模型与 `frame_id % 50` 伪造假框，全面接入 `RKNNEngineDflFree` 3核物理硬绑定 NPU 推理（稳态 20ms）。
- **几何还原**：预处理用标准 `letterbox` 等比缩放+灰边填充，逆变换将检测框还原回原始 1280×720 物理像素。
- **协议升级**：推流结构体升级为 248 字节定长帧（8 字节头 + 10×24 字节 BBox），上位机 Python 脚本同步对齐 COCO 80 类解析与坐标映射。
- **背压防积压**：结果队列施加容量丢旧控制，并在上位机重连瞬间清空陈旧历史帧，彻底消除断连重连瞬间 10800+ FPS 历史积压爆发与延迟雪崩。
- **实测指标**：板载 `edgevision.service` 后台平稳运行，PC 上位机稳定持续接收 **19.2 FPS** 真实检测流。

---

## 接下来要推进的重点任务

0. ✅ **摄像头 20fps 之谜已破（2026-10-24）** —— 见 `docs/tech_notes/CAMERA_FPS_ROOT_CAUSE.md`
   - **真因：代码从没设 `cap.set(CAP_PROP_FPS, 30)`**，V4L2 默认协商到 20fps
   - 改一行后两台摄像头实测 **33.33 ms/帧 = 30.0 fps，400 帧零抖动**（p95 33.50ms）
   - ⚠️ 这**推翻了 BENCHMARKS 2.5.8** 的「物理上限 20fps」结论（已在 2.5.9 标注修正）
   - 遗留：`cap.get(CAP_PROP_FPS)` 回读值会骗人（请求 25 回读 25，实测仍 20fps）
1. ✅ **图像编解码硬件加速（MPP 硬解）—— 完成（2026-10-24）**：
   - **硬解 p50 1.03ms（vs OpenCV 软解 7.76ms，7.6×）**，200/200 有效帧，9 图 PSNR 49.9~77.2dB（BENCHMARKS 2.6）
   - **关键坑**：系统预装 librockchip_mpp.so 与内核驱动版本倒挂 → put_packet 恒返回 -1012；
     用 MPP 源码编译静态库一行不改直接通（TROUBLESHOOTING 第 17 条）
   - **全链路真实闭环验证**：`edgevision_pro_rknn`（MPP硬解→CSC→letterbox→NPU→BEV投影）
     real_000.jpg 检出 `person 0.813 → dist 5.89m`，COCO 图 6 检出，分数与历史 0.8072/0.60 mAP 口径一致
   - 旧版 `zero_copy_pipeline.hpp`（假数据, fd 硬编码）已废弃铲除，`edgevision_pro_main.cpp` 全真链路重写；
   - **README 旗舰 3D BEV 实测演示图去伪存真完成(2026-10-26)**：彻底废弃旧 PC ONNX 仿真假图，用板端原生 C++ 生产程序 `edgevision_pro_rknn`（MPP 硬解 NV12 + 定点 CSC + RKNN DFL-Free INT8 + BEVProjector 几何反投影）重新在真机上跑通真实道路数据，吐出真实 3D 边界框、BEV 鸟瞰雷达与真实 HUD（NPU 19.3ms, MPP 3.3ms），覆盖更新 `docs/images/bev_demo_snapshot.png`。
   - 剩余优化项：① ~~CMAKE_BUILD_TYPE=Release~~ ✅ **已完成（2026-10-25）**，见下方专节；
     ② CSC 51~65ms 主因是 CPU 读 uncached DRM 内存（memcpy 仅 188MB/s）→ 正解 RGA 硬件转换
     （本板 RGA 2.1.0 不支持 full CSC，颜色偏差 ±13% 待量化）**← 现在这是 CPU 侧第一优先项**；
     ③ ~~MPP 硬编 JPEG（省 7.2ms）~~ ❌ **已实测否掉（负结论，见下方专节）**，不要再排期
2. **双目 3D 视觉专项** —— ✅ **双目子项目 `stereo3d` 已开工并真机跑通（2026-10-24）**：
   - **能干什么**：板上 `/root/stereo3d/build/stereo3d_demo`，双摄 1280×720 → 立体矫正 → SGBM 视差 → 深度 → Haar 人脸框内深度中位数 → 3D 坐标 (X,Y,Z)，网页 2×2 实时预览 **`http://<board-ip>:8081`**
   - **实测**：cap 16.3 / rect 5.4 / **sgbm 46.1(瓶颈)** / depth 0.6 / face 20.6 / enc 7.2 ms，全链路 **9.7~11 FPS**；合成真值自检 21 项全 PASS（1/2/3m 层深中值误差 3.94/21.99/6.14 mm）
   - **⚠️ 诚实状态**：未标定（理想平行假设 fx=1108/B=60mm），绝对距离不可信；标定工具链 `tools/calib/` 已交付并自测通过（双目 RMS 0.044px / 极线 y 差 0.029px）
   - **✅ SGBM 提速已完成（task-5）**：诊断确认 OpenCV 带 TBB 已并行（46ms 是真实成本）；6 组×200 帧消融推荐 **nd=32+block=5**（42.83→36.17ms，−15.6%，有效视差占比反升；nd 64→32 理论省一半实测只 15.6%——DP 聚合访存与 D 无关）。参数支持环境变量覆盖（STEREO_NUM_DISP/STEREO_BLOCK）。≤20ms 需降分辨率 480×270，暂不启用
   - **下一步（双目）**：真实场景标定（拍棋盘格 → stereo_calibrate.py）后换掉假设参数，绝对距离才可信
   - 测试协议：**先 `systemctl stop edgevision` 释放 `/dev/video81`，测完必须 `start` 恢复**
   - 文档:`stereo3d/docs/{BENCHMARKS,PIPELINE,LEARNING_NEXT}.md` + `stereo3d/docs/CALIB_FORMAT.md`
3. ✅ **第三阶段：Edge AI Agent 联动（首发通关）**：
   - 板端 C++ 控制面 RPC 服务 `lab/agent/edgevision_agent_server.cpp`（端口 8088）上线，提供 7 温区监控、NPU 3核负载、LED 物理灯控制、9000 端口目标检测采样三大原子硬件工具；
   - PC 客户端 `lab/tests/edge_agent_client.py` 成功打通，内置 ReAct 思考规划引擎（Thought -> Action -> Observation -> Final Answer），实操跑通多步自主巡检。
   - **端侧大模型 RKLLM 原生推理通关（2026-10-24）**：成功部署 `rkllm-runtime v1.3.1`，加载 **Qwen3.5-2B (W8A8, 2.8GB)**，实测调用 NPU 3核与 4个 A76 大核，跑通代数与逻辑推理，系统空闲内存稳定保持 **6.1 GB**！
   - ✅ **纯端侧原生具身智能体 (On-Device Embodied Agent) 闭环首发通关（2026-10-24）**：
     - 程序：`/root/rkllm_deploy/rkllm_embodied_agent`（源码 `lab/agent/rkllm_embodied_agent.cpp`）；
     - 技术架构：无需任何 PC、云端 API 与外网，在 RK3588 单板 NPU 上完成 **大模型推理 -> 原生 Tool Calling 决策 -> 拦截调度 -> Linux sysfs 硬件交互 -> 物理数据回填 -> 终局人话汇报** 100% 本地闭环！
     - 具身能力 1 (物理硬件感知)：自然语言询问板子温度/状态，模型自主触发 `get_system_health`，实测读出 SOC: **34.23°C**、NPU: **35.15°C**、NPU负载: **65%**、可用内存: **4427MB**，并精准总结回答；
     - 具身能力 2 (物理执行器控制)：自然语言下达“把板载指示灯调成急闪报警模式”，模型自主提取参数 `mode="alarm"` 并写入 `/sys/class/leds/work/` 驱动节点，板载 LED 进入 100ms 爆闪；下达“恢复正常心跳”，模型自主调用 `mode="heartbeat"` 恢复呼吸闪烁！
     - 具身能力 3 (视觉检测联动)：原生预置 `get_vision_telemetry`，非阻塞直通本地 9000 端口 YOLOv8 目标检测流。
4. ~~**MPP 硬编 JPEG 加速**~~ ❌ **已实测否掉（2026-10-25，负结论）**：
   本板 MPP JPEG 硬编 **比 OpenCV 软编更慢 1.07~1.16×**，唯一优势是 CPU 占用降到 1/12
   （8.2% vs 99.7%）。延迟模型 `硬编 ≈ 0.35ms + 7.0e-6×像素`（软编无固定开销）——
   每像素算力打平，硬编白付 ioctl+中断往返。**从路线图划掉，CPU 侧优化改做 RGA CSC**。
   代码保留可用（PSNR 42~50dB，合格）；数据见 BENCHMARKS 2.9。

---

## 第三阶段可靠性强化(2026-10-24 晚):P0 修复 + 回归测试 + 巡检守护 + ASR 选型

### ✅ P0 工具层可靠性修复 —— 完成
- 新版 Agent:`/root/rkllm_deploy/rkllm_embodied_agent_p0`(源码 `lab/agent/rkllm_embodied_agent.cpp`,板上副本 md5 ed9e2ec6ef7a7e5d13062513fce16c64,原文件保留未动)
- 修复四件事:
  1. `control_led` 白名单(heartbeat/alarm/on/off),非法值不碰 sysfs 直接报 `invalid_mode`;每步写检查+回读比对(`readback_mismatch`)
  2. **alarm 假成功根因**:板内核 5.10.110 没编 `CONFIG_LEDS_TRIGGER_TIMER`,写 trigger=timer 是 EINVAL,旧代码不查返回值 → 改用户态 100ms 爆闪(fork+execl,pid 记 `/tmp/edgevision_led_alarm.pid`)
  3. `get_vision_telemetry` 收满 248B 才算成功,离线/超时/半帧/畸形分得清,双采样比 frame_id 判新鲜(`fresh` 字段),检测框带坐标
  4. 工具轮数耗尽打未完成警告,不再装作跑完
- **连带坑**(TROUBLESHOOTING 第 20/21 条):fork 子进程继承 NPU 内存映射会钉死 NPU(`rkllm_init errno 14`),必须 fork 后立即 execl
- 冒烟证据:`lab/docs/p0_smoke_evidence.md` + 板上 `/root/rkllm_deploy/p0_smoke/`

### ✅ 回归测试体系 —— 交付并全部通过
- **板上实测 12/12 通过(100%)**，2026-10-24 22:17，含故障注入用例（`--with-faults`：临时停 edgevision 验视觉离线，finally 自动恢复 active ✔）
- 单用例耗时 13.7~66.0s（LLM 端侧推理为主），全套 ~6.5 min；结果落 `_workbench/test_results/20261024_221738/{results.json,summary.md}`
- `lab/tests/agent_regression_test.py`(876 行纯标准库)：12 用例 T01~T12，支持 `--only T03,T04` 只重跑、`--out-dir` 指定结果目录；契约解析锚中文标记
- `lab/tests/bench_contention_sampler.py`：每秒采温区/NPU 三核/内存 → CSV（600 样本实测：zone0 中位 33.31°C，NPU 均值中位 14% / p90 63.5%，内存 4.41~6.65GB）
- `docs/tech_notes/AGENT_TEST_PLAN.md`：用例表 + 指标口径 + NPU 竞争实测流程 + 故障注入安全协议
- **回归过程中修掉 2 个真 bug + 1 个用例缺陷**（详见 TROUBLESHOOTING 第 22 条）：
  1. 爆闪子进程继承 stdout 管道 → 命令卡死 270s（修：exec 前 0/1/2 重定向 /dev/null；270s→21s）
  2. 模型参数值带换行 `\nalarm\n` → 打印成空值、白名单误判（修：提取后 trim）
  3. T05 用「飞行模式」测不出白名单（模型语义映射成合法值 on）→ 改「mode 原样传 flight」
- 另外补了 sys_prompt 条件决策准则（原来「有人就报警否则呼吸」会跳过视觉直接控灯）

### ✅ 快慢双系统巡检守护(快层) —— 交付并完成真实闪灯验收
- `lab/agent/edgevision_patrol_daemon.cpp`:独立 C++17,5s 周期采温度/NPU/视觉帧,状态机 NORMAL/PERSON_ALERT/THERMAL_ALERT 带迟滞,事件写 `/root/patrol/patrol_events.log` + `llm_pending.jsonl` 供慢层 LLM 消费
- 设计原则:**快层绝不加载 LLM**(NPU 竞争/时延/可靠性三理由)
- **LED alarm 假成功已修**(与 Agent 同一根因):原代码写 `trigger=timer`,本板内核没编 `CONFIG_LEDS_TRIGGER_TIMER` 必返 EINVAL 且不查返回值 → 改为用户态爆闪
- **阈值可命令行配置**:`--temp-enter/--temp-exit/--person-score` 等,默认仍是 75/70°C(不为演示写死)
- 单实例 flock 锁,防止两个守护同时抢灯
- **真实闪灯验收通过**(占空比判据):阈值压到 20°C 触发 THERMAL_ALERT,实测 LED 占空比 **49.7% vs heartbeat 基线 20.0%(+29.7pp)**,6s 内翻转 73 次;事件真实落盘
- 设计文档:`docs/tech_notes/PATROL_DAEMON_DESIGN.md`;验收证据:`lab/docs/patrol_led_evidence.md`

### ✅ 多步演示闭环(两幕) —— 交付并全部 PASS
- `lab/tests/agent_multistep_demo.py`(纯标准库):第一幕守护真实告警闪灯 + 第二幕 Agent 一条自然语言多步研判汇报
- 第一幕 PASS:占空比 49.7% vs 20.0%,事件日志与 `llm_pending.jsonl` 均真实落盘
- 第二幕 PASS:端到端 **106.5s**(三次独立测量 106.5 / 112.2 / 123.9s,差异来自每轮生成 token 数不同),工具链 **4~5 个不同工具**(`get_system_health → get_patrol_events → get_led_status → control_led`,部分轮次还调了 `get_vision_telemetry`),Observation **20~29 条**全合法 JSON,未触步数上限,输出 **1315~1350 字**中文研判
  - ⚠️ 口径说明:这里是三次演示的区间值,不是单次精确值;早期本行曾写死「4 个工具 / 20 条 / 1335 字」= 仅第一次演示那次的数据
- **演示虽判 PASS,但翻 trace 发现两个真缺陷并已修复**(详见下方「演示暴露的缺陷」)
- 文档:`docs/tech_notes/AGENT_MULTISTEP_DEMO.md`(含局限与面试讲法)

### ✅ 演示暴露的两个缺陷(2026-10-25 修复)
1. **把正在爆闪的报警灯读成「熄灭」→ 模型编造错误因果**(TROUBLESHOOTING 第 23 条)
   - 现象:守护正在爆闪(占空比 49.7%),`get_led_status` 却返回 `off/brightness 0`,Agent 据此汇报「LED 熄灭导致过热保护被抑制」
   - 根因:`get_led_current_mode()` 单点读 `trigger` 见 `none` 就读一次 `brightness`,撞上 100ms 爆闪灭相 → **周期性物理量用瞬时采样定性 = 混叠误判**
   - 修复:①优先判「爆闪进程是否存活」(`led_user_blink_alive()`,查 pid + `/proc/<pid>/cmdline` 排除僵尸/pid 复用)返回 `alarm_blink`;②兜底 12×15ms 多点采样
   - 验证:同一场景 `off` → `alarm_blink`,陈旧 pid 文件自动清理已实测
2. **`--no-led` 下模型把四种模式挨个试一遍**(4 次 control_led 全被拒)
   - 修复:①`--no-led` 追加最高优先级 sys_prompt;②拒绝返回值加 `hint` 明说「换 mode 也一样被拒」
   - 验证:control_led **4 次 → 1 次**,拒绝后立即如实汇报
3. **回归脚本收尾"假恢复"**:只写 trigger 不清爆闪子进程,且判据 `"heartbeat" in trigger` 对 `[none]` 也判真
   - 修复:先停爆闪子进程+清 pid 文件 → 亮度归位 → 切 trigger → 判据改为匹配 `[heartbeat]` 生效标记 + `blink_left==0`
4. **模型不知道「20°C 是演示阈值」+ 臆测工具实现**
   - 现象:LED 修好后汇报变成「SOC 温度长期偏高(>40°C)…存在硬件降频甚至**永久损坏**的风险」;还自说自话「若系统内部逻辑为 `0=off`,`1=heartbeat`…(推测)」
   - 根因:数据是对的、结论是错的 —— 模型缺「RK3588 多少度才算热」的领域常识,只能按"有没有告警"倒推,把演示阈值当成真实红线
   - 修复 sys_prompt:①补温度常识(正常 30~65°C / >75°C 才算过热 / 85°C 以上才需担心降频)+ 明确「阈值明显偏低(如 20°C)= 验收演示,不要判定真过热」;②禁止臆测工具内部实现(`control_led` 枚举编码、sysfs 数值含义),拿不准就说不确定
   - 修复后同一场景:「SOC 49.9°C ✅ 正常(30~65°C)」「**这些是低阈值演示事件,而非真实过热**」,臆测段落完全消失
   - **工程启示**:不是让模型更聪明,而是补上它缺的那块世界知识 —— Agent 工程最易漏也最见功力的一环

### 下一步(Agent 线)
1. ~~回归测试结果回填~~ ✅ 已回填(12/12 通过,三次全绿)
2. ~~巡检守护默认写 LED 模式用户验收~~ ✅ 已完成(占空比 49.7% 真实闪灯)
3. ~~多步演示闭环(巡检→研判→执行→汇报)~~ ✅ 已完成(两幕 PASS,三次复跑)
4. ~~ASR 板端实测(按 VOICE_ASR_PLAN 评估协议)~~ ✅ 已完成(2026-10-29:ASR RTF 0.023~0.025 / TTS 4 线程 RTF 0.564 / 常驻服务 5.5s→1.8s,见上方 M2 小节与 `BENCHMARKS.md` 2.11)
5. NPU 竞争"仅LLM/仅YOLO"对照场景补测(当前只有混合负载采样)
6. 把两幕演示固化成 systemd 服务(开机自启守护 + 事件触发 Agent) —— 未开始

---

## 编译优化与 MPP 硬编评估(2026-10-25): 一正一负

### ✅ CMAKE_BUILD_TYPE=Release —— 完成(此前所有 CPU 数字都是无优化跑出来的)
- **根因**:`build_native/CMakeCache.txt` 里 `CMAKE_BUILD_TYPE` 是**空的**,`flags.make` 里
  `CXX_FLAGS = -std=gnu++17` —— **一个 `-O` 都没有**。项目此前记录的全部 CPU 侧耗时
  (MJPG 软解 7.76ms / letterbox / DFL 解码 3.09ms / CSC 51~65ms)都是**悲观值**。
- **改动**(`deploy/cpp/CMakeLists.txt`,206→222 行):默认 Release、`-O3 -DNDEBUG` + 架构标志
  自动探测 `-mcpu=cortex-a76.cortex-a55`(失败降级 `-mtune=cortex-a76`)、`option(BUILD_BENCH OFF)`
  把 bench 纳入同一口径、预留 `if(EXISTS src/mpp_jpeg_encoder.cpp)` 集成点、构建摘要打印
- **A/B 实测**(`taskset -c 6` 绑大核,6 轮交替):
  | 口径 | O0 | O3 | 变化 |
  |---|---|---|---|
  | 纯推理 `rknn_run` | 14.79~14.88 ms | 14.90~14.95 ms | **无变化(对照组,证明测量没漂)** |
  | **DFL 解码+NMS** | **3.44~3.51 ms** | **0.62~0.63 ms** | **5.5×** |
  | **全链路** | **18.91~19.06 ms** | **16.21~16.26 ms** | **−14.6%,52.6→61.7 FPS** |
- 二进制从 **206,256 B → 75,688 B(−63%)**
- **方法论收获(可讲)**:不绑核时 O0 的 p50 仍是 3.45ms 但 mean 被拖到 6.4~8.2ms、p95 冲到 17ms
  ——**调度迁移的长尾会污染均值,性能测量必须绑核**
- **生产二进制全链路补测**(同图同模型各 5 次,`taskset -c 6`),把历史「白捡 1.55×」验成真数:
  | 层 | O0 | O3 | 变化 | 判读 |
  |---|---|---|---|---|
  | MPP 硬解 | 1.250 ms | 1.322 ms | +5.8% | **对照组①** 硬件层,不该变 ✔ |
  | **NV12→RGB888 CSC** | **52.016 ms** | **33.583 ms** | **1.549×** ⭐ | 自有循环,优化直接命中 |
  | letterbox(`cv::resize`) | 1.184 ms | 1.222 ms | +3.2% | **对照组②** OpenCV 预编译库,动不了 ✔ |
  | NPU+DFL 解码 | 22.480 ms | 19.708 ms | −2.77 ms | 与 bench 的 −2.78ms 吻合 |
  | **全链路** | **76.928 ms** | **55.836 ms** | **−27.4%(1.378×)** | 13.0 → 17.9 FPS |
  → **两个"不该变"的对照组同时成立**(硬件层 + 预编译库层纹丝不动),唯一自有 CPU 热点 CSC 恰好 1.55×
  —— 排除测量漂移。**注意口径:"1.55×"是 CSC 单层,全链路是 −27.4%**,别混着说。
- 数据:`docs/BENCHMARKS.md` 2.8 节;所有结论都有 6 轮原始日志 + 5 次生产 A/B 日志

### ❌ MPP 硬编 JPEG —— 实测否掉(负结论,价值在于不写进简历当成功)
- `deploy/cpp/{include/mpp_jpeg_encoder.h, src/mpp_jpeg_encoder.cpp, bench/bench_jpeg_encoder.cpp}`
  已实现并验证正确(NV12 输入,输出标准 baseline JPEG,9 图 Y-PSNR 42.46dB / 1280×720 49.27dB)
- **但速度输给软编**:1280×720 mean **7.24 vs 6.65 ms(1.07~1.09×)**;640×640 也是 1.13~1.20×;
  分辨率扫描 64×64~960×540 **全部档位硬编更慢**;4 路并行软编快 **1.88×**
- **唯一优势**:CPU 占用 **8.2% vs 99.7%**,每帧 CPU 时间 0.59ms vs 6.64ms
- **根因(延迟模型)**:硬编 = **0.35ms 固定开销 + 7.0e-6×像素**,软编 = 0 + 7.5e-6×像素
  → 每像素算力打平,硬编白付 ioctl+中断往返;小分辨率固定开销占比更高
- **"真走了硬件"的实证**:`fdba0000.jpege-core` 四核中断数 +3553/+703/+387/+167,其余 VPU 核 0 变化
- **结论**:单路 30fps 场景 CPU 不是瓶颈 → **从路线图划掉「MPP 硬编省 7.2ms」,改做 RGA CSC**
- 数据:`docs/BENCHMARKS.md` 2.9 节;踩坑 6 条在 `bench/mpp_jpeg_encoder_evidence.md`
  (最坑的一条:FIXQP 下 `fps_in_denom=0` 触发 `rc_frm_check_drop` 断言,直接 Segfault)

---

## 车载道路实景 3D 实时看板(2026-10-26 新增,8090 端口)

> 用户需求原话:「让板子循环读高速道路视频,浏览器里看到流畅的动态道路视频流,每帧上 7~8 辆车的 3D 彩色立方体线框紧贴车身移动,
> 右上角自车 BEV 雷达小地图实时动态闪烁」。**已 100% 真实跑通,零假数据。**

### 交付物
| 文件 | 行数 | 说明 |
|---|---|---|
| `deploy/cpp/src/edgevision_dashboard_main.cpp` | 783 | 主程序:mp4 循环 → letterbox 640 → NPU → 跨类别去重 → BEV 投影 → 3D 框 + 雷达 + HUD → MJPEG over HTTP |
| `deploy/cpp/include/bev_render.hpp` | — | 共享渲染层(header-only):`class_color` / `draw_3d_box` / `draw_2d_fallback` / `render_bev_radar` / `paste_radar` |
| `deploy/cpp/web/dashboard.html` | 1270 | 单文件前端,零外链(唯一 `src` 取值 = `/stream`),深色 HUD + 500ms 轮询 `/api/stats` + 断流自愈 |
| `scripts/verify_bev_dashboard.py` | 997 | 端到端验收脚本,纯标准库 6 类检查(页面/stats/snapshot/stream/并发/帧变化性) |

### 实测数字(200 样本,`/api/stats` 直采;口径见 `BENCHMARKS.md` 2.10)
- **24~27 fps**(p50 **25.71** fps);单帧全链路 p50 **38.90 ms**(p95 42.03)
- 分段:**capture 5.42 + prep 7.84 + npu 16.39(dfl 0.69)+ draw 1.58 + encode 7.55**
- 每帧真实 3D 目标 **4~11 个,p50 8 个**(正好是用户要的 7~8 辆);循环 **100+ 圈**
- 端到端验收 **6/6 全绿**,其中第 6 项 **271 帧 → 271 个不同 crc32**(证明是活流不是重发同一张图)

### 三个值得讲的判断
1. **瓶颈不在 NPU**:NPU 16.39ms 只占 42%,而 CPU 侧 `prep 7.84 + encode 7.55 = 15.39ms` 占 40%,
   两者相加吃满 33.3ms 的源帧周期 → 这就是到不了 30fps 的直接原因。**下一步要提速得砍 CPU 侧,不是再压 NPU。**
2. **比单张 jpg 版快(38.90 vs 55.84ms)不是因为优化得好**:单张版在 1080p 全分辨率做 CSC(33.58ms),
   看板先 `resize` 到 720p 再算,等于把这块整个砍掉。**这是分辨率选择的收益,不能算算法优化。**
3. **跨类别去重不能塞进 `postprocess_dfl_free.cpp`**:那个 NMS 是 class-aware(不同类别互不抑制),
   导致一辆车被同时判成 truck/bus/car 输出 3 个 IoU≈0.96 的重合框。但它是**精度评估在用的公共组件**,
   COCO mAP 0.6087/0.6002 就是按它算的 → 改它会让已有指标全部失效。**去重放在看板业务层,口径隔离优先于代码复用。**
   实测丢弃率 ≈0.18 框/帧。

### 明确未实现(不要对外宣传)
H.264 硬解(仍是 libavcodec 软解 5.42ms)/ RGA CSC / DMA-BUF 零拷贝 / 目标跟踪(`#idx` 跨帧不连续)/
RTSP / 摄像头输入(当前只吃视频文件)/ 30fps(实际 24~27,是原速的 82~90%)

---

## 2026-10-28 巡检守护运行态外化(修 Agent「什么时候报警闪烁」答错)

**问题**:问端侧 Agent「什么时候报警闪烁」,它答「只有超过 75°C 才会报警,当前 44.4°C 远低于该阈值,LED 常亮」——
同时出现了「20°C 阈值」和「75°C 才报警」两个互相否定的数字。
上板实测真相:**守护进程当时根本没在运行**,灯是上次异常退出留下的僵尸态
(`trigger=none` + `brightness=255`);而它历史配置用的是 `--temp-enter 20`,并不是 75。
「常亮」这个结论碰巧对,理由全错。

**根因**:告警阈值是守护进程的命令行参数,**属于进程内运行时状态,除它自己没人读得到**;
Agent 只有「历史事件日志」和「写死的默认值 75」两个来源,无法判断一条历史日志是正在生效还是三天前的。
再加上旧版 `get_system_health` 直接返回 `status: normal_optimal`(基于 75°C 判定)这种**结论字段**,
模型连推理都省了直接抄。

**改动(5 处代码 + 1 个 systemd 单元)**:

| 位置 | 改动 |
|---|---|
| `lab/agent/edgevision_patrol_daemon.cpp` | 每周期原子写 `/root/patrol/patrol_state.json`(pid/running/阈值/state/led/updated_at/stale_after_s);启动时自检残留僵尸态并恢复 heartbeat;退出写 `STOPPED` |
| `lab/agent/rkllm_embodied_agent.cpp` | 新增工具 `get_patrol_status`(失联三条件:running + 时间戳未过期 + `kill(pid,0)==0`);删掉 `get_system_health` 的 `status` 结论字段;修复 `get_patrol_events` 丢弃 DAEMON_START 阈值的问题;重写 sys_prompt(「告警阈值 ≠ 物理过热阈值」) |
| `lab/services/edgevision-patrol.service` | 纳入 systemd:`Restart=always` + `ExecStopPost` 恢复 heartbeat + `SuccessExitStatus=0 3`(单实例锁撞码不算失败) |
| `lab/services/edgevision-patrol.default` | → `/etc/default/edgevision-patrol`,阈值走 `PATROL_ARGS`,验收改文件即可 |

**实测(同一块板、同一问题)**:
- 守护在跑(20°C):快照 `state=THERMAL_ALERT / led=alarm`,LED 采样 `255 0 255 0 255 0 255 0`;
  Agent 首轮主动调 `get_patrol_status`,答「**是的,现在会触发 LED 闪烁**……虽然 49°C 在物理安全范围内,但守护阈值极低(20°C)」✅
- 守护已停:快照 `running=false / reason=daemon_stopped`,Agent 答「现在不会自动报警,守护未运行」✅

**用法**:
```bash
systemctl start edgevision-patrol      # 生产阈值 75/70, 间隔 5s
systemctl status edgevision-patrol
cat /root/patrol/patrol_state.json     # 运行态一眼可见
# 验收演示要低阈值: 改 /etc/default/edgevision-patrol 的 PATROL_ARGS 后 restart
```
注意:单元已安装但**未 enable**(`systemctl is-enabled` = disabled),开机自启需手动 `systemctl enable`。

**同日第二轮:LED 状态查询的采样假象**。问「查看当前警告灯状态」,Agent 答「亮度 0(关闭)」+「正常呼吸闪烁」自相矛盾。
板上实测 `trigger=[heartbeat]` 占空比 **22%**(40 次采样亮 9 次)→ **单点读到 0 的概率约 78%**,灯在呼吸,读到的是灭相。
改 `get_led_status`:周期模式(`heartbeat`/`alarm`/`alarm_blink`)改回 **`brightness_duty_pct`**(20×60ms=1.2s 覆盖 1.2 个心跳周期),
静态模式(`on`/`off`)才回瞬时 `brightness`;prompt 禁止把瞬时值当开关状态、要求问「警告灯」时同时查 `get_patrol_status`。
实测修复后:heartbeat → 「亮度占用率约 25%,正常呼吸状态」✅;爆闪 → 「已触发温度告警,LED 进入报警闪烁」✅。
详见 TROUBLESHOOTING 第 23 条「**第二处**」——上一处只修了用户态爆闪,没覆盖内核 `heartbeat` 触发器。

**入口统一**:`rkllm_embodied_agent` 已改为指向 `rkllm_embodied_agent_p0` 的**软链接**,只维护一份二进制。
此前两个入口各自维护(`run_agent.sh` 调前者、交互会话跑后者),改完一个忘同步另一个,验证时因此跑了一次旧二进制。

## 面试材料(2026-10-30 更新)

> 用户指令「停下来准备面试」。**从此不再开新功能**,所有精力转到「把已经做出来的东西讲清楚」。

### 新增
| 文件 | 内容 | 干什么用 |
|---|---|---|
| `docs/RK3588端侧AI部署-面试问答详解.docx` / `.pdf` | **19 章 252 页成书**：193 个问答 + 62 处费曼讲解，含第 16/17 章「简历逐条对应」与附录「十二句不能说错的话」 | **面试主材料，先看这份** |
| `docs/resumes/陈创荣_端侧AI部署_研一实习简历_v13.pdf` | 端侧AI部署版 v13（严格 1 页 A4）：每条 bullet 都是「现象→错误初判→证据→根因→修法」判断链 | 端侧算法/部署岗主力简历 |
| `docs/resumes/陈创荣_Linux应用开发_研一实习简历_v13.pdf` | Linux应用开发版 v13（严格 1 页 A4）：同样改为判断链写法，突出系统编程与排障 | 嵌入式 Linux / C++ 软件岗主力简历 |

### 面试材料的历史沿革
1. 最初只有 `docs/interview/`（W1~W6 六周材料 + 高频八股，179 个问答），2026-10-25 写成。
2. 2026-10-26 扩写成 `docs/interview_book/`（193 个问答 + 费曼讲解 + 16/17 章简历对应），**是旧版的超集**。
3. 2026-10-30 旧版 `docs/interview/` 已删除（内容 100% 被成书覆盖），只保留成书。

### 五条杀手故事(面试最值钱的部分)
1. 官方 5.5ms vs 实测 37ms —— 同算子位置不同差 6 倍,归因不靠猜
2. 余弦 0.9965 骗了我 —— cls 精确等于 0.5 = sigmoid(0),分类头早废了
3. **演示全绿但 Agent 在说谎** —— 分层定位「模型幻觉 vs 感知层喂错数据」
4. 模型不缺智商,缺**可观测性** —— 先被判 49°C「永久损坏风险」,补 75°C 常识后又拿它去否定守护的真实告警(守护阈值 20°C)。两次都不是幻觉,是它没法知道"当前生效阈值到底是多少"
5. **MPP 硬编走通了,但我把它从路线图划掉** —— 负结论也是结论

### 红线(绝对不能说出口)
端到端 18ms / 三核 150+ FPS / 功耗 9.5W / DMA-BUF 零拷贝 / RGA 硬件 CSC /
RTSP-H.264 硬解 / 4 路 1080P / ByteTrack 进 C++ / 余弦 0.9965 当精度

### ⚠️ 投递前必须解决
`git push` 卡凭据 —— 本地 SSH key 不被 GitHub 认可、无 PAT。**仓库不推上去,README 就白扫了**(面试官点不开链接)。当前 `main` 已 ahead 多个 commit。
