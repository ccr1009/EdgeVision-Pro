# NPUD 推理服务化 —— 实施计划

> 目标读者：负责实施的自己（含 AI 助手）。
> 立项动机：**为 Linux 应用开发实习服务**，不是为算法指标服务。
> 端侧部署推理（RKNN/RKLLM）作为加分项保留，但不作为本计划的主线。

---

## 1. 要解决的问题

### 1.1 Phase 0 实测推翻了原立项前提（2026-10-29）

**原以为的问题**：`run_agent.sh` 注释断言「edgevision.service 常驻时，再起 rkllm 就会报
`failed to malloc npu memory`」，因此启动 Agent 必须先停网关和看板。

**实测结论：这个前提是错的。** 三轮复现（gateway + dashboard 全程不停，只清残留 rkllm）：

| 轮次 | 操作 | 结果 |
|---|---|---|
| 1 | 起第 1 个 rkllm | **OK（4s）**，真实推理触发 `get_system_health`，9000 推流 14632 B/2s（mod 248 = 0），NPU 分配失败 0 次 |
| 2 | 复现 | **OK（3s）**，同上，13888 B/2s |
| 3 | 保留第 1 个，再起第 2 个 | **FAIL（5s）**：`used: 3915 MiB` + `196.3 MiB` = 4111 > 4096 |

**结论：gateway + dashboard + 1 个 rkllm 三者完全共存，功能全部正常。**
失败只发生在「第 2 个 rkllm」，而 `TROUBLESHOOTING` 第 27 条的三配置表其实早就写着
「只开 gateway：**1 个 OK**，第 2 个 FAIL」——**「最多 1 个」指的是「能起 1 个」，不是「一个都起不来」**。

`run_agent.sh` 把「第 2 个实例失败」误读成了「不能共存」，属**过度归因**。
真正需要清理的是**残留的 rkllm 实例**，不是 gateway。

> 方法论收获（比代码值钱）：注释和脚本是某个人在某次故障后的推断，不是事实。
> 判断「能不能共存」要去读当时那张「最多能起几个」的表，而不是读脚本里那句因果总结。

### 1.2 那 npud 还做不做？——做，但动机改写

既然共存没问题，原先「消除必须先停服务」的卖点不成立。仍然成立的技术问题：

| 问题 | 证据 |
|---|---|
| 同一份 4.3 MB 模型被 **4 次** `rknn_init`（gateway 3 worker + dashboard 1） | `npu_worker_thread` 每线程一个 `RKNNEngineDflFree` |
| 4 个 context **争抢 3 个物理核**，调度不可控 | 共存时 `rknpu/load`：Core0 47% / Core1 14% / Core2 14% |
| 无法做优先级：想让 Agent 优先，现在没有任何手段 | 三个进程平等抢 NPU |
| IOVA 余量被无谓消耗，第 2 个 rkllm 永远起不来 | `used: 3915 MiB` 里含重复 context |

**新的、诚实的立项理由**：把「多进程各自加载同一模型、各自绑核」收敛为「单一 NPU 服务统一排队」，
让调度可控、IOVA 占用可见。这是一个**架构收敛**项目，不是一个**救火**项目。


---

## 2. 不做什么（明确排除）

| 不做 | 原因 |
|---|---|
| 相机标定 / 3D 人脸识别 | 对 Linux 应用岗零加分；后者技术路径不成立 |
| 新增模型 / 提升 mAP | NPU 已挤爆，再上模型只会更糟 |
| 把 RKLLM 也搬进 npud | 二期以后再说。一期只收敛 RKNN，收益已足够 |
| 重写推理引擎 | `RKNNEngineDflFree` 直接用，不动 |

---

## 3. 现状代码边界（已核实）

| 位置 | 内容 | 在改造中的角色 |
|---|---|---|
| `deploy/cpp/include/rknn_engine_dfl_free.h` | `init / init_from_memory / run_inference(rgb,w,h,vector<Detection>&)` | **npud 唯一需要封装的接口** |
| `deploy/cpp/src/rknn_engine_dfl_free.cpp` | 实现 + 3 核绑定 | 原样拷进 npud |
| `lab/vision/w4_edgevision_gateway.cpp` | 模型预加载到内存、3 worker 各建 context、有界队列、定长 248B TCP 帧 | 客户端改造对象 |
| `deploy/cpp/src/edgevision_dashboard_main.cpp` | `RKNNEngineDflFree npu` 单实例 | 客户端改造对象 |
| `data/models/yolov8n_dflfree_rk3588_i8.rknn` | 同一个模型，两处重复加载 | 收敛到 npud 只加载一次 |

**关键结论：推理接口已经统一，不需要重构算法层。** 改造是纯系统层工作。

---

## 4. 分阶段计划

### Phase 0 · 测量基线（✅ 已完成 2026-10-29）

**产出**（脚本：`_tools/phase0c_repro.sh`，板上日志 `/tmp/p0c_out.log`）：

| 项 | 实测值 |
|---|---|
| gateway + dashboard + 1 个 rkllm | **可共存**，两轮复现，推理正常，推流正常 |
| rkllm init 耗时 | **3~4 s**（模型在 page cache）；冷启动首次读盘 2.98 GB 约 60~90 s |
| 三者共存时 NPU load | Core0 47% / Core1 14% / Core2 14% |
| 第 2 个 rkllm | **5 s 内失败**，`used: 3915 MiB` + `196.3 MiB` > 4096 |
| 共存时 9000 推流 | 13888~14880 B/2s，恒为 248 的整数倍 |
| 共存时 NPU 分配失败 | **0 次** |

**方法上的一个失败教训**：最初想用 `/sys/kernel/debug/dma_buf/bufinfo` 按 attached device 统计 IOVA，
结果 RKNN 走 rknpu 自己的 GEM 分配、**不经过 dma_buf 框架**，bufinfo 里看不到任何 NPU buffer。
有效路径只有「让驱动在分配失败时自己打印 `used:`」——即 `probe_iommu_capacity.sh` 的探针法。

**Phase 0 判定**：原立项前提证伪，动机改写为「架构收敛」（见 1.2），**改造成立，继续 Phase 1**。

---

### Phase 1 · npud 最小可用（2~3 天）

**目标**：让网关从"自己加载模型"变成"向 npud 要结果"。**先跑通，不优化。**

#### 1.1 接口设计（`deploy/cpp/include/npud.h`）

公共头，daemon 和客户端都 include。

```c
#define NPUD_SOCK_PATH  "/run/edgevision/npud.sock"
#define NPUD_SHM_NAME   "/edgevision_frames"
#define NPUD_MAGIC      0x4455504E   /* 'NPUD' */

/* 单帧最大尺寸：640x640x3 */
#define NPUD_FRAME_BYTES (640 * 640 * 3)
#define NPUD_SLOTS       8           /* 每客户端独占一个 slot */

enum npud_op { NPUD_OP_PING = 0, NPUD_OP_DETECT = 1, NPUD_OP_STATS = 2 };

/* 定长请求头 32B */
struct npud_req {
    uint32_t magic;        /* NPUD_MAGIC */
    uint16_t version;      /* 1 */
    uint16_t op;           /* enum npud_op */
    uint32_t seq;          /* 响应回填 */
    uint32_t slot;         /* 客户端分配的 slot 索引 */
    uint32_t img_w, img_h;
    uint32_t deadline_ms;  /* 0 = 不限 */
    uint32_t pad;
    uint64_t reserved;
};

struct npud_det { float x1, y1, x2, y2, score; int32_t cls; };

/* 响应：固定头 + 变长检测数组 */
struct npud_resp {
    uint32_t magic;
    uint32_t seq;
    int32_t  status;       /* 0 或 -errno */
    uint32_t ndet;
    float    infer_ms;
    float    decode_ms;
    uint32_t slot;
    uint32_t pad;
    struct npud_det dets[];   /* 变长 */
};
```

**为什么 `SOCK_SEQPACKET`**：保留消息边界，客户端不用自己做粘包/半包处理。
代价是与 `SOCK_STREAM` 在 `sendmsg` 上有细节差异 —— 这一点要在实现时确认（属于 Phase 1 的技术风险）。

#### 1.2 连接与 slot 分配

- 客户端 `connect` 后先发 `NPUD_OP_PING`，daemon 回 `{status:0, slot:N}` 分配独占 slot
- daemon 用 `SO_PEERCRED` 取 `uid`，非 root 一律拒绝
- slot 用满 → 返回 `-ENOSPC`，客户端退化为直连模式（**保留旧路径**，见第 6 节）

#### 1.3 daemon 骨架

```
main()
 ├─ parse args (--model / --workers / --sock / --shm-slots)
 ├─ single-instance lock  (/run/edgevision/npud.pid, flock)
 ├─ load model into memory once
 ├─ create N workers, each RKNNEngineDflFree::init_from_memory(core=i)
 ├─ shm_open + ftruncate + mmap
 ├─ epoll loop on listen_fd + client fds
 └─ SIGTERM → 排空队列 → release() → 删 shm → exit 0
```

**SDK 约束**：`RKNNEngineDflFree` 不是线程安全的（每个实例一个 context）。
worker 之间必须一实例一线程，或者一实例多线程 + 外部加锁（会串行化，不可取）。
→ **采取：每 worker 一个实例，绑不同物理核，用 pthread 各自服务分到的请求。**

#### 1.4 网关改造

把 `npu_worker_thread` 里的本地推理换成 npud 客户端调用。
**保留原有有界队列和 248B 定长帧输出**——那部分不动，它是作品集里已有的素材。

#### 1.5 验收

- [ ] `npud --help` 正常，`systemctl stop` 后 socket 文件被清理
- [ ] 网关单独跑，检测结果与改造前**逐帧一致**（同一段 mp4，比对检测框坐标，容差 0）
- [ ] 网关 + 看板同时跑，都不再各自 `rknn_init`（`lsof` 确认只有 npud 打开 `.rknn`）
- [ ] `kill -9 npud`，网关于 1s 内返回 `-ECONNREFUSED` 并打印告警，不崩溃
- [ ] `strace -f -e trace=network` 能看到完整的请求-响应往返

**这一阶段做完，简历就能写第一条了。**

---

### Phase 2 · 共享内存零拷贝（3~5 天）

**目标**：把 1.2 MB 的帧数据从 socket 拷出来，改成共享内存直接读写。

#### 2.1 布局

```
/edgevision_frames  (shm_open 命名对象)
├─ header: { magic, nslots, slot_bytes, version, owner[nslots] }
└─ slot[i]: { atomic state, uint32 seq, uint32 w,h, uint8 rgb[1228800] }
```

slot 状态机：`FREE(0) → FILLED(1) → BUSY(2) → FREE(0)`

- 客户端写：等 `state==FREE` → 写像素 → `__atomic_store_n(&state, FILLED, RELEASE)`
- npud 读：`__atomic_load_n(&state, ACQUIRE)` 得 FILLED → 置 BUSY → 推理 → 置 FREE

**每 slot 独占给一个客户端 → SPSC 无锁，不需要信号量。** 这是选择"每客户端一个 slot"而非"全局共享队列"的唯一理由。

#### 2.2 崩溃一致性

客户端在 `FILLED` 之后、npud 取走之前被 `kill -9` → slot 永久卡在 FILLED。
对策：在 header 里记 `owner_pid` + `last_seq`，npud 定期检查 `kill(pid,0)`，owner 已死则强置 FREE。

**这一条是必须实现的**，否则就是给自己埋一个必现的泄漏。

#### 2.3 验收

- [ ] IPC 往返延迟 p50 / p99（`clock_gettime(CLOCK_MONOTONIC)` 包住一次 detect 调用，1000 次采样）
- [ ] 与 Phase 1 的 socket 版本对比，给出 before/after 表
- [ ] 端到端 FPS 不低于改造前（看板 24~27 fps 基线要守住）
- [ ] 故意 `kill -9` 客户端，确认 npud 能回收 slot（`cat /dev/shm` 或 stats 接口验证）
- [ ] `perf record` 确认 `memcpy` 从热点里消失

---

### Phase 3 · systemd 工程化（2 天）

#### 3.1 单元

`/etc/systemd/system/npud.service`：

```ini
[Unit]
Description=EdgeVision NPU inference daemon
After=network.target
Before=edgevision.service edgevision-dashboard.service

[Service]
Type=notify
ExecStart=/root/edgevision/npud --model /root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn
Restart=always
RestartSec=2
WatchdogSec=10
NotifyAccess=main
MemoryMax=800M
CPUQuota=200%
RuntimeDirectory=edgevision
RuntimeDirectoryMode=0755

[Install]
WantedBy=multi-user.target
```

- `Type=notify` + `sd_notify(READY=1)` + 每 5s `WATCHDOG=1`
- `RuntimeDirectory=edgevision` → systemd 自动创建/清理 `/run/edgevision`，socket 路径不再需要手工 mkdir
- 客户端用 `Requires=npud.service` + `After=`，npud 重启时客户端因连接断开而重连

#### 3.2 验收

- [ ] `systemctl restart npud` 期间客户端不退出，恢复后自动重连（日志可见 reconnect）
- [ ] 把 `WatchdogSec` 临时改 2s，人为在 daemon 里 `sleep(5)`，systemd 能 kill 并重启
- [ ] `systemctl show npud -p MemoryMax` 确认限制生效；触发超限时被 OOM kill 但 systemd 拉回
- [ ] `journalctl -u npud -o json` 能按字段筛选（结构化日志）
- [ ] 开机自启后 30s 内 `:9000`、`:8090`、`:8088` 全部可用

---

## 5. 总体验收（做完才能写进简历）

| 指标 | 测量方式 | 目标 |
|---|---|---|
| rknn_init 次数 | `strace -f -e trace=openat` 数 `.rknn` 打开次数 | 从 **4** 降到 **1**（或 N=worker 数） |
| 核占用可控性 | 共存时 `rknpu/load` 三核分布 | 从 47/14/14 变为可按优先级调度 |
| 既有功能不退化 | 9000 推流 / 8090 fps / Agent 推理 | 全部与改造前一致 |
| IPC 往返延迟 | 1000 次采样 | p99 < 5 ms（Phase 2 后） |
| 崩溃隔离 | `kill -9` 各进程逐一测 | 其余功能不中断 |
| 代码量 | `wc -l` | 新增 ≤ 1500 行（含注释） |

---

## 6. 风险与回退

| 风险 | 概率 | 对策 |
|---|---|---|
| `SOCK_SEQPACKET` 在大消息上行为不符合预期（缓冲区上限） | 中 | Phase 1 就写成可切换 `SOCK_STREAM` + 长度前缀，用一个宏切换 |
| ~~Phase 0 测出原前提不成立~~ | **已发生** | 动机改写为「架构收敛」，改造照做，技能价值不变；见 1.2 |
| 多 worker 共享 shm 时的缓存一致性 | 低 | 每 slot 独占 + acquire/release 语义，不共享写 |
| 板上编译慢、调试往返成本高 | 高 | 每个 Phase 都在板上留一个 `_p*.sh` 一键重建脚本（沿用现有 `_p6_build.sh` 风格） |
| 改坏了原演示，面试前拿不出东西 | 中 | **保留旧二进制** `edgevision_preview_old`，Phase 1 不删老代码路径，用 `--no-npud` 开关回退 |

**回退总闸**：所有客户端保留 `EDGEVISION_NO_NPUD=1` 环境变量走旧路径。
在 Phase 3 验收全绿之前，不删旧路径。

---

## 7. 时间预算

| 阶段 | 天数 | 状态 |
|---|---|---|
| Phase 0 测量 | 0.5 ~ 1 | ✅ 完成（半天） |
| Phase 1 npud 骨架 | 2 ~ 3 | 待开始 |
| Phase 2 共享内存 | 3 ~ 5 | |
| Phase 3 systemd | 2 | |
| **合计** | **7.5 ~ 11 天** | |

若时间紧，**Phase 0 + Phase 1 就够写进简历**（"把 3 个各自加载模型的进程收敛为单一 NPU 服务"），
Phase 2/3 作为加分。不要让计划变成负担。

---

## 8. 文档同步义务

每完成一个 Phase，按项目约定同步三处：

1. `docs/CURRENT_STATUS.md` —— 新增一节，写清"现在能干什么"
2. 根 `AGENTS.md` 第 3 节 —— 加一条带证据的 ✅
3. `docs/BENCHMARKS.md` —— 数字 + 测量口径（IPC 延迟必须写清采样方式和机器状态）

新增踩坑按三问规则决定是否进 `TROUBLESHOOTING.md`。

---

## 9. 简历表述（预写，做完后按实测数字填）

> 将 gateway / dashboard 中各自 `rknn_init` 的重复推理收敛为单一 NPU 推理守护进程（`npud`），
> 通过 UNIX socket + POSIX 共享内存环分发任务，把「4 个 context 争抢 3 个物理核」变为统一排队；
> 实现 SO_PEERCRED 鉴权、slot 泄漏自愈、systemd `Type=notify` + Watchdog 自治；
> IPC 往返延迟 p50 __ μs / p99 __ μs，推流与看板 __ fps 不退化。
>
> **配套的故事（比代码更值钱）**：排查 NPU IOMMU 4 GiB 约束时发现既有启动脚本
> 把「第 2 个实例失败」误读为「不能与网关共存」，实测三轮证明三层可共存、失败边界在第 2 个实例；
> 修正了错误归因，并把 `used: 3915 MiB + 196.3 MiB > 4096 MiB` 作为判据固化进文档。
