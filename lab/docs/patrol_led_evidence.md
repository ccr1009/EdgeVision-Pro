# task-5 板上验证证据 — LED alarm 真实爆闪 + 阈值可配置 + 单实例锁

- 日期: 2026-10-25 11:50 (板上时区)
- 执行人: teammate patrol-builder
- 板: EVM3588-B, Debian 11, 内核 5.10.110, /root/lab
- 全程 `edgevision.service` 前后均 `active`（未 stop/restart，只读复核两次）

## 1. 根因回顾（为什么旧 alarm 是假成功）

板上内核 5.10.110 出厂未编 `CONFIG_LEDS_TRIGGER_TIMER`，`/sys/class/leds/work/trigger`
的合法值列表里**没有 `timer`**，`echo timer > trigger` 返回 EINVAL。
旧版 `led_set_alarm()` 写 `trigger=timer` 后不检查内核实际接受结果 → 灯从未爆闪过。
本版照 agent 侧已跑通方案改为**用户态爆闪**：

```
fork → 子进程 setsid() → 0/1/2 重定向 /dev/null → execl("/bin/sh","sh","-c",
"while true; do echo 255 > /sys/class/leds/work/brightness; sleep 0.1;
 echo 0 > /sys/class/leds/work/brightness; sleep 0.1; done")
```

- 父进程写 pid 到 `/tmp/edgevision_led_alarm.pid`（与 agent 侧共用，天然互斥）；
- 切 heartbeat / SIGTERM 退出前：读 pid → SIGTERM → 最多 200ms 轮询 → SIGKILL 兜底
  → unlink pid 文件 → 兜底写 `trigger=none` + `brightness=0`；
- exec 前 0/1/2 必须重定向 /dev/null：否则永不退出的 while 循环持有父进程管道写端，
  调用方（timeout/ssh/管道）永远等不到 EOF（TROUBLESHOOTING 第 22 条同坑）。

## 2. 实际命令（Windows 侧一律写 .sh scp 上板执行，避免引号地狱）

```powershell
scp edgevision_patrol_daemon.cpp build_patrol_daemon.sh verify_task5.sh root@<board-ip>:/root/lab/
ssh root@<board-ip> "sed -i 's/\r$//' /root/lab/verify_task5.sh && bash /root/lab/verify_task5.sh"
```

板上关键步骤（`/root/lab/verify_task5.sh`，产物在 `/tmp/patrol_t5/`）：
1. `bash build_patrol_daemon.sh`（g++ -O2 -std=c++17 -Wall … -lpthread）
2. 非法参数用例 ×4（期望退出码 2）
3. 单实例锁用例（第二实例期望退出码 3）
4. `./edgevision_patrol_daemon --temp-enter 20 --temp-exit 18 --interval 2` 后台跑，
   3s 后每 0.1s `cat /sys/class/leds/work/brightness` 连采 20 次
5. `kill -TERM` 优雅退出 → 复查 trigger / 残留进程 / 日志

## 3. 编译结果（0 error 0 warning）

```
[build] g++ -O2 -std=c++17 -Wall edgevision_patrol_daemon.cpp -lpthread -o edgevision_patrol_daemon
[build] OK -> /root/lab/edgevision_patrol_daemon
BUILD_RC=0
```

MD5（板上 `md5sum` 实测）：

| 文件 | md5 |
|---|---|
| `edgevision_patrol_daemon.cpp` | `095ef17f204a6c55dc0aaf0414fff135` |
| `edgevision_patrol_daemon` | `616ac875b08144d7dc9c95c08b582e87` |

## 4. 参数校验用例（全部 rc=2，stderr 如实报因）

```
exit>=enter rc=2      → [patrol] --temp-exit 80.00 必须严格小于 --temp-enter 75.00 (迟滞带要求)
temp-enter 999 rc=2   → [patrol] --temp-enter 999.00 超出合理范围(1~150)
person-score 1.5 rc=2 → [patrol] --person-score 1.50 超出范围(0~1)
temp-enter abc rc=2   → [patrol] --temp-enter 不是合法数字: abc
```

## 5. 单实例锁用例（flock /tmp/edgevision_patrol.lock）

```
second instance rc=3 (期望3)
[2026-10-25 11:50:51] 另一个巡检守护已在运行(/tmp/edgevision_patrol.lock 被占用), 本实例退出(码 3)
inst1 ended rc=0 (SIGTERM 优雅退出)
```

## 6. 真实爆闪证据（--temp-enter 20 --temp-exit 18 --interval 2 实跑）

`cat /sys/class/leds/work/brightness` 每 0.1s 连采 20 次（爆闪周期 200ms）：

```
0 255 0 255 0 255 0 255 0 255 0 255 0 255 0 255 0 255 0 255
```

爆闪期间 `trigger` 生效位是 `[none]`（用户态直控 brightness 的前提）：

```
[none] rfkill-any … tcpm-source-psy-6-0022-online mmc0 heartbeat mmc1 rfkill0 rfkill1
```

爆闪子进程真实存在（ps 原文）：

```
   4725 ?        Ss     0:00 sh -c while true; do echo 255 > /sys/class/leds/work/brightness; sleep 0.1; echo 0 > /sys/class/leds/work/brightness; sleep 0.1; done
```

## 7. 运行日志（/tmp/patrol_t5/run20.log 全文）

```
[2026-10-25 11:50:51] edgevision_patrol_daemon start pid=4723 mode=led interval=2s
[2026-10-25 11:50:51] 阈值: person score>=0.50 连续2周期->PERSON_ALERT(连续5周期无person恢复); zone0>20.0C->THERMAL_ALERT(<18.0C连续3周期恢复)
[2026-10-25 11:50:51] LED trigger 当前: heartbeat (目标文件 /sys/class/leds/work/trigger)
[2026-10-25 11:50:51] LED -> alarm [set_alarm] (ok)
[2026-10-25 11:50:51] state=THERMAL_ALERT temp=43.46C vision=online frame=78039 person=- npu=14%/12%/14% led=alarm t_clear=0/3
[2026-10-25 11:50:53] state=THERMAL_ALERT temp=42.54C vision=online frame=78099 person=- npu=14%/14%/14% led=alarm t_clear=0/3
[2026-10-25 11:50:55] state=THERMAL_ALERT temp=42.54C vision=online frame=78157 person=- npu=14%/14%/14% led=alarm t_clear=0/3
[2026-10-25 11:50:57] exit: LED restore heartbeat [ok]
[2026-10-25 11:50:57] patrol daemon exit
```

## 8. 事件黑匣子（/root/patrol/）

patrol_events.log 本轮新增（摘录）：

```json
{"ts":"2026-10-25 11:50:51","event":"DAEMON_START","detail":{"mode":"led","interval_s":2,"pid":4723,"temp_enter_c":20.00,"temp_exit_c":18.00,"person_score_th":0.50,"led_trigger_at_start":"heartbeat","action":"none","led_result":"not_attempted"}}
{"ts":"2026-10-25 11:50:51","event":"THERMAL_ALERT","summary":"SOC温度43.46C超过20.00C阈值, LED切换为报警快闪","detail":{"vision":"online","vision_reason":"online","temp_c":43.46,"frame_id":78039,"bbox_count":1,"person_seen":false,"npu_core_load":[14,12,14],"action":"set_alarm","led_result":"ok","prev_state":"NORMAL","new_state":"THERMAL_ALERT"}}
```

llm_pending.jsonl 同步出现同一事件（慢层契约 `{"ts","type","summary","raw"}`）：

```json
{"ts":"2026-10-25 11:50:51","type":"THERMAL_ALERT","summary":"SOC温度43.46C超过20.00C阈值, LED切换为报警快闪","raw":{"vision":"online","vision_reason":"online","temp_c":43.46,"frame_id":78039,"bbox_count":1,"person_seen":false,"npu_core_load":[14,12,14],"action":"set_alarm","led_result":"ok","prev_state":"NORMAL","new_state":"THERMAL_ALERT"}}
```

## 9. 退出后终态（SIGTERM 优雅退出）

- trigger 生效位回到 `[heartbeat]`：
  `none rfkill-any … mmc0 [heartbeat] mmc1 rfkill0 rfkill1`
- 爆闪进程残留：`ps ax | grep brightness` → **0 行**
- `brightness` 读数 0（heartbeat 触发器接管）
- `edgevision.service`：验证前后两次复核均 `active`

## 10. 交付物与范围

- `edgevision-rk3588/lab/edgevision_patrol_daemon.cpp`（v2：用户态爆闪/阈值参数/flock）
- `edgevision-rk3588/lab/build_patrol_daemon.sh`（未改动，沿用）
- 本证据文件 `edgevision-rk3588/lab/patrol_led_evidence.md`
- 未触碰 `rkllm_embodied_agent.cpp`（tool-fixer 范围）与 `docs/`（Lead 统一回填）
