# RK3588 项目工作区 —— 使用说明

> 一块 **EVM3588-B（英码科技 EMA，RK3588 / 8GB / Debian 11）** 开发板上的端侧 AI 项目集合。
> 这份文档是**根目录的入口**：告诉你每个目录是什么、怎么把东西跑起来、哪些约定不能破。
>
> 只想知道「现在能干什么」→ 直接看 `edgevision-rk3588/docs/CURRENT_STATUS.md`。

---

## 1. 这是什么

三条技术线，一条比一条靠上：

| 线 | 内容 | 关键产出 |
|---|---|---|
| **端侧推理加速** | YOLOv8n 在 NPU 上的 INT8 部署与压榨 | DFL 头图手术（37.19 → 15.95 ms/帧，−57%）、真 mAP 掉点量化（−1.4%）、MPP 硬解（7.6×） |
| **端侧具身 Agent** | RKLLM 上的 Qwen3.5-2B + 硬件 Tool Calling | 纯单板离线「感知 → 决策 → 执行」闭环，**6 个**硬件工具，12 用例回归；prompt 分层后工具轮数 **5 → 1**、响应 **68 s → 10 s** |
| **语音交互** | Paraformer ASR + vits TTS + 软唤醒编排网关 | 说完到出声 **~11 s**（ASR RTF **0.023**、TTS 常驻后首段合成 **1 s**）；软唤醒词「小瑞」含同音字容错 |
| **双目 3D 感知** | USB 双摄 → 立体矫正 → SGBM → 深度 → 3D 坐标 | `Z = fx·B/d`，误差传播 `ΔZ = Z²/(fx·B)·Δd`，3m 内可用 |

定位是**校招作品集**：所有数字都来自真机实测，口径与局限写在同一份文档里（`docs/BENCHMARKS.md`）。
没有实现的东西**明确列出来**，不吹。

**整体架构（三条线怎么串起来、共享哪些资源）见 [`docs/ARCHITECTURE.md`](edgevision-rk3588/docs/ARCHITECTURE.md)。**

---

## 2. 目录地图

```
D:\a研究生\项目\rk3588项目\
├── README.md                  ← 本文件（工作区使用说明，不是 git 仓库）
├── AGENTS.md                  ← 项目总纲：我是谁 / 已完成的证据 / 下一步 / 协作偏好
├── edgevision-rk3588\         ← ★ 主项目，唯一 git 仓库
├── rkllm_deploy\              ← 板上 /root/rkllm_deploy 的本地镜像（runtime + demo 源码）
├── rk3588开发板配套资料\       ← 英码官方 SDK / 镜像 / 手册（27.5 GB，只读不动）
├── _tools\                    ← 本地辅助脚本（体检 / 定量实验 / 排查复现）
├── _workbench\                ← 所有临时、探针、历史产物（默认不进任何 git）
└── .workbuddy\                ← AI 助手的历史会话记忆
```

| 条目 | 是什么 | 该不该动 |
|---|---|---|
| `edgevision-rk3588/` | 主仓库。代码在 `deploy/cpp`（C++ 引擎）、`lab`（练习与 Agent 源码）、`stereo3d`（双目）、`convert`（模型转换）；文档全在 `docs/` | ✅ 日常就在这改 |
| `rkllm_deploy/` | `librkllmrt.so`（7.3 MB，RKLLM runtime）+ 4 个 demo 源。**Agent 真源码在 `edgevision-rk3588/lab/rkllm_embodied_agent.cpp`** | 只读参考 |
| `rk3588开发板配套资料/` | 厂商资料。刷机镜像、SDK、原理图 | ❌ 别动，也别提交 |
| `_tools/` | 39 个脚本：体检（`npu_doctor.sh`）、跑 Agent（`run_agent.sh`）、IOMMU 定量实验、排查复现链 | 看 `_tools/README.md` 分类再用 |
| `_workbench/` | `probes/`(探针 331 文件) `test_results/`(回归产物) `board_scripts/` `session_logs/` `tools_residue/` `stereo3d-rk3588-archive/` | 见 `_workbench/README.md`，可整块清 |
| `.workbuddy/` | 2026-10 的会话记忆与协作教训 | 只读 |

---

## 3. 五分钟上手

### 路径 A — 只想读代码和结论

按这个顺序，一共约 40 分钟：

1. `edgevision-rk3588/README.md` —— 项目是什么、旗舰演示、明确未实现清单
2. `edgevision-rk3588/docs/CURRENT_STATUS.md` —— 现在能干什么 + 下一步（**接手先看这份**）
3. `edgevision-rk3588/docs/BENCHMARKS.md` —— 所有性能/精度数字，含测量口径与局限
4. `edgevision-rk3588/docs/TROUBLESHOOTING.md` —— 27 条踩坑记录（面试素材库）
5. `edgevision-rk3588/docs/LEARNING_PLAN_4WEEKS.md` —— 学习路线

### 路径 B — 连板子把东西跑起来

```bash
# 0) SSH（已配免密，密钥 ~/.ssh/id_ed25519_rk3588，别名为 rk3588）
ssh rk3588
# 或 ssh root@<board-ip>

# 1) 看服务状态
systemctl status edgevision          # 9000 TCP 推流网关，开机自启 + 异常自愈
bash /root/npu_doctor.sh             # 六节体检：NPU 驱动 / 谁在占 / 残留实例 / 结论

# 2) 浏览器开三个入口（把 <board-ip> 换成板子 IP）
#    http://<board-ip>:8080   实时检测预览（MJPEG + 3D 框 + HUD）
#    http://<board-ip>:8090   车载道路 3D 实时看板（视频循环 + BEV 雷达）
#    http://<board-ip>:8088/api/status   Agent RPC 控制面状态
```

板子 IP 不入库。所有脚本都从环境变量取：

```bash
export EDGEVISION_BOARD_HOST=<board-ip>     # 三个 lab 脚本缺这个会直接 exit 2
export EDGEVISION_BOARD_PORT=22
export EDGEVISION_SSH_KEY=~/.ssh/id_ed25519_rk3588
export STEREO3D_BOARD=<board-ip>            # stereo3d 侧
```

### 路径 C — 改代码并重新编译

**一律在板子上原生编译**（PC 交叉编译会撞 glibc：Ubuntu 22 的 2.35 vs 板子 Debian 11 的 2.31）。

```bash
# 板子上
cd /root/edgevision && rsync -a --exclude build_native /path/to/local/edgevision-rk3588/deploy/ .  # 或 scp
cd deploy/cpp
cmake -S . -B build_native -DCMAKE_BUILD_TYPE=Release -DCROSS_COMPILE_RK3588=ON
cmake --build build_native -j4
```

CMake 已默认 `Release` + `-O3 -DNDEBUG`，并在缺 `-O` 时打印 `WARNING` 自检 ——
**这个项目曾因 build type 为空跑了一个月 `-O0`**（TROUBLESHOOTING 第 24 条）。

---

## 4. 端口与子系统

| 端口 | 子系统 | 启动方式 | 说明 |
|---|---|---|---|
| 8080 | 实时检测预览 | `lab/vision/edgevision_preview_real.cpp` → 板端 `edgevision_preview_real` | MJPEG + 真实检测框 + 3D 棱边 + BEV 小地图 |
| 8081 | 双目 stereo3d | `stereo3d/build/stereo3d_demo` | 2×2 拼图（原图/矫正/视差/深度） |
| 8088 | Agent RPC 控制面 | `lab/agent/edgevision_agent_server.cpp` | `/api/status` `/api/led` `/api/vision` |
| 8090 | 车载 3D 看板 | `deploy/cpp` target `edgevision_dashboard` | 板端循环读 mp4 → 浏览器活流 |
| 9000 | TCP 推流网关 | `systemd: edgevision.service` | 248 字节定长帧（8B header + 10×24B BBox） |

⚠️ **8080 与 9000 抢同一个摄像头**（`/dev/video81`），别同时开。

---

## 5. 四条硬约束（踩过坑，别再踩）

### ① NPU IOMMU 只有 4 GiB，跑 Agent 前必须清场

RK3588 NPU 的 IOMMU 地址空间是 32 位（4096 MiB），与物理内存 7.5 GB 无关。
一个 RKLLM 实例约占 **~1.95 GB IOVA**，所以：

- 干净板子最多 **2 个** rkllm 实例
- 若 `edgevision.service` + 看板同时跑，**第 2 个就失败**，报 `rkllm_init failed -1 / errno 14 Bad address`

```bash
bash /root/npu_doctor.sh      # 先体检
bash /root/run_agent.sh       # 自动停网关/看板 → 起 Agent → 退出时 trap 恢复
```

### ② 只在板子上原生编译

交叉编译 glibc 版本不匹配，跑起来直接 `GLIBC_2.34 not found`。

### ③ Windows 上别用复杂内联 ssh

PowerShell 会把 `ssh root@<ip> 'a | b; c'` 的引号吃掉 → **写成 `.sh` 脚本 scp 上去再 `bash` 执行**。

### ④ 别用 PowerShell 读写含中文的 UTF-8 源文件

`Set-Content -Encoding UTF8` / `Out-File` 会加 **BOM**，中文会坏、commit message 会乱码。
用编辑器改；脚本里一律 `UTF8Encoding($false)` 或 Python `newline=''` + `encoding='utf-8'`。

---

## 6. 文档地图

### 核心五份（`edgevision-rk3588/docs/`）

| 文档 | 什么时候看 |
|---|---|
| `CURRENT_STATUS.md` | 接手第一份：现在能干什么 + 下一步 |
| `BENCHMARKS.md` | 要任何数字：性能/精度，含口径与局限 |
| `TROUBLESHOOTING.md` | 遇到问题先搜这里；新问题按三问规则追加 |
| `LEARNING_PLAN_4WEEKS.md` | 12 周学习路线（四阶段） |
| `README.md` | 对外门面：亮点 + 旗舰演示 + 明确未实现清单 |

### 分类目录

| 目录 | 内容 | 入库 |
|---|---|---|
| `docs/tech_notes/` | 专项技术方案：Agent 多步演示、回归测试计划、巡检守护设计、摄像头 30fps 根因、BEV 网关架构、ASR 选型 | ✅ |
| `docs/interview/` | 面试冲刺：7 天排期、W1~W6 逐周素材、高频八股速查 | ✅ |
| `docs/interview_book/` | 面试书 18 章源文件 + DOCX/PDF 构建脚本 | ✅（简历文本除外） |
| `docs/archive/` | 历史与职业材料：交接文档、入职指南、12 周路线、学习指南、专利交底书 | ✅ |
| `docs/resumes/` | 简历 docx/pdf/png、生成器、他人简历 | ❌ **`.gitignore` 整目录屏蔽** |

根 `AGENTS.md` 是给 AI 会话的：我是谁、已完成的事（带证据）、下一步、协作偏好。

---

## 7. 维护约定（三条红线）

完整版在 `AGENTS.md` 开头的「工作约定」，这里只列不能破的：

1. **不许用假数据冒充结果。** 演示/汇报/截图里任何数字必须来自真实链路。
   `frame_id % N` 造偏移、硬编码置信度、空检测补框 —— 这类代码一次都不行（本项目踩过两次）。
2. **`TROUBLESHOOTING.md` 只记「面试能讲」的问题。** 记录前过三问：
   根因能讲底层原理？排查有方法论？有可量化证据？三问全过才写全文，过一两问只在速查清单加一行，一问不过**不记**。
3. **完成实质工作要同步三份文档：**
   `docs/CURRENT_STATUS.md`（现在能干什么）→ `AGENTS.md` 第 3 节（一条带证据的 ✅）→ `docs/BENCHMARKS.md`（数字 + 口径）。

### 命名与存放约定

- `_` 前缀 = **非源码、非交付物**（`_tools` `_workbench` `_p6_*` `_test_results`）。会被清理，别把唯一副本放这。
- 一次性探针、回归输出、回拉日志 → `_workbench/`，**不要放仓库里**。
- 仓库里只放：源码、被测过的模型配置、文档、可复现的证据图（小图）。

---

## 8. `_workbench/` 与 `_tools/`

| 目录 | 放什么 | 保留策略 |
|---|---|---|
| `_workbench/probes/` | MPP 攻坚期 265 个探针脚本 + cmake 优化实验 | 需要就留，结论已固化进 `docs/BENCHMARKS.md` 后可整块删 |
| `_workbench/test_results/` | Agent 回归/多步演示的 54 次运行结果 | 保留最近 10 次，更早的删 |
| `_workbench/board_scripts/` | 板上一次性启动脚本 | 结论已文档化后可删 |
| `_workbench/session_logs/` | 两次长会话转录（含手机号，**永不入库**） | 留作素材，介意就删 |
| `_workbench/tools_residue/` | `_tools` 清出的调用痕迹、旧简历生成器、ACL 报告、会话备份 | 可整块删 |
| `_workbench/stereo3d-rk3588-archive/` | 双目项目早期快照（标定原图 19 MB + 合成真值图 21 MB） | 见其中 `ARCHIVED.md`，确认无用后整块删 |

`_tools/` 的分类（现役 / 一次性排查 / 已被取代）见 `_tools/README.md`。

> 根目录**不是 git 仓库**，所以 `_workbench/` 天然不会误提交。真正的护栏在
> `edgevision-rk3588/.gitignore`（个人材料、构建产物、渲染中间件全在那里）。

---

## 9. 当前状态与下一步

**已完成（有实测证据）**：四阶段学习路线通关 → EdgeVision V1.0 工业级网关 →
DFL-Free 模型改造 + INT8 量化修复 + 真 mAP 评测 → MPP 硬解 + Release 编译优化 →
RKLLM 具身 Agent（纯单板离线闭环）→ 车载 3D 实时看板 → 双目 3D 感知。

**明确未实现（不要对外宣传）**：H.264/265 硬解、RTSP、RGA 硬件 CSC、DMA-BUF 零拷贝、
ByteTrack 进 C++、4 路 1080p、30 fps（实际 24~27）、功耗数字。

**下一步**：见 `edgevision-rk3588/docs/CURRENT_STATUS.md` 末尾。
当前主线是**面试与实习投递**，不是继续开功能。
