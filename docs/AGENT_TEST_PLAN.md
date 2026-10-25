# AGENT_TEST_PLAN.md — 端侧具身 Agent 回归测试计划与 NPU 竞争实测流程

> 适用对象: `lab/rkllm_embodied_agent.cpp` 编译出的板上二进制
> `/root/rkllm_deploy/rkllm_embodied_agent_p0`（P0 工具层修复版, 由 tool-fixer 并行交付）。
> 测试器: `lab/agent_regression_test.py`（PC 端, 纯标准库）。
> 采样器: `lab/bench_contention_sampler.py`（PC 端, 纯标准库）。
>
> **安全红线**: 不带 `--with-faults` 时测试器绝不启停任何 systemd 服务;
> 带 `--with-faults` 时也只在单条用例的 try/finally 中停服并在 finally 恢复,
> 恢复失败立即打印大警告并终止整场回归。

---

## 1. stdout 契约（测试器解析依据）

| 行类型 | 格式 | 出现时机 |
|---|---|---|
| 用户回显 | `👤 [User]: <query>` | 每轮输入后 |
| 工具调用 | `⚙️  [Agent 决策] 触发物理硬件工具: <tool_name>` | 模型发出 `<tool_call>` 时 |
| 参数行 | `   -> 设定参数: mode = <mode>` | 仅 `control_led` |
| 观察行 | `🔍 [硬件感知 (Observation)] 返回: <JSON数组>` | 每次工具执行后 |
| 终局汇报 | 自由文本（流式） | 工具循环结束 |

- REPL 提示符 `EdgeAgent> `, 支持 `/clear`（清上下文保系统提示词）; 无参数启动进 REPL,
  命令行传参则单轮退出。
- Observation JSON 字段:
  - `get_system_health` → `soc_temp_c / npu_temp_c / npu_load / mem_avail_mb / current_led_mode / status`
  - `get_led_status` → `led_name / current_mode / brightness / description`
  - `control_led` → `success(bool) / current_mode / msg`; P0 修复后非法 mode 返回
    `error: "invalid_mode"`, 旧二进制会安全回退 heartbeat（测试两者都算通过）。
  - `get_vision_telemetry` → `status("online"/"standby"/"no_data"/"error") / frame_id /
    detected_count / detections[]`

---

## 2. 用例表（12 组）

通过标准统一为: **该用例全部断言为真**。`T05`/`T08`/`T12` 为多路径宽容断言（`any_of`）,
命中任一路径即过——因为模型行为存在合理分歧（如拒绝非法指令时可以不调工具）。

| ID | 指令（要点） | 预期工具序列 | 关键断言 | 通过标准 |
|---|---|---|---|---|
| T01_temp_query | 「开发板现在温度是多少？硬件运行状态怎么样？」 | `get_system_health` | tool_called(health) + tool_not_called(control_led) + obs_json_valid + reply 含「温度/度」 | 只查不改, Observation 合法 |
| T02_led_status_query | 「现在板载指示灯是什么状态？」 | `get_led_status` | tool_called(get_led_status) + tool_not_called(control_led) | 只查不改 |
| T03_led_alarm | 「把板载指示灯调成急闪报警模式」 | `control_led(mode=alarm)` | tool_arg mode=alarm + obs success=true | 参数正确且写入成功 |
| T04_led_heartbeat | 「恢复心跳呼吸灯」 | `control_led(mode=heartbeat)` | tool_arg mode=heartbeat + success | 参数正确且写入成功 |
| T05_led_invalid_mode | 「把指示灯设成飞行模式」 | 无 或 `control_led(非法)` | any_of: ①调了且 obs error=invalid_mode ②回退 heartbeat ③口头拒绝 | **绝不允许 current_mode 变成非法值** |
| T06_vision_online | 「摄像头画面里现在有什么东西？」 | `get_vision_telemetry` | obs status=online 或存在 frame_id/detected_count | 视觉在线时帧数据可用 |
| T07_full_patrol | 「先检查硬件健康和温度, 再看看画面目标」 | `get_system_health` → `get_vision_telemetry` | tool_order 子序列断言 + 两者都调用 | 顺序正确、两工具齐 |
| T08_conditional_decision | 「如果画面里有人就报警, 否则保持呼吸灯」 | `get_vision_telemetry` → `control_led`(视条件) | 必调视觉; 控灯则 mode∈{alarm,heartbeat}; 或合理口头汇报 | 至少完成感知, 决策合理 |
| T09_repl_clear_then_query | REPL: 「把灯调成急闪报警」→ `/clear` → 「现在指示灯什么状态？」 | 轮1 `control_led(alarm)`; 轮2 `get_led_status` | /clear 后第二轮必须真查（tool_called get_led_status 且不 control_led） | 上下文清空后仍正确决策 |
| T10_long_context_boundary | ~800 字重复长指令（逼近 max_context=2048） | 任意 | exit_clean（进程不崩、正常退出） | 超长输入不致崩溃 |
| T11_repl_double_control | REPL: 「把灯调成常亮」→「把灯改成急闪报警」 | `control_led(on)` → `control_led(alarm)` | 两轮 tool_arg 分别=on / alarm | 连发指令参数不串扰 |
| T12_vision_offline_fault | 【--with-faults】停 edgevision 后「现在画面里有什么？」 | `get_vision_telemetry` | obs status∈{standby,offline,no_data,error} 或 reply 说明离线/无法判断 | 优雅降级, 不捏造数据 |

补充约定:
- 每条 CLI 用例独立起进程: `ssh <host> "cd /root/rkllm_deploy && timeout 180 ./<bin> '<query>'"`,
  query 经 base64 过线（默认）或 shlex.quote 转义（`--no-b64`）, 单引号安全。
- REPL 用例把输入序列（含 `exit`）经 stdin 一次性喂入。
- 结果落盘 `lab/agent_test_results/<时间戳>/results.json`（逐用例 pass/fail + 原始 stdout）
  与 `summary.md`（通过率表格）。

---

## 3. 指标口径

| 指标 | 定义 | 备注 |
|---|---|---|
| **工具决策正确率** | 「调用了正确工具**且**参数正确」的用例数 / 该维度适用用例数 | 只看工具层, 不看自然语言; T01/T02/T05 的 tool_not_called、T05 的拒绝路径均计入"正确决策"。分母在 results.json 里逐条可查 |
| **端到端成功率** | 断言**全部**通过的用例数 / 实际执行用例数 | SKIP（未开 --with-faults、服务非 active、stop 失败）不计入分母 |
| **时延 wall_s** | PC 侧计时器: 发起 ssh → 进程退出 | **包含** ssh 握手、模型逐 token 生成、板端 timeout 上限等全部开销, **不能**当 NPU 推理时延引用; 只用于纵向趋势对比（同一 host 同一传输方式） |

报告里引用任何一个百分比, 必须注明对应分母与 SKIP 情况。

---

## 4. NPU 竞争实测流程（先测再优化）

**目的**: LLM（RKLLM 3 核绑定）与 YOLO 网关（edgevision, RKNN 3 核绑定）同时抢 NPU 时,
互相掉多少帧/掉多少性能。**结论模板一律「先测再优化」: 没有竞争数据前,
不做任何"抢占优化"（绑核调整/限流/降频）**。

### 4.1 三场景

| 场景 | LLM 侧 | 视觉侧 | 说明 |
|---|---|---|---|
| S1 仅网关 | 不跑 | `systemctl start edgevision`（保持默认运行即可） | 基线: 视觉稳态吞吐/FPS 与温度内存 |
| S2 仅 LLM | 板上跑 `rkllm_embodied_agent_p0` 连续提问（或 REPL 脚本化连发） | `systemctl stop edgevision` | 基线: LLM 单独的 tokens/s 与温度内存 |
| S3 同时 | 同 S2 的连续提问 | edgevision 保持运行 | 竞争态: 两边指标同时采 |

> S2/S3 中 LLM 侧的"连续提问"建议用回归测试器的 T01+T07+T08 组合循环 3 轮,
> 保证每次负载形态一致。

### 4.2 采样器参数

```bash
python lab/bench_contention_sampler.py --duration 180 --interval 1 --out bench_S1_gateway.csv
python lab/bench_contention_sampler.py --duration 180 --interval 1 --out bench_S2_llm.csv
python lab/bench_contention_sampler.py --duration 180 --interval 1 --out bench_S3_both.csv
```

- `--interval 1`（1 Hz）足够捕捉温升与负载趋势; 采样本身走独立 ssh 通道, 对 NPU 无干扰。
- 采样窗口 180s: 覆盖「启动峰值 + 稳态」, 启动后前 30s 数据可标记为预热段剔除。

### 4.3 重复与统计

- **每场景至少重复 3 次**, 取**中位数**（温区/内存取全程稳态段中位数; FPS/tokens/s
  取稳态段中位数）。中位数抗单次离群（参考 stereo3d 深度中位数结论）。
- 三场景对比出表: zone0/zone1/zone2 温度、mem_avail_mb、以及各自业务指标（视觉 FPS、
  LLM tokens/s）在 S1/S2/S3 下的中位数与差值。
- **判读**:
  - S3 视觉 FPS 相对 S1 掉幅 > 15% 或 LLM tokens/s 掉幅 > 15% → 存在实质竞争,
    才进入优化议题（候选: 错峰调度、rknn_set_core_mask 划分核心、降低 LLM max_new_tokens）。
  - 差值 < 5% → 竞争可忽略, 结论写「实测无明显竞争, 无需优化」。
- 温度超 85℃ 或 MemAvailable 低于 1GB 时终止实验（模型 2.8GB + 视觉服务并存,
  内存是硬约束）。

### 4.4 结论模板

```
【NPU 竞争实测结论】(日期, 板子, 模型版本)
- S1 仅网关: 视觉 FPS 中位数 __; zone0 中位 __℃
- S2 仅 LLM: tokens/s 中位 __; zone0 中位 __℃
- S3 同时:   视觉 FPS __(-__%), tokens/s __(-__%); zone0 __℃
- 判定: 竞争显著/不显著（阈值 15%）→ 下一步: 优化议题清单 或 维持现状
- 局限: 采样含 ssh 开销; LLM 负载形态依赖提问脚本; n=3 中位数。
```

---

## 5. 故障注入安全协议（--with-faults 专属）

1. **开关隔离**: 不带 `--with-faults` 时, `T12` 直接 SKIP, 脚本内不存在任何启停服务的
   代码路径被执行。
2. **前置检查**: 注入前必须 `systemctl is-active edgevision` 为 active; 若服务本来就
   没跑, 该用例 SKIP（不背"把停着的服务当注入成果"的锅）。
3. **try/finally 恢复**: `systemctl stop edgevision` 后跑用例, **无论用例成败、超时、
   异常**, finally 里一律 `systemctl start edgevision` 并回读 `is-active` 验证。
4. **恢复失败处理**: 回读非 active → 控制台打印 70 列 `!` 大警告 + 手动恢复命令
   （`ssh root@<board-ip> 'systemctl start edgevision'`）, 并**立即终止整场回归**,
   防止后续用例在服务缺失下产生误导性结果。该状态同时写入 results.json 的
   `fault_recovery` 字段。
5. **LED 测后恢复**: 整场测试结束（无论成败）把 `/sys/class/leds/work/trigger` 直接写回
   `heartbeat`（不经过模型, 避免模型本身失败导致 LED 停在 alarm 爆闪）, 回读 trigger
   验证; `--no-restore-led` 可跳过。
6. **禁止事项**: 不停 `led-heartbeat` 服务; 不动 9000 推流以外的端口; 不在板上执行
   本仓库之外未审阅的脚本。

---

## 6. 已知局限

1. **stdout 解析对模型输出格式敏感**: 断言锚定的是「[Agent 决策]」「(Observation)」等
   方括号中文标记而非 emoji; 若 C++ 端改了这些打印文案, 测试器必须同步改
   `RE_TOOL/RE_ARG/RE_OBS` 三条正则。模型若生成非契约文本（如把工具名说在自然语言里）,
   工具断言不受影响（只认契约行）, 但 `reply_mentions` 宽松匹配可能偶发误报/漏报。
2. **wall-clock 含 ssh 开销**: 每条 CLI 用例都是一次全新 ssh 会话 + 模型冷启动加载
   （rkllm_init 数秒级）, wall_s 远大于纯推理时间, **只可用于趋势对比, 不可当延迟指标**。
3. **REPL 用例的一次性喂入**: 输入经管道一次性写入, 无逐轮等待; 依赖二进制按行消费
   stdin 的行为。若未来二进制改为异步交互, 需改用 pexpect 式逐行交互（当前纯标准库
   方案选择牺牲这一点换可移植性）。
4. **单轮查询的 NPU 冷启动**: T01~T08 每条都是冷启动进程, 工具决策本身不受影响,
   但时延数据冷热混杂, 一律不作延迟引用。
5. **采样器最小间隔 0.2s**: 纯标准库 ssh 通道的现实下限; 更高频需上 paramiko 或
   板端自采（有意不做, 保持零依赖）。
6. **`/sys/kernel/debug/rknpu/load` 权限**: 板上 root 可读; 若内核 debugfs 权限收紧,
   npu_core 列会为空（不致失败, 温度内存仍可用）。
7. **工具决策正确率的分母依赖用例设计**: "正确工具"是人工先验定义的（写在用例表）,
   模型给出表外但合理的多步方案时会被记为失败; 复核时看 results.json 里的
   原始 stdout 人工裁决。
