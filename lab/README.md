# lab/ 是什么

**这里是唯一的源码目录。** 板子上的同名文件都是部署副本，改板子上的不算数。

目录里有 **78 个文件**（顶层 72 + `ipc/` 2 + `agent_test_results/` 4），但**不是只有十几个在用**。
按「改它之前要不要先问清楚」分成四类，**看下面这张表的分类，别看文件名**。

> ⚠️ **上一版这里有错，已修正（2026-11-02）**：
> 旧版说「73 个文件，当前在用的只有 11 个，其余是实验草稿不要改」。
> 实测文件数是 78 不是 73；更严重的是**那 11 个"活文件"里有 6 个全仓零引用**，
> 而巡检守护 `edgevision_patrol_daemon.cpp`（文档引用 7 次）、
> 多步演示 `agent_multistep_demo.py`（引用 8 次）、`tts_daemon.cpp`、
> `edgevision_agent_server.cpp` 这些**一等公民**却被划进了"草稿不要改"。
> 这个划分会误导人改错文件，所以重写了。

---

## 一、正在跑的东西（改之前先看文档）

### 1.1 视觉底座（全项目的脊柱：9000 端口）

| 文件 | 是什么 | 部署到板上 |
| :--- | :--- | :--- |
| `w4_edgevision_gateway.cpp` | **9000 TCP 定长帧推流网关**（248 B/帧），`edgevision.service` 跑的就是它 | 裸 g++ 编译，见 `build_w4_gateway.sh` |
| `w4_edgevision_preview_real.cpp` | 8080 MJPEG 实时预览（检测框 + 3D 棱边 + BEV 小地图） | 见 `build_preview.sh` |
| `patch_gateway_dflfree.py` | 把 DFL-Free 后处理打进网关的补丁脚本（历史工具） | — |

> **9000 网关是全项目的脊柱**。Agent、巡检守护、ESP32 云台、PC 客户端全部连它。
> 但它**不在 `deploy/cpp` 里**——`deploy/cpp` 的 6 个 target 没有一个是网关，
> 那些只做单张 jpg / 看板。网关的源码就在这里，靠 `build_w4_gateway.sh` 手写 g++ 编译，
> 并且 `#include "rknn_engine_dfl_free.h"` + `-I/root/edgevision/deploy/cpp/include`，
> **源码级依赖 `deploy/cpp`**。改网关时两边都要看。

### 1.2 边缘智能体 + 语音（CI 在 `deploy.sh` 里）

| 文件 | 是什么 | 部署到板上 |
| :--- | :--- | :--- |
| `rkllm_embodied_agent.cpp` | **Agent 主体**：7 个硬件工具、sys_prompt、快速通道、护栏 | `/root/rkllm_deploy/rkllm_embodied_agent.cpp` |
| `voice_gateway.cpp` | **语音编排**：ASR 子进程、软唤醒、半双工、TTS 分段播报、fork Agent | `/root/voice_gateway.cpp` |
| `tts_daemon.cpp` | **TTS 常驻守护**（纯 CPU 推理，不碰 ALSA/RKNN），首段合成 1 s 靠它 | `/root/tts_daemon.cpp` |
| `voice_start.sh` | 语音网关启停脚本（含 `Speaker on`、`fuser -k` 清麦克风） | `/root/voice_start.sh` |

### 1.3 巡检守护（快慢双系统里的「快层」）

| 文件 | 是什么 | 部署到板上 |
| :--- | :--- | :--- |
| `edgevision_patrol_daemon.cpp` | 5 s 周期采温度/NPU/视觉帧，迟滞状态机控 LED；**刻意不加载 LLM**避让 NPU | 见 `build_patrol_daemon.sh` |
| `edgevision-patrol.service` / `.default` | systemd unit 与默认参数（`PATROL_ARGS`） | `/etc/systemd/system/` + `/etc/default/` |
| `verify_state.sh` | 验证「运行态快照」是否真实反映守护状态 | `/root/` |
| `diag_patrol_launch.sh` | 守护启动失败时的诊断脚本 | `/root/` |

### 1.4 控制面与 systemd

| 文件 | 是什么 | 部署到板上 |
| :--- | :--- | :--- |
| `edgevision_agent_server.cpp` | 8088 HTTP/JSON RPC 控制面（7 温区 / LED / 视觉） | 单独 `g++`，见文件头注释 |
| `voice-gateway.service` | 语音网关 unit，**含 `Conflicts=edgevision.service`** | `/etc/systemd/system/` |
| `tts-daemon.service` | TTS 常驻 unit | `/etc/systemd/system/` |
| `edgevision-dashboard.service` | 8090 看板 unit（读 mp4，**不占摄像头**） | `/etc/systemd/system/` |

### 1.5 自测工装（改完代码必须跑）

| 文件 | 是什么 | 部署到板上 |
| :--- | :--- | :--- |
| `regress.py` | **回归 20 条**：守护已知行为，期望词 + 禁止词双向断言 | `/root/regress.py` |
| `explore.py` | **探索 15 项**：撞未知边界（注入 / 矛盾 / 越权 / 假成功） | `/root/explore.py` |
| `loop_check.sh` | **声学回路连通性自检**：播录音看空气里有没有能量台阶；不过就退出 | `/root/loop_check.sh` |
| `test_e2e.sh` | 语音端到端自测（靠声学回路，板子自己说给自己听） | `/root/test_e2e.sh` |
| `bench_voice.sh` | 唤醒词命中率基准 | `/root/bench_voice.sh` |
| `bench_params.sh` | 参数扫描（`-O2/-O3` × 上下文长度） | `/root/bench_params.sh` |
| `test_kws.sh` | 唤醒词手动实测（**独占麦克风**，跑前必须停网关） | `/root/test_kws.sh` |

### 1.6 客户端 / PC 侧

| 文件 | 是什么 |
| :--- | :--- |
| `agent_multistep_demo.py` | 多步演示闭环编排器（纯标准库，文档引用 8 次） |
| `agent_regression_test.py` | PC 端回归测试器（纯标准库） |
| `bench_contention_sampler.py` | NPU 竞争实测采样器 |
| `edge_agent_client.py` | PC 端 Agent 交互客户端 |
| `pc_client_viewer.py` | PC 端 9000 推流查看器 |

---

## 二、历史实验草稿（**留档，不要改**）

这些是 10-20 ~ 10-27 攻坚期的一次性脚本。**结论已经固化进 `docs/`**，留着只为查历史：

- `_p0_*`（7 个）：Agent 工具层 P0 修复期，结论在 `p0_smoke_evidence.md`
- `_p6_*`（3 个）：task-6 编译/冒烟
- `bench_mjpg_decode*.cpp`、`bench_cam_fps*.cpp`：MPP 硬解与摄像头帧率攻坚
- `check_*.cpp` / `find_nonzero.cpp` / `inspect_anchor.cpp` / `probe_scores.cpp` / `test_*.cpp`：INT8 量化与后处理调试
- `w4_d23_signal.cpp` / `w4_d24_epoll.cpp`：第 4 周学习练习（信号 / epoll）
- `_tts_resident_bench.cpp`、`test_led_duty.sh`、`probe_led_hardware.sh`：单项验证探针
- `*.from_board`：从板子回拉的临时副本（已在 `.gitignore`）

> **注意**：`lab/README.md` 上一版说「7 份文档引用了草稿」，实测被引用的草稿只有 2 个
> （`bench_cam_fps.cpp`、`bench_contention_sampler.py`），而且后者现在归在 1.6 类。
> 所以"搬走草稿要改文档"这个理由，实际只对 `bench_cam_fps.cpp` 成立。

---

## 三、板子上的目录

| 路径 | 内容 |
| :--- | :--- |
| `/root/rkllm_deploy/` | Agent 二进制 + 2.98 GB 模型 + `rkllm.h` + 运行库 |
| `/root/voice_gateway` + `.cpp` | 语音网关二进制与源码副本 |
| `/root/tts_daemon` + `.cpp` | TTS 常驻服务 |
| `/root/edgevision/` | 视觉引擎（`deploy/cpp/build_release/`）、`data/videos/highway_traffic.mp4` |
| `/root/asr/` | 全部模型：Paraformer、VITS、KWS、VAD、两版 runtime |
| `/root/patrol/` | `patrol_events.log` / `llm_pending.jsonl` / `patrol_state.json` |
| `/root/regress.py` `/root/explore.py` | 自测脚本 |

`bp.sh` `bv.sh` `te.sh` `tk.sh` `vs.sh` 是**短名入口，现在是软链接**，全指向正本：

```bash
bp.sh -> bench_params.sh      bv.sh -> bench_voice.sh      te.sh -> test_e2e.sh
tk.sh -> test_kws.sh          vs.sh -> voice_start.sh
```

> 2026-10-31 之前它们是**实体副本**，各自漂移过。其中 `vs.sh` 停在 10-29 版，
> 缺 `amixer -c 2 cset numid=16 on`——用它启动网关**完全没声音，日志却显示播放完成**，
> 正好掉回"假成功"那个坑。`tk.sh` 停在旧唤醒参数（`score=1.0/thr=0.25`，现为 `3.0/0.10`），
> 用它测唤醒率会得到偏低的错误结论。改成软链接后就再也漂不了。
> 旧副本留档在板上 `/root/archive/2026-10-31-dedup/dup-scripts/`。

对比源码时以 `/root/*.cpp` 和 `/root/*.py` 为准。

---

## 四、`/root/archive/` 什么时候看

板上 `/root/archive/` 是**移走而不是删除**的归档区，每个子目录有 `MANIFEST.md` 说明
移了什么、为什么移、怎么还原。

| 目录 | 内容 |
| :--- | :--- |
| `2026-10-31-dedup/old-agent-src/` | 4 份旧版 Agent 源码（含和正本 md5 相同的 `agent_new.cpp` 残留） |
| `2026-10-31-dedup/dup-scripts/` | 4 组 `xxx.sh`/`xxxr.sh` 重复对 + `vgw.cpp` + `.bak` + 两份过期短名 |

判断标准：**只移"多份同名、改了不生效"的东西**。

---

## 五、改了代码怎么上线

```bash
cd edgevision-rk3588/lab
./deploy.sh agent            # 同步 + 编译，不重启
./deploy.sh agent --restart  # 同步 + 编译 + 重启语音网关（会等 70s 就绪）
./deploy.sh gateway          # 语音网关同理
```

`deploy.sh` 会**先编译再替换**：编译失败就保留旧二进制，不会把板子弄成不能跑。

> ⚠️ `deploy.sh` **只管 Agent 和语音网关两个源文件**（`rkllm_embodied_agent.cpp`、
> `voice_gateway.cpp`）。改网关（9000）、巡检守护、TTS、预览页**它不管**，
> 得用对应的 `build_*.sh`。这是当前的一个真实缺口。

手动等价步骤（脚本就是干这个）：

```bash
scp rkllm_embodied_agent.cpp rk3588:/root/rkllm_deploy/agent_new.cpp
ssh rk3588 'cd /root/rkllm_deploy && tr -d "\r" < agent_new.cpp > rkllm_embodied_agent.cpp \
  && g++ -O2 rkllm_embodied_agent.cpp -L. -lrkllmrt -Wl,-rpath=. -o .new \
  && mv -f .new rkllm_embodied_agent_p0'
```

**为什么要 `tr -d "\r"`**：Windows 行尾的 `\r` 会让 g++ 报 `stray '\r'`。脚本里已处理。

---

## 六、改完必须做的两件事

```bash
python3 /root/regress.py     # 20 条已知行为，必须全绿
python3 /root/explore.py     # 15 项边界探测，看有没有新发现
```

两个脚本都会**自己停网关腾出 NPU、跑完自动拉起**，全程约 2~3 分钟，不需要人参与。

改动涉及语音链路时，**先跑 `loop_check.sh`**：回路不通时 `bench_voice.sh` / `test_e2e.sh`
仍会产出具体数字（0/10 + 幻觉文本），那**不是失败，是没有测量**。

---

## 七、源码与部署的漂移检查

```bash
# 本地算规范化 md5（去掉 \r），板上直接算
for f in rkllm_embodied_agent.cpp voice_gateway.cpp regress.py explore.py; do
  printf "%-28s %s\n" "$f" "$(tr -d '\r' < $f | md5sum | cut -c1-8)"
done
ssh rk3588 'md5sum /root/rkllm_deploy/rkllm_embodied_agent.cpp /root/voice_gateway.cpp'
```

md5 不一致 = 有人在板上直接改了代码，或者本地改了没部署。**以本地为准**。

---

## 八、这个目录的已知问题（2026-11-02 记录）

1. **`lab/` 这名字名不副实**：9000 网关、Agent、语音、巡检守护四个**在跑的服务**都在这里，
   而 `deploy/cpp`（听名字才是"正式代码"）反而不含网关。目录名会误导人。
   若要重排，`lab/` 在全仓被 25 个文档按路径引用 **122 次**，成本最高，放最后做。
2. **`deploy.sh` 覆盖不全**：只同步 Agent 与语音网关，网关/守护/TTS/预览各有自己的 build 脚本。
3. **248 B 帧协议常量重复 3 份**：`rkllm_embodied_agent.cpp`（约 539~554 行）、
   `esp32_gimbal/host/gimbal_control.py`（16~22 行）、`edgevision_patrol_daemon.cpp`（约 276~281 行）
   各写了一遍，没有共享头文件。改协议要同时改三处。
