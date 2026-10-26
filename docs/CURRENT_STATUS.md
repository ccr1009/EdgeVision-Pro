# 当前状态快照(CURRENT_STATUS)

> 完整背景看项目根目录的 **`AGENTS.md`**(新对话会自动加载)。

## 一句话
RK3588 EVM3588-B 边缘AI校招项目 + Linux 应用层学习。

> 最后一次更新:**2026-10-26 · 实习简历定稿 v12 (DOCX/PDF 双格式 Exactly 1 Page)**(全面同步 DFL-Free 57% 优化、COCO128 mAP 0.6002、MPP 硬件解码 1.03ms、Qwen3.5-2B 单板具身 Agent 硬件闭环)

## 现在能干什么
- SSH 板子:`ssh root@<board-ip>`(**已配免密**;别名 `ssh rk3588`;有线。IP 与密码见本地私密记录,不入库)
- **工业级全链路服务**:`systemctl status edgevision`(开机自启、5秒自愈、3核 NPU、真实摄像头、TCP 9000 推流)
- **网页实时预览(带真实检测框)**:浏览器开 **`http://<board-ip>:8080`**
  - 启动:`cd /root/lab && ./w4_edgevision_preview_real /dev/video81 0.25`
  - 注意它和 9000 网关**抢同一个 `/dev/video81`**,同时跑必须错开
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

---

## 第二阶段进展(第 5~7 周):端侧推理深度剖析与模型优化 —— 核心已完成

### ✅ 1. 真实画框与后处理(所见即所得)—— 完成，且做成了网页实时预览
- `lab/w4_edgevision_preview_real.cpp`:用新模型 + `dflfree::decode` 画**真实检测框**。
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

0. ✅ **摄像头 20fps 之谜已破（2026-10-24）** —— 见 `docs/CAMERA_FPS_ROOT_CAUSE.md`
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
2. **双目 3D 视觉专项** —— ✅ **新项目 `stereo3d-rk3588` 已开工并真机跑通（2026-10-24）**：
   - **能干什么**：板上 `/root/stereo3d/build/stereo3d_demo`，双摄 1280×720 → 立体矫正 → SGBM 视差 → 深度 → Haar 人脸框内深度中位数 → 3D 坐标 (X,Y,Z)，网页 2×2 实时预览 **`http://<board-ip>:8081`**
   - **实测**：cap 16.3 / rect 5.4 / **sgbm 46.1(瓶颈)** / depth 0.6 / face 20.6 / enc 7.2 ms，全链路 **9.7~11 FPS**；合成真值自检 21 项全 PASS（1/2/3m 层深中值误差 3.94/21.99/6.14 mm）
   - **⚠️ 诚实状态**：未标定（理想平行假设 fx=1108/B=60mm），绝对距离不可信；标定工具链 `tools/calib/` 已交付并自测通过（双目 RMS 0.044px / 极线 y 差 0.029px）
   - **✅ SGBM 提速已完成（task-5）**：诊断确认 OpenCV 带 TBB 已并行（46ms 是真实成本）；6 组×200 帧消融推荐 **nd=32+block=5**（42.83→36.17ms，−15.6%，有效视差占比反升；nd 64→32 理论省一半实测只 15.6%——DP 聚合访存与 D 无关）。参数支持环境变量覆盖（STEREO_NUM_DISP/STEREO_BLOCK）。≤20ms 需降分辨率 480×270，暂不启用
   - **下一步（双目）**：真实场景标定（拍棋盘格 → stereo_calibrate.py）后换掉假设参数，绝对距离才可信
   - 测试协议：**先 `systemctl stop edgevision` 释放 `/dev/video81`，测完必须 `start` 恢复**
   - 文档:`stereo3d-rk3588/docs/{BENCHMARKS,PIPELINE,LEARNING_NEXT}.md` + 根 `docs/CALIB_FORMAT.md`
3. ✅ **第三阶段：Edge AI Agent 联动（首发通关）**：
   - 板端 C++ 控制面 RPC 服务 `lab/edgevision_agent_server.cpp`（端口 8088）上线，提供 7 温区监控、NPU 3核负载、LED 物理灯控制、9000 端口目标检测采样三大原子硬件工具；
   - PC 客户端 `lab/edge_agent_client.py` 成功打通，内置 ReAct 思考规划引擎（Thought -> Action -> Observation -> Final Answer），实操跑通多步自主巡检。
   - **端侧大模型 RKLLM 原生推理通关（2026-10-24）**：成功部署 `rkllm-runtime v1.3.1`，加载 **Qwen3.5-2B (W8A8, 2.8GB)**，实测调用 NPU 3核与 4个 A76 大核，跑通代数与逻辑推理，系统空闲内存稳定保持 **6.1 GB**！
   - ✅ **纯端侧原生具身智能体 (On-Device Embodied Agent) 闭环首发通关（2026-10-24）**：
     - 程序：`/root/rkllm_deploy/rkllm_embodied_agent`（源码 `lab/rkllm_embodied_agent.cpp`）；
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
- 新版 Agent:`/root/rkllm_deploy/rkllm_embodied_agent_p0`(源码 `lab/rkllm_embodied_agent.cpp`,板上副本 md5 ed9e2ec6ef7a7e5d13062513fce16c64,原文件保留未动)
- 修复四件事:
  1. `control_led` 白名单(heartbeat/alarm/on/off),非法值不碰 sysfs 直接报 `invalid_mode`;每步写检查+回读比对(`readback_mismatch`)
  2. **alarm 假成功根因**:板内核 5.10.110 没编 `CONFIG_LEDS_TRIGGER_TIMER`,写 trigger=timer 是 EINVAL,旧代码不查返回值 → 改用户态 100ms 爆闪(fork+execl,pid 记 `/tmp/edgevision_led_alarm.pid`)
  3. `get_vision_telemetry` 收满 248B 才算成功,离线/超时/半帧/畸形分得清,双采样比 frame_id 判新鲜(`fresh` 字段),检测框带坐标
  4. 工具轮数耗尽打未完成警告,不再装作跑完
- **连带坑**(TROUBLESHOOTING 第 20/21 条):fork 子进程继承 NPU 内存映射会钉死 NPU(`rkllm_init errno 14`),必须 fork 后立即 execl
- 冒烟证据:`lab/p0_smoke_evidence.md` + 板上 `/root/rkllm_deploy/p0_smoke/`

### ✅ 回归测试体系 —— 交付并全部通过
- **板上实测 12/12 通过(100%)**，2026-10-24 22:17，含故障注入用例（`--with-faults`：临时停 edgevision 验视觉离线，finally 自动恢复 active ✔）
- 单用例耗时 13.7~66.0s（LLM 端侧推理为主），全套 ~6.5 min；结果落 `_test_results/20261024_221738/{results.json,summary.md}`
- `lab/agent_regression_test.py`(876 行纯标准库)：12 用例 T01~T12，支持 `--only T03,T04` 只重跑、`--out-dir` 指定结果目录；契约解析锚中文标记
- `lab/bench_contention_sampler.py`：每秒采温区/NPU 三核/内存 → CSV（600 样本实测：zone0 中位 33.31°C，NPU 均值中位 14% / p90 63.5%，内存 4.41~6.65GB）
- `docs/AGENT_TEST_PLAN.md`：用例表 + 指标口径 + NPU 竞争实测流程 + 故障注入安全协议
- **回归过程中修掉 2 个真 bug + 1 个用例缺陷**（详见 TROUBLESHOOTING 第 22 条）：
  1. 爆闪子进程继承 stdout 管道 → 命令卡死 270s（修：exec 前 0/1/2 重定向 /dev/null；270s→21s）
  2. 模型参数值带换行 `\nalarm\n` → 打印成空值、白名单误判（修：提取后 trim）
  3. T05 用「飞行模式」测不出白名单（模型语义映射成合法值 on）→ 改「mode 原样传 flight」
- 另外补了 sys_prompt 条件决策准则（原来「有人就报警否则呼吸」会跳过视觉直接控灯）

### ✅ 快慢双系统巡检守护(快层) —— 交付并完成真实闪灯验收
- `lab/edgevision_patrol_daemon.cpp`:独立 C++17,5s 周期采温度/NPU/视觉帧,状态机 NORMAL/PERSON_ALERT/THERMAL_ALERT 带迟滞,事件写 `/root/patrol/patrol_events.log` + `llm_pending.jsonl` 供慢层 LLM 消费
- 设计原则:**快层绝不加载 LLM**(NPU 竞争/时延/可靠性三理由)
- **LED alarm 假成功已修**(与 Agent 同一根因):原代码写 `trigger=timer`,本板内核没编 `CONFIG_LEDS_TRIGGER_TIMER` 必返 EINVAL 且不查返回值 → 改为用户态爆闪
- **阈值可命令行配置**:`--temp-enter/--temp-exit/--person-score` 等,默认仍是 75/70°C(不为演示写死)
- 单实例 flock 锁,防止两个守护同时抢灯
- **真实闪灯验收通过**(占空比判据):阈值压到 20°C 触发 THERMAL_ALERT,实测 LED 占空比 **49.7% vs heartbeat 基线 20.0%(+29.7pp)**,6s 内翻转 73 次;事件真实落盘
- 设计文档:`docs/PATROL_DAEMON_DESIGN.md`;验收证据:`lab/patrol_led_evidence.md`

### ✅ 多步演示闭环(两幕) —— 交付并全部 PASS
- `lab/agent_multistep_demo.py`(纯标准库):第一幕守护真实告警闪灯 + 第二幕 Agent 一条自然语言多步研判汇报
- 第一幕 PASS:占空比 49.7% vs 20.0%,事件日志与 `llm_pending.jsonl` 均真实落盘
- 第二幕 PASS:端到端 **106.5s**(三次独立测量 106.5 / 112.2 / 123.9s,差异来自每轮生成 token 数不同),工具链 **4~5 个不同工具**(`get_system_health → get_patrol_events → get_led_status → control_led`,部分轮次还调了 `get_vision_telemetry`),Observation **20~29 条**全合法 JSON,未触步数上限,输出 **1315~1350 字**中文研判
  - ⚠️ 口径说明:这里是三次演示的区间值,不是单次精确值;早期本行曾写死「4 个工具 / 20 条 / 1335 字」= 仅第一次演示那次的数据
- **演示虽判 PASS,但翻 trace 发现两个真缺陷并已修复**(详见下方「演示暴露的缺陷」)
- 文档:`docs/AGENT_MULTISTEP_DEMO.md`(含局限与面试讲法)

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
4. ASR 板端实测(按 VOICE_ASR_PLAN 评估协议)
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

## 面试材料(2026-10-25 晚):开发线收尾,转面试冲刺

> 用户指令「停下来准备面试」。**从此不再开新功能**,所有精力转到「把已经做出来的东西讲清楚」。

### 新增
| 文件 | 内容 | 干什么用 |
|---|---|---|
| `docs/INTERVIEW_PREP_PLAN.md` | 7 天排期 / **两分钟项目自述逐字稿(约 110 秒)** / 五条杀手故事(各 90 秒) / 红线表(9 条绝对不能说) / 被问倒怎么办 / 反问 / 投递前清单 | **这是总入口,先看这份** |
| `docs/陈创荣_端侧AI部署_研一实习简历_v12.docx` / `.pdf` | **端侧AI部署版 v12（严格 1 页 A4）**：聚焦 DFL-Free 实测（纯推理 15.95ms/全链路 20.58ms/48.6 FPS）、INT8 COCO128 mAP 0.6002（-1.4%）、MPP 硬解 1.03ms（7.6×）、Qwen3.5-2B 单板 Tool Calling 闭环 | **端侧算法/部署岗主力简历** |
| `docs/陈创荣_Linux应用开发_研一实习简历_v12.docx` / `.pdf` | **Linux应用开发版 v12（严格 1 页 A4）**：技能重排突出 C++/并发/Socket网络/系统服务/外设驱动/构建优化；项目语言转换为系统工程（有界队列0丢帧、定长帧二进制推流、背压丢旧防积压、Cache优化、CMake -O3 重构、驱动版本倒挂排错）；AI 作为异构计算壁垒加分项 | **嵌入式 Linux / C++ 软件岗主力简历** |
| `docs/interview-recap/W5-端侧推理部署.md` | RKNN 量化、DFL 图手术、三核调度、逐层剖析、**测量方法论**、编译优化、MPP 一正一负、边界 | 端侧 AI 部署岗第一主力 |
| `docs/interview-recap/W6-端侧Agent.md` | RKLLM 部署、Tool Calling、5 个硬件工具、快慢双系统、**三个真缺陷复盘**、回归体系、边界 | 差异化卖点 |

### 修正了 3 处会主动自贬的过期内容
1. `interview-recap/README.md` 的「诚实边界」表 4 条全是 8 天前状态(说没 NMS / 没接 MPP / 只测推理边界)——**这些早就做完了**,照旧背等于白让功劳。已重写成 **11 行真实状态表**(✅ 已实现 4 项 / ❌ 未实现 7 项),必背数字从 13 行扩到 **A~E 五组 22 行**。
2. **完全没有 W5/W6** —— 恰好是 JD 明写「RK3588 + RKNN/RKLLM 优先」的核心两块。已补齐。
3. `docs/RESUME_校招_端侧AI部署.md` 里还写着 RKNN Runtime **1.5.2**、DFL 头「**正在实施**」、「NPU 推理**尚为桩实现**」—— 全是 8 天前的状态。已连同 `RESUME_INTERN_2026.md` 一起把数字口径统一到最新实测。

### 五条杀手故事(面试最值钱的部分)
1. 官方 5.5ms vs 实测 37ms —— 同算子位置不同差 6 倍,归因不靠猜
2. 余弦 0.9965 骗了我 —— cls 精确等于 0.5 = sigmoid(0),分类头早废了
3. **演示全绿但 Agent 在说谎** —— 分层定位「模型幻觉 vs 感知层喂错数据」
4. 模型不缺智商,缺常识 —— 49°C 被判成「永久损坏风险」
5. **MPP 硬编走通了,但我把它从路线图划掉** —— 负结论也是结论

### 红线(绝对不能说出口)
端到端 18ms / 三核 150+ FPS / 功耗 9.5W / DMA-BUF 零拷贝 / RGA 硬件 CSC /
RTSP-H.264 硬解 / 4 路 1080P / ByteTrack 进 C++ / 余弦 0.9965 当精度

### ⚠️ 投递前必须解决
`git push` 卡凭据 —— 本地 SSH key 不被 GitHub 认可、无 PAT。**仓库不推上去,README 就白扫了**(面试官点不开链接)。当前 `main` ahead **7**。
