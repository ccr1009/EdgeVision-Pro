# Agent 工具清单与设计取舍

> 6 个硬件工具的契约：**模型看到什么、参数怎么传、返回什么结构、失败长什么样**。
> 面试常问「你的工具怎么设计的、模型为什么会误用」——这份就是答案。
>
> 相关：[ARCHITECTURE.md](../ARCHITECTURE.md) ｜ [TROUBLESHOOTING.md](../TROUBLESHOOTING.md) 第 19/20/23/28 条

---

## 总览

| 工具 | 参数 | 作用 | 是否有副作用 |
|---|---|:---:|---|
| `get_system_health` | 无 | 温度（SOC/CPU/NPU 各温区）+ NPU 三核负载 + 可用内存 + LED 状态 | ❌ 只读 |
| `get_led_status` | 无 | LED 当前工作模式 + 亮度占空比 | ❌ 只读 |
| `control_led` | `mode` (enum) | 改 LED 模式：`heartbeat` / `alarm` / `on` / `off` | ✅ **写硬件** |
| `get_vision_telemetry` | 无 | 视觉网关最新检测框（坐标/类别/置信度/新鲜度） | ❌ 只读 |
| `get_patrol_events` | `tail` (string) | 巡检事件历史（ts / type / summary），研判"过去发生过什么" | ❌ 只读 |
| `get_patrol_status` | 无 | 巡检守护**当前运行态**（是否在跑、实际生效阈值、pid、快照新鲜度） | ❌ 只读 |
| `control_service` | `name` + `action` (enum) | 启停板载子系统：看板(:8090) / 推流(:9000) / 巡检守护 / **语音网关自身** | ✅ **操作系统服务** |

**只有 2 个写操作**（`control_led` 写硬件、`control_service` 操作系统服务），其余只读——这是刻意的：
Agent 的主要价值是"研判 + 在明确指令下执行单一动作"，不是让它自由操作硬件。

---

## 逐个说明

### 1. `get_system_health`

**模型看到的描述**：
> 获取开发板当前的物理温度(SOC/CPU/NPU各温区)、NPU三核实时利用率、可用内存及指示灯当前状态

**参数**：无（`properties: {}`）

**返回**（关键字段）：
```json
{
  "soc_temp_c": 42.5,
  "npu_temp_c": 42.5,
  "npu_load": {"core0": 13, "core1": 13, "core2": 13},
  "mem_available_mb": 3834,
  "led_mode": "heartbeat",
  "note": "是否触发告警取决于巡检守护当前生效阈值, 请调用 get_patrol_status 查询"
}
```

**设计要点**：**故意删掉了 `status: normal_optimal` 这种结论字段**。
早期版本返回结论，模型直接抄，跳过推理——而结论基于 prompt 里写死的 75°C，
与实际生效的 20°C 阈值矛盾（详见 TROUBLESHOOTING 第 28 条）。
现在只给原始温度 + 一句明确的指路（"要判断告警请查 get_patrol_status"）。

---

### 2. `get_led_status`

**参数**：无

**返回**：
```json
{
  "current_mode": "heartbeat",
  "description": "正常呼吸闪烁",
  "brightness": 0,
  "brightness_duty_pct": 22
}
```

**设计要点**：**周期模式下必须同时给占空比，不能只给瞬时亮度**。
`heartbeat` 的灭相占 75% 以上，单点采样亮度读 0 是**正常的采样假象**，不是"关闭"。
早期只返回 `brightness`，模型把正在呼吸的灯判成"已关闭"，还对用户编了一套因果（TROUBLESHOOTING 第 23 条）。
`brightness_duty_pct` 是对 20 个采样点（间隔 60 ms）算出的亮相比，静态模式下返回 `-1`。

---

### 3. `control_led`

**参数**：
```json
{
  "mode": {
    "type": "string",
    "enum": ["heartbeat", "alarm", "on", "off"],
    "description": "heartbeat(正常呼吸闪烁), alarm(100ms紧急急闪), on(常亮), off(关闭)"
  }
}
```
`required: ["mode"]`

**返回**：
- 成功：`{"success": true, "mode": "off", ...}`
- 失败：`{"success": false, "error": "invalid_mode"}` 或 `{"error": "led_readonly_mode"}`

**设计要点**：
1. **用 enum 而不是自由字符串**——模型容易编造 `"blinking"`、`"normal"` 这类词，enum 能在 schema 层挡住一部分。
2. **非法值明确拒绝且不动硬件**，返回 `invalid_mode`，避免"以为改了其实没改"。
3. **幂等写**：用户说"设成心跳"时，即使当前已经是心跳也要真的写一遍。
   模型容易因为"当前就是"而跳过——但用户可能刚被守护进程改过，或上一轮写入失败。
4. **`--no-led` 模式下有独立约束**：控制权归巡检守护，Agent 只读，
   调用会返回 `led_readonly_mode`。这条在 sys_prompt 里是"最高优先级"，因为实测模型会连试 4 种模式。

---

### 4. `get_vision_telemetry`

**参数**：无

**返回**：
```json
{
  "targets": [{"x": 320, "y": 180, "w": 64, "h": 128, "class": "person", "score": 0.87}],
  "fresh": true,
  "frame_id": 18432
}
```

**设计要点**：**必须带新鲜度**（`fresh` + `frame_id`）。
视觉是独立进程推流，Agent 读到的可能是几秒前的帧。
没有新鲜度字段时，模型会把"3 秒前画面里有人"当成"现在有人"（条件指令场景下直接导致误动作）。

---

### 5. `get_patrol_events`

**参数**：`tail`（**type 是 string 不是 integer**）
> 可选, 读取最近多少条事件, 默认 10, 最大 50

**返回**：按时间排序的事件列表
```json
[{"ts": "2026-10-28 11:15:29", "type": "THERMAL_ALERT", "summary": "SOC温度47.15C超过20.00C阈值, LED切换为报警快闪"}]
```

**设计要点**：
- **`tail` 用 string**：实测模型经常传 `"tail": "10"`，用 integer 会被 schema 拒绝导致整轮失败，不如在服务端 `stoi` 容错。
- **只说明过去**：这工具产出的是历史，不能回答"现在会不会报警"。sys_prompt 里反复强调这一点，因为模型极爱用历史推断现状。

---

### 6. `get_patrol_status`

**参数**：无

**返回**（最复杂的一个）：
```json
{
  "running": false,
  "reason": "snapshot_stale",
  "snapshot_age_s": 105529,
  "pid": 5017,
  "mode": "led",
  "interval_s": 5,
  "active_thresholds": {"soc_temp_enter_c": 75.0, "soc_temp_exit_c": 70.0, "person_score": 0.5},
  "last_state": "NORMAL",
  "soc_temp_c": 37.92,
  "led_target": "heartbeat",
  "updated_at": "2026-10-28 14:07:01",
  "verdict": "巡检守护当前未运行(snapshot_stale), 没有任何进程会自动根据温度/人员切换 LED; ..."
}
```

**设计要点**：
1. **运行态必须外化**。阈值是巡检守护进程内的运行时状态（通过命令行参数传入，验收时会用 20°C 这种极低值），
   **不暴露给模型，模型就只能编**（TROUBLESHOOTING 第 28 条）。
2. **存活判定用三条件与**：快照说 `running: true` + 时间戳未过期 + `kill(pid, 0) == 0`。
   只看时间戳会漏"进程刚死、快照还没过期"的两三个采样周期；只看 pid 会漏 pid 被内核复用。
3. **`verdict` 是唯一允许给结论的字段**，但它描述的是"守护进程状态"，不是"温度是否正常"——
   两者是不同的问题，混在一起就是第 28 条那个 bug。

---

### 7. `control_service`（2026-10-30 新增）

**参数**：
```json
{
  "name":   {"enum": ["dashboard", "gateway", "patrol", "voice"]},
  "action": {"enum": ["start", "stop", "status"]}
}
```

**服务映射**：

| name | systemd unit | 说明 |
|---|---|---|
| `dashboard` | `edgevision-dashboard` | 车辆实时检测看板 HTTP :8090（/dev/video83） |
| `gateway` | `edgevision` | TCP 推流网关 :9000（/dev/video81） |
| `patrol` | `edgevision-patrol` | 巡检守护（快层，纯 C 循环，不占 NPU） |
| `voice` | `voice-gateway` | 语音交互网关（**就是 Agent 自己**） |

**返回**：
```json
{"service": "dashboard", "action": "start", "success": true, "active": "active",
 "note": "看板已启动(:8090)。视觉推理会占用 NPU，我之后的响应会变慢，这是正常的资源争用。"}
```

**三个必须处理的问题**：

1. **杀自己之前要把话说完** —— `voice` + `stop` 时先返回"3 秒后关闭"，
   由 `setsid()` 脱离会话的子进程延迟执行；否则用户只听到半句就断线
2. **两种启动方式都要覆盖** —— `systemctl stop` 停不掉 `voice_start.sh` 手动起的实例，
   所以实际命令是 `sh -c "systemctl stop ...; pkill -TERM -f voice_gatewa"`
3. **启动后的副作用必须如实说** —— 开看板会让 decode 从 7 掉到 3 tok/s（实测），
   工具返回里带 `note` 要求模型主动告知。这是**给事实 + 必要的上下文**，不是替模型下结论

细节见 TROUBLESHOOTING 第 36 条。

---

### 回执规范（2026-10-31）

`control_service` 的返回值会被两条路径消费：LLM 组织自然语言、快速通道直接播报。
两条路径都要求**回执自带语义**，不能靠调用方猜：

| 字段 / 约定 | 作用 |
| :--- | :--- |
| `note` 含「本来就没在运行」 | 区分 no-op 与真实停止（见 TROUBLESHOOTING 第 44 条） |
| `already: true` | 启动时目标已达成，不是这次启动的 |
| `handover: true` | 语音网关的控制权交接，进程会自己退出 |
| `url` | 看板的真实访问地址，**必须带**（不给就会编 `localhost`） |
| 快速通道措辞带对象名 | 「看板已停止」而不是「已停止」——一句话里必须有主语 |

这份约定解决的是同一个老问题：**工具返回值是 Agent 的「感知」，感知含糊，回答就一定含糊。**

---

### voice 的四种语义（2026-10-31 修正）

`control_service(name="voice")` 不能按"另一个服务"处理——**它可能就是 Agent 自己**。
原先只按 start/stop 两分支写，实测出现了两种错答：

| 处境 | 用户说 | 旧行为 | 现行为 |
| :--- | :--- | :--- | :--- |
| 在网关内（父进程 = `voice_gateway`） | 关闭语音助手 | 延迟 3 秒自杀 ✅ | 不变 ✅ |
| 在网关内 | 开启语音助手 | 试图再起一个网关（抢 NPU） | **「已经在运行了」** |
| 在手动控制台（自己占 2 GB NPU） | 开启语音助手 | `systemctl start` → 必 `errno 14` | **交接**：本进程退出释放 NPU，8 秒后由 systemd 拉起 |
| 在手动控制台 | 关闭语音助手 | 声称"3 秒后关闭"，其实什么都没关 | **「本来就没在运行」** |

**如何判断"我是不是网关"**：读 `/proc/<ppid>/comm`，父进程是 `voice_gateway` 即成立。
比 `pgrep` 可靠——不会匹配到自己，也不受进程名截断影响。

**交接为什么要 8 秒**：`rkllm_destroy` 释放 2 GB NPU 映射要 3~5 秒（见 TROUBLESHOOTING 第 39 条），
起太快会撞上 `errno 14`。代价是控制台会自己退出——这是预期行为，会先说明再退。

---

## 五个设计取舍（面试重点）

| 取舍 | 为什么 |
|---|---|
| **给事实，不给结论** | 结论会过期、会基于错阈值、会让模型跳过推理。`status: normal_optimal` 这种字段是反模式 |
| **运行态必须外化成可查快照** | 进程内状态模型无法感知，不给它就是在逼它编 |
| **写操作要有 enum + 明确拒绝** | 模型会发明参数值；拒绝时必须不动硬件并说明原因 |
| **周期量必须给占空比** | 单点采样是混叠，会把"正在闪"读成"关着" |
| **每轮最多 5 步** | 2B 模型容易在工具调用里打转（实测对"你好"连调 5 个工具），`max_steps=5` + 相同工具签名检测提前收束 |

---

## 已知失败模式（都是实测遇到的）

| 现象 | 根因 | 处理 |
|---|---|---|
| 模型返回的 tool_call 解析失败 | RKLLM `role="tool"` **强制要求规范 JSON 数组** `[{...}]`，不能是对象，`tool_response_str` 也不能为空 | 正常化后再回填（TROUBLESHOOTING 第 19 条） |
| 说"已把灯关掉"，实际灯还在闪 | `trigger=timer` 写入假成功（内核没编 `CONFIG_LEDS_TRIGGER_TIMER`），且写失败没人检查 | 写后**回读比对**，失败如实上报（第 20 条） |
| 回答"灯已关闭"，实际正爆闪 | 单点采样混叠 | 改给占空比（第 23 条） |
| 答"温度正常所以不报警"，实际已超阈值 | 阈值未外化，模型拿 prompt 里的 75°C 常识替代真实 20°C | 新增 `get_patrol_status`（第 28 条） |
| 对"你好"连调 5 个工具、68 秒才回 | sys_prompt 写着"必须先调全部感知工具拿全量数据" | 改成"只调能直接回答的那一个"（第 29 条） |

---

## 相关文档

- 多步推理的演示设计：[`AGENT_MULTISTEP_DEMO.md`](AGENT_MULTISTEP_DEMO.md)
- 巡检守护（快层）为什么不用 LLM：[`PATROL_DAEMON_DESIGN.md`](PATROL_DAEMON_DESIGN.md)
- 12 条用例与故障注入：[`AGENT_TEST_PLAN.md`](AGENT_TEST_PLAN.md)
- 代码：`lab/rkllm_embodied_agent.cpp`（工具 schema 在第 1181~1260 行）
