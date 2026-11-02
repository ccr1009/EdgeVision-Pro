# lab/ — 项目源码目录

**这里是唯一的源码目录。** 板子上的同名文件都是部署副本，改板子上的不算数。

> ⚠️ **目录名叫 `lab`，但放的是生产线代码，不是练习。**
>
> `lab` 是第 1 周四开的一个名字，后来 9000 网关、Agent、语音、巡检守护
> 四个**正在开机自启的服务**都长了进来，名字就名不副实了。
> **没有改名**，原因写在第七节：板端 `/root/lab/` 是两个 systemd unit 的
> `ExecStart` 路径，仓库改名会和板子对不上，风险大于收益。
>
> 第 1~4 周的**真·练习代码**在 [`../learn/`](../learn/)，那里才是"learning lab"。

> 2026-11-02 重排：原先 72 个文件全部平铺在 `lab/` 根下，找东西靠翻。
> 现在按职责分成 8 个子目录，**顶层只剩这份 README**。

---

## 一、按职责找东西

| 我要改… | 去这里 |
| :--- | :--- |
| Agent 的工具、prompt、护栏 | [`agent/`](agent/) |
| 语音识别/合成/唤醒 | [`voice/`](voice/) |
| 9000 网关、8080 预览 | [`vision/`](vision/) |
| 回归测试、探针、客户端 | [`tests/`](tests/) |
| systemd unit 文件 | [`services/`](services/) |
| 编译脚本、一键上线 | [`build/`](build/) |
| 历史实验草稿（不要改） | [`drafts/`](drafts/) |
| 攻坚工具源码 | [`tools/`](tools/) |
| 一次性调试脚本 | [`scripts/`](scripts/) |

---

## 二、六个目录的详细内容

### `agent/` — 边缘智能体（3 个源文件 + 3 个诊断脚本）

| 文件 | 是什么 | 部署到板上 |
| :--- | :--- | :--- |
| `rkllm_embodied_agent.cpp` | **Agent 主体**：7 个硬件工具、sys_prompt、快速通道、护栏 | `/root/rkllm_deploy/rkllm_embodied_agent.cpp` |
| `edgevision_patrol_daemon.cpp` | **巡检守护快层**：5 s 周期采温度/NPU/视觉帧，迟滞状态机控 LED，**刻意不加载 LLM** | `/root/lab/edgevision_patrol_daemon` |
| `edgevision_agent_server.cpp` | 8088 HTTP/JSON RPC 控制面 | 单独 `g++`，见文件头注释 |
| `diag_patrol_launch.sh` | 守护启动失败诊断 | `/root/` |
| `verify_state.sh` | 验证运行态快照是否真实 | `/root/` |
| `probe_led_hardware.sh` | 探针：LED heartbeat 是否真的出光 | `/root/` |

> **这 7 个工具**（`lab/agent/rkllm_embodied_agent.cpp` 的 schema 里数得出来）：
> `get_system_health` / `get_led_status` / `control_led` / `get_vision_telemetry` /
> `get_patrol_events` / `get_patrol_status` / `control_service`

### `voice/` — 语音交互

| 文件 | 是什么 |
| :--- | :--- |
| `voice_gateway.cpp` | **语音编排**：ASR 子进程、软唤醒、半双工、TTS 分段播报、fork Agent |
| `tts_daemon.cpp` | **TTS 常驻守护**（纯 CPU 推理，不碰 ALSA/RKNN），首段合成 1 s 靠它 |
| `voice_start.sh` | 启停脚本（含 `Speaker on`、`fuser -k` 清麦克风） |

### `vision/` — 视觉底座（**全项目的脊柱**）

| 文件 | 是什么 |
| :--- | :--- |
| `edgevision_gateway.cpp` | **9000 TCP 定长帧推流网关**（248 B/帧），`edgevision.service` 跑的就是它 |
| `edgevision_preview_real.cpp` | 8080 MJPEG 实时预览（检测框 + 3D 棱边 + BEV 小地图） |
| `cam_stream_server.cpp` | 8080 的另一实现（裸 V4L2，学习期） |
| `patch_gateway_dflfree.py` | 把 DFL-Free 后处理打进网关的补丁（历史工具） |

> ⚠️ **9000 网关不在 `deploy/cpp` 里。** `deploy/cpp` 的 6 个 target 没有一个是网关，
> 那些只做单张 jpg / 看板。网关源码在这里，靠 `build/build_w4_gateway.sh` 手写 g++
> 编译，并且 `#include "rknn_engine_dfl_free.h"` + `-I/root/edgevision/deploy/cpp/include`，
> **源码级依赖 `deploy/cpp`**。改网关时两边都要看。
>
> **2026-11-02 已去掉 `w4_` 前缀**（原名 `w4_edgevision_gateway.cpp`）：它第 4 周写，
> 但早已是项目主线，不是练习。改名后仓库里叫 `edgevision_gateway.cpp`。
> **板端仍叫 `w4_edgevision_gateway.cpp` / 二进制 `w4_edgevision_gateway` 不变**——
> 因为 `edgevision.service` 的 `ExecStart` 和多个排查脚本的 `pgrep -f` 依赖这个名字。
> 改代码后要用 `scp lab/vision/edgevision_gateway.cpp rk3588:/root/lab/w4_edgevision_gateway.cpp`。

### `tests/` — 自测工装与客户端（13 个）

**改完代码必须跑的四个**：

| 文件 | 是什么 |
| :--- | :--- |
| `regress.py` | **回归 20 条**：守护已知行为，期望词 + 禁止词双向断言 |
| `explore.py` | **探索 15 项**：撞未知边界（注入 / 矛盾 / 越权 / 假成功） |
| `loop_check.sh` | **声学回路连通性自检**：不过就退出，堵死"没测量却产出数字" |
| `test_e2e.sh` | 语音端到端自测（板子自己说给自己听） |

**其余**：

| 文件 | 是什么 |
| :--- | :--- |
| `bench_voice.sh` | 唤醒词命中率基准 |
| `bench_params.sh` | 参数扫描（`-O2/-O3` × 上下文长度） |
| `test_kws.sh` | 唤醒词手动实测（**独占麦克风**，跑前必须停网关） |
| `agent_multistep_demo.py` | 多步演示闭环编排器（纯标准库） |
| `agent_regression_test.py` | PC 端回归测试器 |
| `bench_contention_sampler.py` | NPU 竞争实测采样器 |
| `edge_agent_client.py` | PC 端 Agent 交互客户端 |
| `pc_client_viewer.py` | PC 端 9000 推流查看器 |
| `run_agent_test.py` | 板端 Agent 测试驱动 |

### `services/` — systemd unit（5 个）

| 文件 | 装到板上 |
| :--- | :--- |
| `voice-gateway.service` | `/etc/systemd/system/`（**含 `Conflicts=edgevision.service`**） |
| `tts-daemon.service` | `/etc/systemd/system/` |
| `edgevision-dashboard.service` | `/etc/systemd/system/`（读 mp4，**不占摄像头**） |
| `edgevision-patrol.service` | `/etc/systemd/system/` |
| `edgevision-patrol.default` | `/etc/default/`（`PATROL_ARGS`） |

### `build/` — 编译与上线（6 个）

| 文件 | 干什么 |
| :--- | :--- |
| `deploy.sh` | **一条命令上线**：同步 → 板上编译 → 可选重启。只覆盖 Agent 与语音网关两个源文件 |
| `build_embodied_agent.sh` | 板上编译并启动 Agent |
| `build_patrol_daemon.sh` | 板上编译巡检守护 |
| `build_w4_gateway.sh` | 板上编译 9000 网关 |
| `build_preview.sh` | 板上编译 8080 预览 |
| `build_and_run_agent.sh` | 编译并运行 `test_tools2`（旧测试） |

> ⚠️ **`deploy.sh` 覆盖不全**：它只管 Agent 和语音网关。改网关（9000）、巡检守护、
> TTS、预览页**它不管**，得用对应的 `build_*.sh`。这是当前的真实缺口。

### `drafts/` — 历史实验草稿（31 个，**不要改**）

10-20 ~ 10-27 攻坚期的一次性脚本。**结论已固化进 `docs/`**，留着只为查历史：

- `_p0_*`（7 个）：Agent 工具层 P0 修复期，结论在 `docs/p0_smoke_evidence.md`
- `_p6_*`（3 个）：task-6 编译/冒烟
- `bench_mjpg_decode*.cpp`、`bench_cam_fps*.cpp`：MPP 硬解与摄像头帧率攻坚
- `check_*.cpp` / `find_nonzero.cpp` / `inspect_anchor.cpp` / `probe_scores.cpp` / `test_*.cpp`：INT8 量化与后处理调试
- `w4_d23_signal.cpp` / `w4_d24_epoll.cpp`：第 4 周学习练习（信号 / epoll）
- `w4_edgevision_preview.cpp`：8080 预览的旧版本
- `*.from_board`：从板子回拉的临时副本

### `tools/` — 攻坚工具源码（51 个）｜`scripts/` — 一次性脚本（26 + 15 个）

见各自目录的 README。都是从板端 `/root/lab/` 抢救回来的。

---

## 三、改了代码怎么上线

```bash
cd edgevision-rk3588/lab/build
./deploy.sh agent            # 同步 + 编译，不重启
./deploy.sh agent --restart  # 同步 + 编译 + 重启语音网关（会等 70s 就绪）
./deploy.sh gateway          # 语音网关同理
```

`deploy.sh` 会**先编译再替换**：编译失败就保留旧二进制，不会把板子弄成不能跑。
它用 `dirname "$0"` 自己定位，**从任何目录调用都行**。

手动等价步骤（脚本就是干这个）：

```bash
scp agent/rkllm_embodied_agent.cpp rk3588:/root/rkllm_deploy/agent_new.cpp
ssh rk3588 'cd /root/rkllm_deploy && tr -d "\r" < agent_new.cpp > rkllm_embodied_agent.cpp \
  && g++ -O2 rkllm_embodied_agent.cpp -L. -lrkllmrt -Wl,-rpath=. -o .new \
  && mv -f .new rkllm_embodied_agent_p0'
```

**为什么要 `tr -d "\r"`**：Windows 行尾的 `\r` 会让 g++ 报 `stray '\r'`。脚本里已处理。

---

## 四、改完必须做的两件事

```bash
python3 /root/regress.py     # 20 条已知行为，必须全绿
python3 /root/explore.py     # 15 项边界探测，看有没有新发现
```

两个脚本都会**自己停网关腾出 NPU、跑完自动拉起**，约 2~3 分钟，不需要人参与。

改动涉及语音链路时，**先跑 `loop_check.sh`**：回路不通时 `bench_voice.sh` / `test_e2e.sh`
仍会产出具体数字（0/10 + 幻觉文本），那**不是失败，是没有测量**。

---

## 五、板子上的目录（**和仓库结构不同**）

| 板上路径 | 内容 |
| :--- | :--- |
| `/root/rkllm_deploy/` | Agent 二进制 + 2.98 GB 模型 + `rkllm.h` + 运行库 |
| `/root/voice_gateway` + `.cpp` | 语音网关二进制与源码副本 |
| `/root/tts_daemon` + `.cpp` | TTS 常驻服务 |
| `/root/lab/` | 网关/守护二进制、大量历史探针（243 个文件） |
| `/root/edgevision/` | 视觉引擎 `deploy/cpp/build_release/`、`data/videos/highway_traffic.mp4` |
| `/root/asr/` | 全部模型：Paraformer、VITS、KWS、VAD、两版 runtime |
| `/root/patrol/` | `patrol_events.log` / `llm_pending.jsonl` / `patrol_state.json` |

短名入口是**软链接**，全指向正本：

```bash
bp.sh -> bench_params.sh      bv.sh -> bench_voice.sh      te.sh -> test_e2e.sh
tk.sh -> test_kws.sh          vs.sh -> voice_start.sh
```

> 2026-10-31 之前它们是实体副本，各自漂移过。`vs.sh` 停在 10-29 版，
> 缺 `amixer -c 2 cset numid=16 on`——用它启动网关**完全没声音，日志却显示播放完成**，
> 正好掉回"假成功"那个坑。改成软链接后就再也漂不了。

---

## 六、源码与部署的漂移检查

```bash
# 本地算规范化 md5（去掉 \r），板上直接算
for f in agent/rkllm_embodied_agent.cpp voice/voice_gateway.cpp tests/regress.py tests/explore.py; do
  printf "%-38s %s\n" "$f" "$(tr -d '\r' < $f | md5sum | cut -c1-8)"
done
ssh rk3588 'md5sum /root/rkllm_deploy/rkllm_embodied_agent.cpp /root/voice_gateway.cpp'
```

md5 不一致 = 有人在板上直接改了代码，或者本地改了没部署。**以本地为准**。

---

## 七、这个目录的已知问题

1. **`lab/` 这名字名不副实**：9000 网关、Agent、语音、巡检守护四个**在跑的服务**都在这里，
   而 `deploy/cpp`（听名字才是"正式代码"）反而不含网关。**暂时不改名**——`lab/` 在全仓被
   引用 116 次，改名收益不抵成本。
2. ~~**`w4_` 前缀会误导**~~ **已在 2026-11-02 修掉**：`w4_edgevision_gateway.cpp` →
   `edgevision_gateway.cpp`，`w4_edgevision_preview_real.cpp` → `edgevision_preview_real.cpp`。
   板端文件名保持 `w4_` 不变（unit 与 pgrep 依赖）。
   注意 `drafts/` 里还有 `w4_d23_signal.cpp`、`w4_d24_epoll.cpp`、`w4_edgevision_preview.cpp`
   ——**那几个是真的第 4 周练习与旧版本**，前缀是对的。
3. **`deploy.sh` 覆盖不全**：只同步 Agent 与语音网关，其余各有自己的 build 脚本。
4. **248 B 帧协议常量重复 3 份**：`agent/rkllm_embodied_agent.cpp`（约 539~554 行）、
   `esp32_gimbal/host/gimbal_control.py`（16~22 行）、`agent/edgevision_patrol_daemon.cpp`
   （约 275~281 行）各写了一遍，没有共享头文件。改协议要同时改三处。
