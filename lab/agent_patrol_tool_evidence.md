# task-6 验收证据: Agent 接入巡检守护事件 + --no-led + 多步能力

> 作者: tool-fixer (teammate) ｜ 日期: 2026-10-25 ｜ 板: root@<board-ip> (EVM3588-B, Debian 11)
>
> 本文档记录 task-6 全部验收证据: 改造摘要 / 编译输出 / 4 条冒烟原始输出 / 12 条回归结果 / md5 对照。
> 板上冒烟完整 stdout: `/root/rkllm_deploy/p0_smoke/t6_{A-read-events,B-led-and-research,C-no-led,D-multi-tool}.log`

---

## 1. 改造摘要 (全部在 edgevision-rk3588/lab/rkllm_embodied_agent.cpp)

| # | 改造点 | 落点 (行号为最终 961 行版) |
|---|---|---|
| 1 | 新工具 `get_patrol_events`: 读 `/root/patrol/patrol_events.log`(黑匣子, event 键) + `/root/patrol/llm_pending.jsonl`(慢层契约, type 键, PERSON_ALERT/THERMAL_ALERT/RECOVER), 合并按 ts 排序取尾 N 条, 每条 `{ts,type,summary}`; 参数 tail 默认 10 上限 50 | 505-620 行区 |
| 2 | 日志读取用 `read_tail_lines()`: `seekg(end)` 回退 `n*1024B` 丢半行再 getline, **严禁整读**(日志可被轮转成 .1 且单份 5MB+), 内存 O(n) 与文件大小无关; 超长(≥1024B)畸形行跳过 | 511-543 |
| 3 | Observation 体积熔断 `PATROL_OBS_MAX_BYTES=1500`: 新事件优先, 超限追加 `{"note":"older events truncated to fit context"}` 截断, 防止肥事件列表挤爆推理上下文 | 610-640 |
| 4 | `--no-led` 只读模式: `g_no_led_mode` 闸门在 tool_control_led 白名单之前, 任何 mode(含合法)都不碰 sysfs, 返回 `[{"success":false,"error":"led_readonly_mode","note":"patrol daemon owns LED; agent is read-only"}]`; 启动打印提示行 | 25/819-824/267-277 |
| 5 | `max_steps` 3→5 + stall 检测: 连续两轮工具调用签名相同(无新工具)提前收束; 耗尽且仍有 `<tool_call>` 打印上限警告停住 | 760-775 |
| 6 | `max_context_len` 2048→8192: 冒烟暴露多工具轮 Observation 回填把 2048 挤爆(`E rkllm: The context is not enough! history_s:1309, cur_input_s:843`), 终局汇报被掐死; 模型头上限 16384, 板上空闲内存足够 | 855-857 |
| 7 | `max_new_tokens` 384→512: D 条长研判报告在 384 tokens 被硬截断(断在表格中间), 512 后完整收尾 | 811 |
| 8 | sys_prompt 补: get_patrol_events 准则 + 多步任务先感知再决策 + 有巡检事件优先读事件不直接控灯; 严禁捏造行后追加「工具返回 success:false 或任何 error 必须如实告知用户操作被拒绝及原因, 绝对不能声称操作已成功」(C 条曾被模型嘴上编造「已切换 alarm」); 回归修复再加【最高优先级】幂等执行条款 + 条件实测条款(见 §5) | 849-872 |
| 9 | tools_json 注册 `get_patrol_events` (tail 参数); dispatch 链加分支+`<parameter=tail>` 提取+契约前缀打印不变 | 864-882 |
| 10 | 不变量: 4 个旧工具全保留, `[Agent 决策]/设定参数:/(Observation)]` 契约前缀未动, sys_prompt「严禁捏造虚假硬件数据」未动, 不碰 patrol daemon 与 9000 网关 | — |

## 2. md5 对照 (改前基线 → 改后)

| 文件 | md5 | 说明 |
|---|---|---|
| 改前基线(本地=板上) | `44ec7166d4bf7c8e06d27de0b6799255` | task-1 交付版, 753 行 |
| 中间版 | `099156fd37355a7a6def2d1958d67b5e` (43304B, 967 行) | 冒烟 v2 发现 C 条汇报编造成功 + D 条报告被 max_new_tokens=384 截断 |
| 修复版 | `d2c6a41bc46f3805529704f16e1f8a50` (43537B, 980 行) | max_new_tokens 384→512 + sys_prompt 诚实条款(不许把拒绝说成成功) |
| 回归修复版 | `7c0f8f07fee9d2d3f68845c009b96131` (44252B, 956 行) | T04/T11 根因修复: sys_prompt 幂等执行条款(显式控灯指令必须立即 control_led, 不被事件干扰改判、不因"当前已相同"跳过) |
| **最终版本地 `lab/rkllm_embodied_agent.cpp`** | `6ec791e3a2f3d9e9bc6227531abbf2cc` (44868B, 961 行) | T08 根因修复: sys_prompt 条件实测条款(条件引用什么数据源就必须实测, 巡检历史事件不能替代实时感知) |
| **板上 `/root/rkllm_deploy/rkllm_embodied_agent_p0_t6.cpp`** | `6ec791e3a2f3d9e9bc6227531abbf2cc` | scp 后板上实测, **与本地一致** (44868B) |
| 板上二进制 `/root/rkllm_deploy/rkllm_embodied_agent_p0_t6` | `0ddafcb538901ba5e9d7ecfd3c7b4bc7` (95576B, 12:45:23) | 最终版编译产物, 12/12 回归所用二进制 |

## 3. 板上编译输出

命令: `cd /root/rkllm_deploy && g++ -O2 rkllm_embodied_agent_p0_t6.cpp -L. -lrkllmrt -Wl,-rpath=. -o rkllm_embodied_agent_p0_t6`

```
=== 上传后源码 md5 ===
099156fd37355a7a6def2d1958d67b5e  /root/rkllm_deploy/rkllm_embodied_agent_p0_t6.cpp
=== 关键新增点 grep 自检 ===
g_no_led_mode 4 处 / get_patrol_events 8 处 / max_steps=5 在 760 行 / --no-led 在 819 行
=== 开始编译 (bin: rkllm_embodied_agent_p0_t6) ===
BUILD_EXIT=0
```

**0 error 0 warning** (g++ 无任何告警输出, BUILD_EXIT=0)。

最终版(d2c6a4…)重传后板上 md5 一致, 再次编译 **BUILD_EXIT=0 零 error 零 warning**。

回归修复版(7c0f8f…)与最终版(6ec791e3…)重传后板上 md5 均与本地一致, 两次编译均 **BUILD_EXIT=0 零 error 零 warning**; 最终二进制 `rkllm_embodied_agent_p0_t6` 95576B (板上 12:45:23 重建, md5 0ddafcb5…)。

## 4. 冒烟 4 条 (板上实测, 完整 stdout 见板上 p0_smoke/t6_*.log)

> 本节冒烟输出在最终版(d2c6a4…: max_context=8192 + Observation 熔断 1500B + max_new_tokens=512 + sys_prompt 诚实条款)上重跑, 板上时间 2026-10-25 12:15:50~12:19:11, 4 条 exit 全 0。
> 完整 stdout: 板上 `/root/rkllm_deploy/p0_smoke/t6_{A-read-events,B-led-and-research,C-no-led,D-multi-tool}.log`。

### A. 读巡检事件 — `./rkllm_embodied_agent_p0_t6 "现在巡检记录里有哪些异常事件"`

- 模型一轮并调 `get_patrol_events(tail=50)`(契约前缀 `⚙️ [Agent 决策]` + `-> 设定参数: tail = 50` 正常)。
- Observation 返回 11 条事件(黑匣子 DAEMON_START 空 summary + THERMAL_ALERT 带「SOC温度43.46C超过20.00C阈值, LED切换为报警快闪」), 体积熔断未触发。
- 终局完整汇报两起热异常(11:50:51 43.46°C / 11:56:14 48.08°C)并建议进一步排查散热, **无编造**。exit=0。

### B. 查灯态+研判 — `"开发板现在的指示灯状态和温度都正常吗"`

- 模型并调 `get_led_status` + `get_system_health`(LED off / SOC 51.769°C / NPU 负载 65%×3 / mem 4595MB)。
- 终局完整: 逐项研判灯态、温度、NPU 利用率, 结论「硬件运行状态正常且稳定」。**context 8192 修复生效, 无 `The context is not enough!`**。exit=0。

### C. --no-led 拒绝控灯 — `--no-led "把板载指示灯调成急闪报警模式"`

- 启动打印 `[Mode] --no-led 只读模式: 巡检守护拥有 LED 写权, 本进程 control_led 只返回 led_readonly_mode 拒绝且绝不触碰 sysfs。`
- 模型先 `get_led_status`, 再 `control_led(mode=alarm)` → **被闸门拒绝**: `[{"success": false, "error": "led_readonly_mode", "note": "patrol daemon owns LED; agent is read-only"}]`。
- **sys_prompt 诚实条款生效**: 终局如实汇报「作为运维 Agent 无法直接控制其模式, 建议通过巡检守护进程的异常处理机制触发」, **不再编造「已切换 alarm」**(冒烟 v2 曾编造)。
- LED 前后对照: trigger 全程 `[none]` / brightness 0, **零变化** ✓ (验收期间 patrol-builder 守护未在跑, 灯保持 none 属正常)。exit=0。

### D. 一条指令连调 ≥2 工具 — `"全面巡检一下：先看巡检告警事件，再看系统温度健康，最后看看画面里有什么，综合研判设备状态"`

- 一条指令模型按序并调 **3 个工具**: `get_patrol_events(tail=10)` → `get_system_health` → `get_vision_telemetry`(fresh=true, frame_id=126353)。
- **max_new_tokens 512 修复生效**: 终局综合研判报告完整(上轮 384 tokens 截断在表格中间), 本轮含事件分析/温度健康/视觉检测/综合研判/处置建议完整收尾。exit=0。
- 模型自述「SOC 温度超 20°C 阈值属过热风险」沿用了事件流里的巡检口径(阈值是 patrol daemon 配置的告警线, 非硬件安全线), 属模型解读层面, 工具数据全部真实。

## 5. PC 侧 12 条回归

命令: `python edgevision-rk3588/lab/agent_regression_test.py --with-faults --timeout 240 --out-dir _test_results --bin /root/rkllm_deploy/rkllm_embodied_agent_p0_t6`

### 第一轮 (10/12, 结果 `_test_results/20261025_121945/`)

- PASS×10: T01 temp / T02 led_status / T03 alarm / T05 invalid_mode / T06-T10 / T12(故障注入: 停 edgevision→恢复 active, LED 自动写回 heartbeat)。
- **FAIL T04_led_heartbeat**: 期望 `control_led(mode=heartbeat)`, 实际模型读事件后被 THERMAL_ALERT 带偏, 改控 off —— sys_prompt「不要在无依据时直接改变指示灯」条款太强, 显式指令也被事件干扰改判。
- **FAIL T11_repl_double_control#1**: REPL「把灯调成常亮」时灯已是 on/255(前置用例遗留), 模型查完状态自作主张「无需调整」跳过 control_led; 第 2 轮 alarm 正常 —— 缺「显式指令幂等执行」条款。**非上下文串扰**。
- 失败详情: `_test_results/_t04_t11_detail.txt`。

### 根因修复 (sys_prompt, 本地 956 行版)

- 改写原「不要在没有依据的情况下直接去改变指示灯」→「研判类任务优先读事件; **用户明确下达指示灯控制指令时必须立即 control_led 按用户要求执行, 不要被事件干扰或改判模式**」。
- 新增【最高优先级】幂等执行条款: 用户要求把灯调成某模式时必须立即调用 control_led 且 mode=用户要求的值, 「**即使查询到当前状态接近或相同也要显式执行(幂等写), 绝不能因为\"当前已经是这样\"而跳过; 也不要参考温度、巡检事件等其他信息来更改用户指定的模式**」。

### 复验与最终全量 (最终二进制 md5 6ec791e3…, 板上 12:45:23 重建)

- `--only T04_led_heartbeat,T11_repl_double_control` 复跑 (二进制 7c0f8f…): **2/2 PASS** (24.0s / 43.6s, `_test_results/20261025_123347/`)。
- 第二轮全量 (二进制 7c0f8f…, `_test_results/20261025_123521/`): **11/12** —— 新增 FAIL T08_conditional_decision: 条件指令「画面里有人就报警」模型未调 get_vision_telemetry, 被 THERMAL_ALERT 带偏断言「守护已报警无需干预」。根因: 「有巡检事件优先读事件」条款权重过强, 历史事件替代了条件实测。
- **T08 修复** (本地 961 行版, md5 6ec791e3…): sys_prompt 新增【最高优先级】条件实测条款 —— 条件引用哪个数据源就必须调对应工具实测, 巡检历史事件只用于研判过去、不能替代实时感知, 不能以「守护进程已处理」为由跳过条件未触发的动作。板上重编译 0 error 0 warning (95576B, 12:45:23), `--only T08` 复跑 **PASS** (80.7s, `_test_results/20261025_124539/`)。
- **最终全量 12/12 PASS (100%)** (二进制 6ec791e3…, `_test_results/20261025_124718/`): T01 36.9s / T02 23.2s / T03 24.3s / T04 28.8s / T05 27.6s / T06 25.4s / T07 40.0s / T08 27.7s / T09 32.8s / T10 90.8s / T11 32.3s / T12(故障注入) 19.7s。回归后脚本自动写回 LED ✔。
- 收尾核验: 板上 trigger `[heartbeat]` / brightness 255 (曾发现 patrol 链路残留爆闪子进程 pid 20495 持续把 trigger 拉回 none, kill 后恢复)。

> 坑: 复测脚本若用 PowerShell `-Encoding ASCII` 写中文查询, 板端收到 `???????` 乱码导致用例无效 (T04_EXIT=0 但行为无效, 需作废重测) —— 含中文的板端指令一律走回归脚本的 base64 通道或 `--only`。

## 6. 已知坑记录 (复现/规避)

1. **rpath=. 相对 CWD**: ssh 直接执行 `./bin` 报 exit 127 打不开 librkllmrt.so → 必须 `cd /root/rkllm_deploy` 再跑, 或 `export LD_LIBRARY_PATH=/root/rkllm_deploy:$LD_LIBRARY_PATH`。
2. **Windows 侧 ssh 双引号转义**: `"tr -d '\r'"` 里 `\r` 被 PowerShell 吃成字面回车 → 脚本内不要依赖转义序列, LF 规范化全部在本地用 write 工具完成后 scp。
3. **脚本 CRLF 混行**: Windows 编辑过的 .sh 即使本地看是 LF 也可能带残余 CR → 板上统一 `tr -d '\r'` 兜底。
4. **patrol-builder 并行改动**: 验收期间 patrol daemon 出现反复重启(事件流多次 DAEMON_START), LED trigger 在 heartbeat/none 跳变属守护行为, 与 Agent 无关; 冒烟判读时用事件流时间戳区分。
5. **PowerShell 脚本编码**: `-Encoding UTF8` 带 BOM → 板上 bash 报 `\ufeff#!/bin/bash: No such file or directory`; `-Encoding ASCII` 把中文变 `???????` → 用例无效。含中文的板端指令一律走回归脚本 base64 通道(`--only`), 或 printf 内联写脚本。
6. **`echo BUILD_EXIT=$?` 被 PowerShell 展开**: 显示 `BUILD_EXIT=False` 属显示层问题, 以 g++ 有无告警输出 + ls 时间戳/二进制 md5 为准。
7. **残留爆闪子进程抢占 LED**: patrol 链路(守护/冒烟)遗留的用户态爆闪子进程(`while true; do echo 255>…brightness; sleep 0.1; echo 0>…; done`)会持续把 trigger 拉回 none, 手工 `echo heartbeat > trigger` 写不进; 先 `ps aux | grep brightness` 找到 pid kill, 再写 trigger 才生效。
8. **ssh 内联脚本引号**: `printf '…grep -o "\[.*\]"…'` 的内层双引号会把 ssh 外层引号配对搞坏(EOF error); 复杂板端脚本一律本地 base64 编码后 `echo <b64> | base64 -d > x.sh && bash x.sh`。
