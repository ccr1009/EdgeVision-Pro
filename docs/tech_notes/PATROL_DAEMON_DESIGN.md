# PATROL_DAEMON_DESIGN.md — 快慢双系统巡检守护·快层设计

> 交付物: `lab/edgevision_patrol_daemon.cpp`(快层本体) + `lab/build_patrol_daemon.sh`(板上编译脚本)
> 板型: EVM3588-B (RK3588, 8GB, Debian 11, kernel 5.10.110)
> 铁律: **快层绝不加载 RKLLM / 任何模型**, 不链接 rknn_api —— 它与 9000 推流网关
> (`edgevision.service`, 7×24, 任何情况下禁止 stop)共存, 是轻量规则反射进程。

---

## 1. 架构图 (ASCII)

```
                     ┌──────────────────────────────────────────────────┐
                     │              RK3588 (EVM3588-B, 板上)             │
                     │                                                  │
  UVC 摄像头 ──────▶ │ ┌───────────────────────────────┐                │
  /dev/video81       │ │ edgevision.service  (7×24)    │                │
                     │ │  letterbox→NPU 3核推理→       │                │
                     │ │  TCP :9000 推 248B 定长帧      │◀── 禁止 stop!  │
                     │ └──────────────┬────────────────┘                │
                     │                │ TCP(即连即收一帧即断)            │
                     │ ┌──────────────▼────────────────┐   采样 5s/次    │
                     │ │  edgevision_patrol_daemon     │◀── /sys/class/ │
  zone0 温度 ◀────── │ │  〔快层: 规则反射〕            │     thermal/0  │
                     │ │  ① 采样: 温度/NPU负载/视觉帧   │◀── /sys/kernel │
  NPU 三核负载 ◀──── │ │  ② 状态机 + 迟滞               │     /debug/rknpu│
                     │ │  ③ LED 决策与写入              │                │
  板载 LED work ◀─── │ │  ④ 黑匣子 + 慢层事件队列        │                │
                     │ └───────┬───────────────┬───────┘                │
                     │         │               │                        │
                     │  patrol_events.log   llm_pending.jsonl            │
                     │  (黑匣子,>5MB轮转)   (慢层消费队列, JSONL 追加)      │
                     └─────────────────┬────────────────────────────────┘
                                       │ (预留, 未来)
                     ┌─────────────────▼────────────────────────────────┐
                     │  慢层 LLM 研判 (RKLLM / Qwen 端侧大模型, 未来)      │
                     │  tail -F /root/patrol/llm_pending.jsonl           │
                     │  → 事件去重/降噪/语义研判/巡检报告                  │
                     └──────────────────────────────────────────────────┘
```

数据流一句话: **传感器 → 规则状态机 → LED + 日志**; 日志中的 `llm_pending.jsonl`
是「快→慢」的单向事件出口, 慢层只读不写。

## 2. 快慢双系统分层

| 层 | 进程 | 触发 | 延迟 | 职责 | 依赖 |
|---|---|---|---|---|---|
| **快层(本交付)** | `edgevision_patrol_daemon` | 5s 周期 | 毫秒级规则判定 | 采样、迟滞状态机、LED 反射、事件落盘 | 仅 std+pthread, **不加载模型** |
| **慢层(未来)** | RKLLM 具身智能体(已有 `rkllm_embodied_agent.cpp`) | 事件驱动(消费 pending 队列) | 秒级~十秒级 | 事件语义研判、降噪合并、巡检报告、复杂决策 | Qwen 端侧大模型 + NPU |

快层回答「**现在要不要闪警灯**」(毫秒级必须给答案); 慢层回答「**刚才发生了什么、
值不值得上报、下一步巡检建议**」(可以慢, 但要理解语义)。两层通过
`llm_pending.jsonl` 解耦: 快层只管追加, 慢层只管消费, 互不阻塞。

### llm_pending.jsonl 契约 (慢层消费接口, 冻结字段)

每行一个独立 JSON 对象, 仅 3 种 `type`, 快层追加写、慢层 `tail -F` 消费:

```json
{"ts":"2026-10-24 21:05:12","type":"PERSON_ALERT","summary":"画面连续2周期检出人员(最高置信度0.81, 帧3604), LED切换为报警快闪","raw":{"vision":"online","vision_reason":"online","temp_c":43.50,"frame_id":3604,"bbox_count":1,"person_seen":true,"best_person_score":0.81,"npu_core_load":[12,0,8],"action":"set_alarm","led_result":"ok","prev_state":"NORMAL","new_state":"PERSON_ALERT"}}
{"ts":"2026-10-24 21:03:02","type":"THERMAL_ALERT","summary":"SOC温度76.20C超过75.0C阈值, LED切换为报警快闪","raw":{...}}
{"ts":"2026-10-24 21:04:40","type":"RECOVER","summary":"人员告警解除(连续5周期无person)、温度回落至70.0C以下(连续3周期), LED恢复心跳呼吸","raw":{"recovered":["PERSON_ALERT","THERMAL_ALERT"],"new_state":"NORMAL",...}}
```

字段说明: `ts` 本地时间秒级; `summary` 一句话人话(慢层可直接朗读/上报);
`raw` 是触发当周期的完整原始采样(`detail_common`: vision/vision_reason/temp_c/
frame_id/bbox_count/person_seen/best_person_score/npu_core_load) + 本次动作与结果
(action/led_result) + 状态迁移信息(prev_state/new_state 或 recovered)。
`raw` 字段是**增量兼容**的: 未来加字段不删字段, 慢层按「取所需」解析。

## 3. 状态机与迟滞参数表

```
                    person 连续≥2 周期 score≥0.5
          ┌────────────────────────────────────────┐
          │                                        ▼
   ┌──────────┐   连续≥5 周期无 person   ┌────────────────┐
   │  NORMAL  │◀────────────────────────│  PERSON_ALERT   │ (LED alarm)
   │ (heartbeat)│                        └────────────────┘
   └──────────┘                                   ▲
          ▲                                        │ zone0 > 75.0°C (瞬时触发)
          │   zone0 < 70.0°C 连续≥3 周期            │
          └────────────────────────────────────────┘
                        THERMAL_ALERT (LED alarm)
```

- PERSON 与 THERMAL 是**两个独立子状态**, 任一激活 → LED=alarm; 都恢复 → LED=heartbeat。
- 状态机整体状态取 `PERSON_ALERT > THERMAL_ALERT > NORMAL` 的最高优先级展示。

| 参数 | 值 | 含义 | 为什么这么定 |
|---|---|---|---|
| `--interval` | 5s (1~3600 可调) | 采样周期 | 与 5s 巡检节奏对齐; 300ms 视觉超时占周期 <6% |
| `PERSON_SCORE_TH` | 0.5 | person 置信度门槛 | 与网关检测阈值口径一致, 过滤边缘误检 |
| `PERSON_ENTER_N` | 2 | 连续检出次数→告警 | 单帧误检不闪灯; 10s 内确认, 人眼可接受 |
| `PERSON_EXIT_N` | 5 | 连续无 person 次数→恢复 | 恢复比进入慢(2:5), 防止人来回走动灯闪烁 |
| `TEMP_ENTER_C` | 75.0°C | 触发阈值(zone0, 瞬时) | SOC 高温警戒线; 瞬时触发宁可误报不可漏报 |
| `TEMP_EXIT_C` | 70.0°C | 恢复阈值 | **迟滞 5°C**: 75 上下抖动时不会反复翻状态 |
| `TEMP_EXIT_N` | 3 | 连续低于恢复阈值的周期 | 15s 确认真正回落, 排除单点读数毛刺 |
| 视觉超时 | 300ms | 连接/收帧各 300ms | 单周期最多损失 0.6s, 不拖死巡检节奏 |
| 黑匣子轮转 | >5MB → rename `.1` | 保留一代 | 64GB eMMC 上 10MB 上限可忽略; 一代够回溯 |

**迟滞防抖动的两个方向**: 进入条件与退出条件**不相等**(温度 75 进 / 70 出,
person 2 次进 / 5 次出), 且退出需要**连续 N 周期确认**——单点毛刺永远翻不了状态。
采样失败(网关拒连/超时、温区读不到)冻结对应计数器: 视觉断流不触发恢复
(数据缺失 ≠ 没有人), 温度读不到时若已在告警则**保持告警**(失败安全 fail-safe)。

## 4. 日志格式示例

### /root/patrol/patrol_events.log (黑匣子, 一行一 JSON, >5MB 轮转为 `.1`)

```json
{"ts":"2026-10-24 21:02:00","event":"DAEMON_START","detail":{"mode":"dry-run","interval_s":5,"pid":12345,"led_trigger_at_start":"heartbeat","action":"none","led_result":"not_attempted"}}
{"ts":"2026-10-24 21:03:02","event":"THERMAL_ALERT","summary":"SOC温度76.20C超过75.0C阈值, LED切换为报警快闪","detail":{"vision":"online","vision_reason":"online","temp_c":76.20,"frame_id":3510,"bbox_count":0,"person_seen":false,"npu_core_load":[5,0,3],"action":"set_alarm","led_result":"ok","prev_state":"NORMAL","new_state":"THERMAL_ALERT"}}
{"ts":"2026-10-24 21:05:12","event":"PERSON_ALERT","summary":"画面连续2周期检出人员(最高置信度0.81, 帧3604), LED切换为报警快闪","detail":{...}}
{"ts":"2026-10-24 21:06:44","event":"RECOVER","summary":"人员告警解除(连续5周期无person)、温度回落至70.0C以下(连续3周期), LED恢复心跳呼吸","detail":{"recovered":["PERSON_ALERT","THERMAL_ALERT"],"new_state":"NORMAL","action":"set_heartbeat","led_result":"ok",...}}
```

`event` 取值: `DAEMON_START` / `PERSON_ALERT` / `THERMAL_ALERT` / `RECOVER`。
**只在状态翻转时追加**, 稳态不刷屏。dry-run 下 `action` 为 `would_set_alarm` /
`would_set_heartbeat`, `led_result` 为 `dry_run_no_write` —— 黑匣子如实记录
「想做但没做」, 与真实模式可区分。

### stdout 心跳行 (每周期一行, 便于 `timeout 60 ... > log` 观察)

```
[2026-10-24 21:02:05] state=NORMAL temp=43.50C vision=online frame=3500 person=- npu=5%/0%/3% led=heartbeat
[2026-10-24 21:02:10] state=NORMAL temp=43.60C vision=online frame=3525 person=0.42 npu=6%/0%/3% led=heartbeat
[2026-10-24 21:02:15] state=NORMAL temp=43.55C vision=gateway_refused frame=- person=- npu=5%/0%/3% led=heartbeat
```

## 5. 为什么快层不用 LLM (三条硬理由)

1. **NPU 竞争**: RK3588 三核 NPU 已被 9000 推流网关 7×24 独占做 YOLOv8
   推理。若快层再加载 RKLLM(数 GB 权重 + 常驻推理上下文), 会与检测主业务
   抢 NPU 时间片, 直接威胁推流帧率与实时性——巡检是伴生功能, 没资格影响主业。
2. **时延**: LLM 单次推理数百 ms~秒级, 且慢层调度不可控; 而「温度超限→闪灯」
   是毫秒级就该给答案的反射动作。用 `if (temp > 75)` 这类确定性规则, 判定
   成本≈0, 周期内 300ms 超时框定一切 IO 上界, 实时性天然满足。
3. **可靠性**: LLM 输出不确定(可能幻觉、可能拒绝、可能超时), 而报警是安全
   行为, 必须**可预测、可复现、可回归测试**。规则状态机行为 100% 确定,
   出问题可二分定位; LLM 断供/卡死时快层必须照常工作——所以快层零模型依赖,
   只有 std+pthread, 与主业务故障域隔离。

## 6. 后续接入 LLM 慢层的预留接口

- **消费入口**: 慢层 `tail -F /root/patrol/llm_pending.jsonl`(或定期 `seek` 到
  上次读取 offset), 逐行解析 JSON, 喂给 RKLLM 做语义研判(合并同一窗口内的
  重复告警、生成中文巡检简报、决定是否升级策略)。
- **协议已冻结**: `{"ts","type","summary","raw"}` 四字段, `type` 仅 3 类;
  `raw` 增量兼容。慢层可以安全地按行消费, 不需要理解快层内部实现。
- **慢层只读**: 快层是 `llm_pending.jsonl` 的唯一写者(单进程追加, O_APPEND
  语义), 慢层消费进度由自己维护(记录 offset/行号), 不回写、不删除、不改名,
  彻底避免双写竞争。
- **背压策略**: 快层只在状态翻转时写事件(稳态零写入), 天然低频; 慢层若离线,
  文件按行累积, 5MB 级别完全可控(告警事件本身低频; 若未来事件暴涨, 慢层
  消费后可自行归档, 快层不负责清理)。
- **未来扩展位**: 快层已预留 `--interval` 与阈值常量(`TEMP_ENTER_C` 等),
  后续可加配置文件热加载; 慢层研判结果如需回控 LED, 应通过新独立通道
  (如未来 8088 控制面 RPC), 不直接写快层文件, 保持单向数据流清晰。

## 7. 运行方式 (板上)

```bash
# 编译(板上原生, 一律不交叉编译——glibc 风险)
bash /root/lab/build_patrol_daemon.sh

# 验收自测(dry-run: 不写任何 LED 节点, 动作字段记 would_*)
timeout 60 /root/lab/edgevision_patrol_daemon --dry-run --interval 5 > /tmp/patrol_dryrun.log 2>&1

# 正式巡检(写 LED; 退出时自动恢复 heartbeat)
/root/lab/edgevision_patrol_daemon --interval 5

# 可选: 完全不碰 LED
/root/lab/edgevision_patrol_daemon --no-led
```

安全边界: 本守护**永远不会 stop/restart edgevision.service**; 9000 端口只做
「连上→收一帧 248B→断开」的只读探测, 对网关零压力(建联瞬间网关即推一帧,
收满即断, 与上位机行为一致)。
