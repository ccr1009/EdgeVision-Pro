# AGENTS.md — 仓库内协作须知（**一页指路**）

> **本文不是总纲。总纲在这里 ↓**
>
> # `D:\a研究生\项目\rk3588项目\AGENTS.md`（65 KB，权威版）
>
> 那份含完整 12 周路线、全部踩坑记录、协作偏好。**每次新对话先读它。**
> 本文件只放三样东西：① 总纲在哪 ② 从哪儿读起 ③ 绝对不能违反的硬约束。
>
> **为什么改成这样（2026-11-02）**：本文件以前是一份 6.3 KB 的"精简副本"，
> 试图把总纲浓缩下来。结果是**它自己会过期而没人发现**——例如它曾写着
> "5 个硬件工具"，而 `lab/agent/rkllm_embodied_agent.cpp` 里实际有 **7 个**
> （多出 `get_patrol_status` 和 `control_service`）。同一份信息存两处，
> 迟早有一处是错的。现在只留指路，不再复述。

---

## 一、总纲在哪（含镜像）

| 位置 | 说明 |
| :--- | :--- |
| `D:\a研究生\项目\rk3588项目\AGENTS.md` | **权威版，65 KB，改这份** |
| `docs/workspace/AGENTS.md` | 上述文件的镜像（已脱敏，供备份/异地恢复用） |

> 工作区根目录**不是 git 仓库**，所以总纲本身靠 `docs/workspace/` 那个镜像做备份。
> 改了正本记得同步镜像，见 `docs/workspace/INDEX.md`。

---

## 二、接手时按这个顺序读

| 顺序 | 文件 | 为什么 |
| :---: | :--- | :--- |
| 1 | **根目录 `AGENTS.md`** | 总纲：我是谁 / 硬件 / 坑 / 已完成 / 路线 / 偏好 |
| 2 | `docs/CURRENT_STATUS.md` | **现在能干什么** + 下一步 |
| 3 | `docs/BENCHMARKS.md` | 所有数字的口径与复现命令（**引用数字只认这份**） |
| 4 | `docs/ARCHITECTURE.md` | 三条技术线怎么串、端口进程表、资源争用 |
| 5 | `lab/README.md` | 项目主线源码在哪（不是"练习目录"） |

---

## 三、目录速查

| 目录 | 放什么 | 关键提醒 |
| :--- | :--- | :--- |
| `deploy/cpp/` | 视觉引擎 CMake 工程（6 个 target） | **不含 9000 网关** |
| `lab/` | 项目主线源码，**按职责分 8 个子目录**，顶层只有 README | 见下方各子目录 |
| `lab/agent/` | Agent 主体、巡检守护、8088 控制面（3 源 + 3 诊断） | 项目主线 |
| `lab/voice/` | 语音网关、TTS 常驻、启停脚本 | 项目主线 |
| `lab/vision/` | **9000 网关**、8080 预览 | 9000 网关是脊柱，**不在 deploy/cpp 里** |
| `lab/tests/` | 回归/探索/声学自检/客户端（13 个） | 改完代码必须跑前 4 个 |
| `lab/services/` | 5 个 systemd unit | 装到 `/etc/systemd/system/` |
| `lab/build/` | 6 个编译与上线脚本 | `deploy.sh` 只覆盖 Agent 与语音网关 |
| `lab/drafts/` | 31 个历史草稿 | **不要改**，结论已在 `docs/` |
| `lab/tools/` | 攻坚工具/探针源码（25 个 .cpp + 25 个 .sh） | 从板端抢救回来，含 RGA **失败**尝试 |
| `lab/scripts/` | 26 个调试脚本 + `board_root/` 15 个 | 价值低，记录排查过程 |
| `learn/` | **第 1~4 周学习线代码（26 个）** | 与 `docs/LEARNING_PLAN_4WEEKS.md` 对应 |
| `esp32_gimbal/` | ESP-IDF 云台工程 + 8 个 host 脚本 | 独立工程，消费 9000 的 248B 帧 |
| `stereo3d/` | 双目 3D（自带 CMake） | 与主仓零代码依赖，**只抢摄像头** |
| `tools/` | 根 `_tools/` 的脱敏镜像 | 用前设 `BOARD_IP` / `BOARD_PASSWORD` |
| `docs/workspace/` | 根目录 4 份文档镜像 | 只读备份，改正本 |
| `gateway/ bev/ pipeline/ track/` | 早期 Python 原型（2026-09） | 四者互相 import，已不是主线 |

---

## 四、绝对不能违反的（这几条会真的出事）

### ① 不许用假数据冒充结果

演示/汇报/截图里任何数字必须来自真实链路。`frame_id % N` 造偏移、硬编码置信度、
空检测补框 —— 这类代码一次都不行。**本项目踩过两次。**

### ② 只在板子上原生编译

交叉编译会撞 glibc（Ubuntu22 的 2.35 vs 板子 Debian11 的 2.31）。

### ③ 板子 IP 与密码不入库

仓库是 **public**。2026-10 曾因 `AGENTS.md` / `CURRENT_STATUS.md` 里写着板子 root 密码
而做过两次 `git filter-branch`。**写文档用 `<board-ip>` 占位。**

### ④ 别用 PowerShell 读写含中文的 UTF-8 源文件

`Set-Content -Encoding UTF8` / `Out-File` 会加 BOM，中文会坏、commit message 会乱码。
用编辑器改，或脚本里一律 `UTF8Encoding($false)`。

### ⑤ Windows 侧别用复杂内联 ssh

`ssh root@<ip> 'a | b; c'` 的引号会被 PowerShell 吃掉 → **写成 `.sh` 脚本 scp 上去再 `bash`**。

### ⑥ 资源互斥：这些同时开会打架

| 抢什么 | 后果 | 现状 |
| :--- | :--- | :--- |
| NPU IOMMU 4 GiB | 第 2 个 rkllm 必 `errno 14` | ✅ `voice-gateway.service` 有 `Conflicts=edgevision.service` |
| `/dev/video81` | 8080 预览 × 9000 网关花屏 | ⚠️ 靠人工记忆 |
| `/dev/video83` | stereo3d 右目 × 看板 `--cam` | ✅ 2026-11-02 已去掉了看板的 `--cam` |
| 麦克风 `hw:0,0` | 后来者 `exit 255` 无限重启 | ✅ unit 里 `fuser -k -9` 兜底 |

> **能同时跑**：8090 看板（mp4 模式）+ 9000 + 8080；巡检守护 + 9000；8088 + 9000；
> `tts_daemon` + 任何（它不碰 ALSA）。
> **有代价**：语音网关 × 9000 同跑时 Agent 单轮 **10 s → 39 s**。

---

## 五、协作偏好（全文见总纲第 7 节）

说中文；简洁直接可操作；用户是学生、嵌入式新手，解释用**大白话+类比**；
**学习场景让用户自己敲代码、AI 当私教陪同排错**，不要直接替他把代码全写好；
校招项目要**真实、可量化、能讲清**，警惕吹嘘的数字；买硬件给明确型号+价格+接口。

---

## 六、改完实质工作要同步的文档

1. `docs/CURRENT_STATUS.md` —— 现在能干什么
2. 根目录 `AGENTS.md` 第 3 节 —— 一条带证据的 ✅
3. `docs/BENCHMARKS.md` —— 数字 + 口径

`TROUBLESHOOTING.md` 只记「面试能讲」的问题，过三问才写全文：
根因能讲底层原理？排查有方法论？有可量化证据？
