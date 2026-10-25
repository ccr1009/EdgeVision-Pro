#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RK3588 端侧具身 Agent —— 多步演示闭环编排器 (纯标准库)

一句话说明
----------
把「巡检 -> 研判 -> 执行 -> 汇报」这一整条链路跑成**一条命令可复现、每步都有真实数据佐证**
的演示, 而不是靠嘴讲。

为什么是两幕 (设计取舍, 用户已拍板「守护优先, Agent 只读不写灯」)
----------------------------------------------------------------
快层守护 (edgevision_patrol_daemon) 和具身 Agent (rkllm_embodied_agent) 共用
/sys/class/leds/work/ 这一个物理 LED。两者同时写灯会互相覆盖, 谁也说不清最后是谁点亮的。
用户决策: **守护优先, Agent 只读不写灯**。所以演示拆成两幕:

  第一幕 守护自主闭环 (快层, 无 LLM)
      阈值临时降到 20°C -> 守护 5s 内检测到过热 -> 真实爆闪 -> 写事件日志
      这一步的「巡检 / 研判 / 执行」全部由守护完成, 不消耗一毫秒 NPU
  第二幕 Agent 多步研判汇报 (慢层, LLM)
      一条自然语言指令 -> Agent 自主连调多个感知工具 -> 综合研判 -> 中文汇报
      Agent 带 --no-led, 只读硬件状态和守护事件, 绝不抢灯

两幕合起来才是完整的「快慢双系统」: 快层保证毫秒级反应且不占 NPU, 慢层负责需要理解
和解释的研判。这比「让 LLM 每 5 秒跑一次」在时延、NPU 占用、可靠性上都更合理
(理由见 docs/PATROL_DAEMON_DESIGN.md 第 5 节)。

判定原则 (与项目总纲一致: 不许用假数据冒充结果)
--------------------------------------------
1. 灯有没有真闪, **只认 /sys/class/leds/work/brightness 的实测采样**, 不信程序自述;
2. 事件有没有真发生, **只认 /root/patrol/patrol_events.log 的落盘内容**;
3. Agent 有没有真调工具, **只认 stdout 上的契约行** (⚙️ [Agent 决策] 触发物理硬件工具:xxx);
4. 任何一项拿不到证据就判 FAIL, 不做「大概成功了」的模糊结论。

用法
----
    python lab/agent_multistep_demo.py                 # 完整两幕演示
    python lab/agent_multistep_demo.py --act1-only     # 只跑守护真实闪灯
    python lab/agent_multistep_demo.py --act2-only     # 只跑 Agent 多步研判
    python lab/agent_multistep_demo.py --keep-daemon   # 结束后不杀守护(方便人工观察)

产物: <out-dir>/demo_<时间戳>/{demo_report.md, trace.json, led_samples.txt, agent_raw.txt}
"""

import argparse
import base64
import json
import os
import re
import shlex
import subprocess
import sys
import time
from datetime import datetime

# --------------------------------------------------------------------------
# 板端路径与常量
# --------------------------------------------------------------------------
REMOTE_DIR = "/root/rkllm_deploy"              # Agent 二进制目录(模型是相对路径, 必须 cd)
AGENT_BIN = "rkllm_embodied_agent_p0"

PATROL_DIR = "/root/lab"
PATROL_BIN = "edgevision_patrol_daemon"
PATROL_EVENTS = "/root/patrol/patrol_events.log"
PATROL_LLM_LOG = "/root/patrol/llm_pending.jsonl"
PATROL_LOG = "/tmp/demo_patrol.log"
PATROL_PIDFILE = "/tmp/demo_patrol.pid"
# 进程匹配锚点: 必须以 "./" 开头匹配**守护自己的 argv[0]**, 而不是 ssh 里那句
# bash -c 包装命令 —— 后者命令行里也含二进制名, 用宽松的 'pgrep -f <name>' 会匹配到
# 自己这层 shell, 于是"停不掉/已启动"的判断全是假的(第一版就踩了这个)。
PATROL_MATCH = r"^\./edgevision_patrol_daemon"

LED_TRIGGER = "/sys/class/leds/work/trigger"
LED_BRIGHTNESS = "/sys/class/leds/work/brightness"

SSH_OPTS = ["-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            "-o", "StrictHostKeyChecking=accept-new"]

# 验收用的临时阈值: 远低于测温实际值(30~45°C) -> 必然触发 THERMAL_ALERT,
# 从而验证「检测 -> 研判 -> 执行(真实闪灯)」整条路径, 而不必真的把板子烤到 75°C。
DEMO_TEMP_ENTER = "20"
DEMO_TEMP_EXIT = "18"
DEMO_INTERVAL = "2"

# 契约行正则 —— 与 agent_regression_test.py 保持完全一致, 只锚中文标记不锚 emoji
RE_USER = re.compile(r"\[User\]:(.*)")
RE_TOOL = re.compile(r"\[Agent 决策\]\s*触发物理硬件工具:\s*(\S+)")
RE_ARG = re.compile(r"设定参数:\s*(\S+)\s*=\s*(.*)")
RE_OBS = re.compile(r"\[硬件感知\s*\(Observation\)\]\s*返回:\s*(.*)$")
RE_STEP_LIMIT = re.compile(r"工具执行轮数已达上限")

# 第二幕交给模型的一句话。
# 刻意用「①②③」把任务拆成显式子步骤: 既压住模型跳步的倾向, 又天然要求它连调多个工具,
# 从而真正跑出「多步」而不是「一问一答」。
AGENT_QUERY = (
    "请做一次完整巡检并汇报："
    "① 查开发板整体硬件状态（温度、NPU负载、可用内存）；"
    "② 读巡检守护最近报过的告警事件；"
    "③ 结合这两项数据判断开发板当前是否处于异常状态，"
    "说明板载指示灯现在应该处于什么状态，并给我处置建议。"
)

# 第二幕判定门槛: 至少两个**不同**的感知工具才算真多步 (单工具=一问一答, 不算闭环)
MIN_DISTINCT_TOOLS = 2


# --------------------------------------------------------------------------
# SSH 基础设施 (与回归测试器同款: 纯 list 传参, 不经 Windows shell 转义)
# --------------------------------------------------------------------------
def sh_run(host, remote_cmd, timeout_s, stdin_bytes=None):
    """执行一条远程命令, 返回 (returncode, stdout, stderr)。
    TIMEOUT 用字符串 'TIMEOUT' 表示, 便于和数字 rc 区分。"""
    cmd = ["ssh"] + SSH_OPTS + [host, remote_cmd]
    try:
        p = subprocess.run(cmd, input=stdin_bytes,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout_s)
        return (p.returncode,
                p.stdout.decode("utf-8", errors="replace"),
                p.stderr.decode("utf-8", errors="replace"))
    except subprocess.TimeoutExpired as e:
        return ("TIMEOUT",
                (e.stdout or b"").decode("utf-8", errors="replace"),
                (e.stderr or b"").decode("utf-8", errors="replace"))


def sh_ok(host, remote_cmd, timeout_s=20):
    """只关心成功与否的便捷封装。"""
    rc, out, err = sh_run(host, remote_cmd, timeout_s)
    return rc == 0, out, err


def b64(host, text, timeout_s=20):
    """把中文文本安全送到板端 (base64 过线, 规避 Windows 编码坑)。"""
    raw = base64.b64encode(text.encode("utf-8")).decode("ascii")
    return sh_run(host, "echo %s | base64 -d" % raw, timeout_s)


# --------------------------------------------------------------------------
# 第一幕: 守护自主闭环 —— 真实告警 + 真实闪灯
# --------------------------------------------------------------------------
def read_led_state(host):
    """读一次 LED 的 trigger 和 brightness, 返回 (trigger, brightness:int|None)。"""
    rc, out, _ = sh_run(host, "cat %s; echo '---'; cat %s" %
                        (LED_TRIGGER, LED_BRIGHTNESS), 15)
    if rc != 0:
        return None, None
    parts = out.split("---")
    trig = parts[0].strip() if parts else ""
    bright = None
    if len(parts) > 1:
        try:
            bright = int(parts[1].strip())
        except ValueError:
            bright = None
    return trig, bright


# 采样间隔为什么是 0.02s 而不是 0.1s（两个都必须躲的坑）
# 坑 1 · 混叠锁定: 爆闪是 100ms 亮/100ms 灭的方波。若采样间隔也取 0.1s，采样
#   周期与方波周期**同频**，读到的相位被锁死 -> 可能连续几十次全是 255 或全是 0，
#   于是把「在闪」误判成「常亮/常灭」。采样间隔必须远小于方波半周期，且避开整数倍。
# 坑 2 · heartbeat 本身就在闪: 板上已确认 heartbeat 触发器运行时 brightness 也在
#   0/非0 之间跳变(实测 3 秒内 68/300 次非零，占空比约 22%)。所以「有没有翻转」
#   **完全无法区分 alarm 与 heartbeat** —— 那是假验证。
#   唯一可靠的判据是**占空比**: alarm 是对称方波(占空比≈50%)，
#   heartbeat 是双脉冲+长暗期(占空比≈20%)。故本脚本先取 heartbeat 基线，
#   再比告警期间的占空比是否显著抬升。
ALARM_DUTY_MIN = 0.35     # alarm 占空比下限(理论 0.5, 留余量)
ALARM_DUTY_LIFT = 0.15    # 相对 heartbeat 基线至少要抬升这么多


def sample_led(host, duration_s=6.0, interval_s=0.02):
    """连续采样 brightness, 返回 (采样序列 list[int], 原始输出)。

    一次 SSH 里跑完整个采样循环，避免逐次往返把采样率拖到不可用。
    """
    n = int(duration_s / interval_s)
    loop = ("i=0; while [ $i -lt %d ]; do cat %s; sleep %s; i=$((i+1)); done"
            % (n, LED_BRIGHTNESS, interval_s))
    rc, out, err = sh_run(host, loop, int(duration_s) + 60)
    if rc != 0 and not out:
        return [], (out + err)
    vals = []
    for line in (out or "").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            vals.append(int(line))
        except ValueError:
            pass
    return vals, (out + err)


def count_flips(vals):
    """统计采样序列里「亮<->灭」的翻转次数。亮=非0, 灭=0。"""
    flips = 0
    prev = None
    for v in vals:
        cur = (v != 0)
        if prev is not None and cur != prev:
            flips += 1
        prev = cur
    return flips


def duty_cycle(vals):
    """亮灯时间占比。这是区分 alarm 与 heartbeat 的唯一可靠判据。"""
    if not vals:
        return 0.0
    return sum(1 for v in vals if v != 0) / float(len(vals))


def baseline_heartbeat_duty(host, duration_s=3.0):
    """取 heartbeat 常态占空比基线。

    必须先测基线再触发告警: 没有基线就无法证明告警期间的占空比抬升是
    「alarm 生效」而不是「板子本来就这样」。
    """
    sh_run(host, "echo heartbeat > %s" % LED_TRIGGER, 20)
    time.sleep(0.8)
    vals, _ = sample_led(host, duration_s=duration_s, interval_s=0.02)
    return duty_cycle(vals), vals


def tail_file(host, path, n=10, max_bytes=65536):
    """读远端文件尾部 n 行 (守护日志超过 5MB 会轮转, 别整文件读)。"""
    cmd = ("if [ -f {p} ]; then tail -c {b} {p} | tail -n {n}; "
           "else echo '__NO_FILE__'; fi").format(
               p=shlex.quote(path), b=max_bytes, n=int(n))
    rc, out, _ = sh_run(host, cmd, 20)
    return out if rc == 0 else ""


def patrol_meta(host):
    """检查守护二进制与它支持的参数, 返回 dict(存在/支持阈值参数/已支持单实例锁)。"""
    exists, _, _ = sh_ok(host, "test -x %s/%s && echo yes" %
                         (PATROL_DIR, PATROL_BIN), 15)
    if not exists:
        return {"exists": False, "temp_flags": False, "lock": False}

    _, helps, _ = sh_run(host, "cd %s && ./%s --help 2>&1 | head -40" %
                         (PATROL_DIR, PATROL_BIN), 20)
    _, strings_out, _ = sh_run(
        host, "grep -a -o -m1 -- '--temp-enter' %s/%s 2>/dev/null" %
        (PATROL_DIR, PATROL_BIN), 20)
    temp_flags = ("temp-enter" in (helps or "")) or bool((strings_out or "").strip())
    lock = "flock" in (helps or "") or "单实例" in (helps or "")
    return {"exists": True, "temp_flags": temp_flags, "lock": lock,
            "help": (helps or "").strip()}


def stop_patrol(host):
    """按 pid 文件优雅停守护, 再兜底精确匹配; 返回 (是否已无残留, 残留 pid 文本)。

    注意匹配锚点: 必须匹配 `./edgevision_patrol_daemon` 这个 argv[0] 开头,
    不能模糊匹配二进制名 —— ssh 那层 `bash -c "...edgevision_patrol_daemon..."`
    的命令行里同样含这个字符串, 模糊匹配会把自己这层 shell 当成守护, 导致
    "停不掉 / 还在跑" 的误判(第一版实测就踩了: 报残留 pid 3430, 那其实是 ssh 的 shell)。
    """
    sh_run(host, "if [ -f %s ]; then kill -TERM $(cat %s) 2>/dev/null; fi" %
           (PATROL_PIDFILE, PATROL_PIDFILE), 20)
    time.sleep(1.5)
    # 兜底: pid 文件可能因异常退出而失效
    sh_run(host, "pkill -TERM -f '%s' 2>/dev/null; true" % PATROL_MATCH, 20)
    time.sleep(1.0)
    sh_run(host, "pkill -KILL -f '%s' 2>/dev/null; true" % PATROL_MATCH, 20)
    time.sleep(0.3)
    _, out, _ = sh_run(host, "pgrep -af '%s' 2>/dev/null | head -5" % PATROL_MATCH, 15)
    leftover = (out or "").strip()
    return (not leftover), leftover


def wait_patrol_up(host, pid, tries=10, delay=0.4):
    """确认守护进程真的活着(而不是 nohup 那层 fork 完就退)。

    为什么要单独验: `nohup ... &` 的退出码只反映**启动动作**是否完成,
    与守护进程是否成功常驻无关。真实判据是 kill -0 能否看到这个 pid。
    """
    for _ in range(tries):
        rc, out, _ = sh_run(host, "kill -0 %s 2>/dev/null && echo alive" % pid, 15)
        if rc == 0 and "alive" in (out or ""):
            return True
        time.sleep(delay)
    return False


def act1_daemon_alert(host, verbose=True):
    """第一幕: 起守护(临时低阈值) -> 等真实告警 -> 采样证明灯真在闪 -> 读事件日志落盘。

    返回 dict(ok, checks[], led_samples, events_tail, llm_tail, notes[])
    """
    art = {"ok": False, "checks": [], "led_samples": [], "events_tail": "",
           "llm_tail": "", "notes": [], "daemon_flags": []}

    def check(name, passed, detail):
        art["checks"].append({"name": name, "pass": bool(passed),
                              "detail": detail})
        if verbose:
            print("  [%s] %s — %s" % ("✔" if passed else "✘", name, detail))
        return passed

    meta = patrol_meta(host)
    if not check("守护二进制可用", meta["exists"], "%s/%s" % (PATROL_DIR, PATROL_BIN)):
        return art
    check("支持阈值参数", meta["temp_flags"],
          "有 --temp-enter/--temp-exit" if meta["temp_flags"]
          else "未检测到阈值参数, 无法在不重编译的前提下降阈值")
    check("支持单实例锁", meta["lock"], "flock" if meta["lock"] else "未检测到")

    # 1) 记录 LED 起点, 并备份守护日志(演示只关心本次产生的事件)
    trig0, bright0 = read_led_state(host)
    if verbose:
        print("  起点 LED: trigger=%s brightness=%s" % (trig0, bright0))
    sh_run(host, "mkdir -p %s && touch %s %s" %
           (os.path.dirname(PATROL_EVENTS), PATROL_EVENTS, PATROL_LLM_LOG), 20)
    _, before, _ = sh_run(host, "wc -l %s %s 2>/dev/null" %
                          (PATROL_EVENTS, PATROL_LLM_LOG), 20)
    art["notes"].append("演示前日志行数: %s" % (before or "").strip().replace("\n", " / "))

    # 1.5) 先取 heartbeat 占空比基线 —— 没有基线就没法证明后面的占空比抬升是 alarm 造成的
    if verbose:
        print("  采集 heartbeat 基线占空比 3s ...")
    base_duty, base_vals = baseline_heartbeat_duty(host, duration_s=3.0)
    art["baseline_duty"] = base_duty
    check("heartbeat 基线占空比合理", 0.05 <= base_duty <= 0.45,
          "%.1f%% (heartbeat 为双脉冲+长暗期, 预期 15~30%%)"
          % (base_duty * 100))

    # 2) 起守护: 临时把过热阈值压到 20°C (实测板温 30~45°C -> 必然越限)
    sh_run(host, "pkill -TERM -f '%s' 2>/dev/null; sleep 1; true" % PATROL_MATCH, 20)
    if meta["temp_flags"]:
        launch = (
            "cd %s && nohup ./%s --interval %s --temp-enter %s --temp-exit %s "
            "> %s 2>&1 & echo $! > %s; sleep 0.5; cat %s"
        ) % (PATROL_DIR, PATROL_BIN, DEMO_INTERVAL,
             DEMO_TEMP_ENTER, DEMO_TEMP_EXIT, PATROL_LOG, PATROL_PIDFILE,
             PATROL_PIDFILE)
        art["daemon_flags"] = ["--interval", DEMO_INTERVAL,
                               "--temp-enter", DEMO_TEMP_ENTER,
                               "--temp-exit", DEMO_TEMP_EXIT]
    else:
        # 兜底: 老二进制没有阈值参数 -> 只能 dry-run, 第一幕会诚实判 FAIL
        art["notes"].append("守护不支持阈值参数, 退化为 --dry-run 观察(不会真闪灯)")
        launch = (
            "cd %s && nohup ./%s --interval %s --dry-run "
            "> %s 2>&1 & echo $! > %s; sleep 0.5; cat %s"
        ) % (PATROL_DIR, PATROL_BIN, DEMO_INTERVAL, PATROL_LOG,
             PATROL_PIDFILE, PATROL_PIDFILE)

    rc, pid_out, err = sh_run(host, launch, 30)
    pid = (pid_out or "").strip()
    # 关键: nohup 的退出码只说明"启动动作"完成, 不代表守护真活着。
    # 真判据 = kill -0 能看到这个 pid (否则进程可能一起来就崩了)。
    alive = pid.isdigit() and wait_patrol_up(host, pid)
    check("守护已启动并常驻", alive,
          "pid=%s, kill -0 通过" % pid if alive
          else "pid=%s, 进程未能存活 (日志: %s)"
          % (pid or "?", tail_file(host, PATROL_LOG, n=5).strip()[:200]))
    if not alive:
        return art

    # 3) 给守护几个周期把状态机推到 THERMAL_ALERT
    #    interval=2s + 进入条件是瞬时 >enter, 因此 6s 内必然翻转; 留 8s 余量。
    if verbose:
        print("  等待守护检测到过热 (约 8s) ...")
    time.sleep(8)

    # 4) 关键证据: 采样占空比, 对比 heartbeat 基线
    #
    # 为什么不能只看「有没有翻转」: heartbeat 自身就在 0/非0 之间跳,
    # 翻转次数根本无法区分 alarm 与 heartbeat。必须比**占空比**:
    # alarm = 对称方波(≈50%), heartbeat = 双脉冲+长暗期(≈20%)。
    if verbose:
        print("  采样 LED 6s (对比 heartbeat 基线占空比) ...")
    vals, _ = sample_led(host, duration_s=6.0, interval_s=0.02)
    art["led_samples"] = vals
    duty = duty_cycle(vals)
    flips = count_flips(vals)
    base_duty = art.get("baseline_duty") or 0.0
    lift = duty - base_duty
    art["alarm_duty"] = round(duty, 3)
    art["alarm_flips"] = flips
    check("LED 占空比抬升到爆闪水平", duty >= ALARM_DUTY_MIN and lift >= ALARM_DUTY_LIFT,
          "告警占空比 %.1f%% vs heartbeat 基线 %.1f%% (抬升 %+.1f 个百分点), "
          "翻转 %d 次, 采样 %d 点"
          % (duty * 100, base_duty * 100, lift * 100, flips, len(vals)))

    # 5) 事件落盘: patrol_events.log 必须出现 THERMAL_ALERT
    ev = tail_file(host, PATROL_EVENTS, n=10)
    art["events_tail"] = ev
    alerted = "THERMAL_ALERT" in ev
    check("事件日志记录 THERMAL_ALERT", alerted,
          (ev.strip().splitlines()[-1] if ev.strip() else "(空)")[:160])

    # 6) 慢层事件队列: llm_pending.jsonl 必须同步出事件 (第二幕的输入)
    lm = tail_file(host, PATROL_LLM_LOG, n=5)
    art["llm_tail"] = lm
    check("llm_pending.jsonl 有事件", "THERMAL_ALERT" in lm,
          (lm.strip().splitlines()[-1] if lm.strip() else "(空)")[:160])

    # 7) 守护自身是否还活着 (没崩溃)
    alive, out, _ = sh_ok(host, "kill -0 $(cat %s) 2>/dev/null && echo alive" %
                          PATROL_PIDFILE, 15)
    check("守护仍在运行", alive and "alive" in (out or ""), "kill -0 通过")

    art["ok"] = all(c["pass"] for c in art["checks"])
    return art


# --------------------------------------------------------------------------
# 第二幕: Agent 多步研判汇报 —— 一条自然语言指令跑完
# --------------------------------------------------------------------------
def agent_supports_no_led(host):
    """探测二进制是否支持 --no-led。

    为什么要探: main() 是「有 argv[1] 就当单轮 query」, 如果不支持还硬传 --no-led,
    它会被当成**用户问题**跑一轮, 产生看起来正常实则无关的输出 —— 必须提前识别。
    """
    rc, out, _ = sh_run(
        host, "grep -a -o -m1 -- '--no-led' %s/%s 2>/dev/null" %
        (REMOTE_DIR, AGENT_BIN), 20)
    return bool((out or "").strip())


def run_agent(host, query, use_no_led, timeout_s=300):
    """在板上跑一轮 Agent, 返回 (rc, stdout, stderr, wall_s)。"""
    argv = "./" + AGENT_BIN
    if use_no_led:
        argv += " --no-led"
    raw = base64.b64encode(query.encode("utf-8")).decode("ascii")
    remote = ("echo %s | base64 -d > /tmp/demo_query.txt && "
              "cd %s && timeout %d %s \"$(cat /tmp/demo_query.txt)\"; "
              "rc=$?; rm -f /tmp/demo_query.txt; exit $rc") % (
                  raw, shlex.quote(REMOTE_DIR), timeout_s, argv)
    t0 = time.time()
    rc, out, err = sh_run(host, remote, timeout_s + 30)
    return rc, out, err, round(time.time() - t0, 1)


def parse_trace(stdout_text):
    """按契约行把 stdout 拆成回合, 抽工具链/参数/Observation/回复正文。

    与 agent_regression_test.py:parse_turns 同源逻辑, 单独实现以免演示脚本
    依赖测试脚本内部实现(两者解耦, 任一改动不影响另一个)。
    """
    turns = []
    cur = None
    lines = stdout_text.splitlines()

    def new_turn(q):
        return {"query": q, "tools": [], "args": [], "obs": [],
                "obs_ok": True, "body": []}

    for idx, raw_line in enumerate(lines):
        line = raw_line.rstrip("\r")
        m = RE_USER.search(line)
        if m:
            if cur is not None:
                turns.append(cur)
            cur = new_turn(m.group(1).strip())
            continue
        if cur is None:
            continue
        mt = RE_TOOL.search(line)
        if mt:
            cur["tools"].append(mt.group(1).strip())
            continue
        ma = RE_ARG.search(line)
        if ma:
            val = ma.group(2).strip()
            if not val:
                # 兼容「值跑到下一行」: 往下最多 3 行找第一行非契约内容
                for j in range(idx + 1, min(idx + 4, len(lines))):
                    cand = lines[j].strip()
                    if not cand:
                        continue
                    if (RE_USER.search(cand) or RE_TOOL.search(cand)
                            or RE_ARG.search(cand) or RE_OBS.search(cand)):
                        break
                    val = cand
                    break
            tool = cur["tools"][-1] if cur["tools"] else "?"
            cur["args"].append({"tool": tool, "param": ma.group(1).strip(),
                                "value": val})
            continue
        mo = RE_OBS.search(line)
        if mo:
            payload = mo.group(1).strip()
            seg = None
            if "[" in payload and "]" in payload:
                seg = payload[payload.index("["):payload.rindex("]") + 1]
            if seg is not None:
                try:
                    data = json.loads(seg)
                    if not isinstance(data, list):
                        data = [data]
                    cur["obs"].extend(d for d in data if isinstance(d, dict))
                except (ValueError, TypeError):
                    cur["obs_ok"] = False
            else:
                cur["obs_ok"] = False
            continue
        cur["body"].append(line)

    if cur is not None:
        turns.append(cur)

    markers = ("[Agent 决策]", "设定参数:", "(Observation)]", "==========",
               "----------", "[Edge Agent", "[User]:")
    for t in turns:
        t["reply"] = "\n".join(
            ln for ln in t["body"] if not any(mk in ln for mk in markers)).strip()
    return turns


def act2_agent_multistep(host, verbose=True, timeout_s=300):
    """第二幕: 一条自然语言 -> Agent 多步研判 -> 中文汇报。"""
    art = {"ok": False, "checks": [], "tools": [], "distinct": 0,
           "obs": [], "reply": "", "raw": "", "wall_s": 0, "no_led": False,
           "step_limit_hit": False}

    def check(name, passed, detail):
        art["checks"].append({"name": name, "pass": bool(passed),
                              "detail": detail})
        if verbose:
            print("  [%s] %s — %s" % ("✔" if passed else "✘", name, detail))
        return passed

    exists, _, _ = sh_ok(host, "test -x %s/%s && echo yes" %
                         (REMOTE_DIR, AGENT_BIN), 15)
    if not check("Agent 二进制可用", exists, "%s/%s" % (REMOTE_DIR, AGENT_BIN)):
        return art

    # 守护正在控灯 -> Agent 必须以只读模式跑, 否则会抢灯(用户已拍板守护优先)
    use_no_led = agent_supports_no_led(host)
    art["no_led"] = use_no_led
    check("Agent 支持 --no-led", use_no_led,
          "以只读模式运行, 不抢守护的灯" if use_no_led
          else "未支持 --no-led, 本幕可能覆盖守护的告警灯")

    if verbose:
        print("  下发一条自然语言巡检指令 ...")
    rc, out, err, wall = run_agent(host, AGENT_QUERY, use_no_led, timeout_s)
    art["raw"] = out or ""
    art["wall_s"] = wall
    safe_rc = rc if isinstance(rc, int) else -1
    check("Agent 进程正常退出", safe_rc == 0,
          "rc=%s, 耗时 %.1fs" % (rc, wall))
    if safe_rc != 0:
        art["raw"] += "\n[stderr]\n" + (err or "")
        return art

    turns = parse_trace(out or "")
    turn = turns[0] if turns else None
    if turn is None:
        check("解析到 Agent 回合", False, "stdout 无 [User]: 契约行")
        return art
    check("解析到 Agent 回合", True, "query=%s…" % turn["query"][:24])

    tools = turn["tools"]
    distinct = len(set(tools))
    art["tools"] = tools
    art["distinct"] = distinct
    art["obs"] = turn["obs"]
    art["reply"] = turn["reply"]
    art["step_limit_hit"] = bool(RE_STEP_LIMIT.search(out or ""))

    check("真实多步(≥%d 个不同工具)" % MIN_DISTINCT_TOOLS,
          distinct >= MIN_DISTINCT_TOOLS,
          "工具链=%s (不同 %d 个)" % (tools or ["(无)"], distinct))
    check("Observation 均为合法 JSON", bool(turn["obs_ok"]),
          "%d 个对象" % len(turn["obs"]))
    check("未触发步数上限", not art["step_limit_hit"],
          "max_steps 内完成" if not art["step_limit_hit"] else "打到了上限告警")
    check("输出了中文研判结论", len(turn["reply"]) >= 20,
          "%d 字" % len(turn["reply"]))

    art["ok"] = all(c["pass"] for c in art["checks"])
    return art


# --------------------------------------------------------------------------
# 报告落盘
# --------------------------------------------------------------------------
def write_report(out_dir, meta, act1, act2):
    os.makedirs(out_dir, exist_ok=True)

    trace = {"meta": meta, "act1_daemon": act1, "act2_agent": act2}
    with open(os.path.join(out_dir, "trace.json"), "w", encoding="utf-8") as f:
        json.dump(trace, f, ensure_ascii=False, indent=2)

    with open(os.path.join(out_dir, "led_samples.txt"), "w",
              encoding="utf-8") as f:
        a1 = act1 or {}
        f.write("# 第一幕 LED brightness 采样序列 (每 0.02s 一次)\n")
        f.write("# 判据: alarm 是对称方波(占空比≈50%), heartbeat 是双脉冲+长暗期(≈20%)\n")
        f.write("# heartbeat 基线占空比: %.1f%%\n" % (a1.get("baseline_duty", 0) * 100))
        f.write("# 告警期占空比      : %.1f%%\n" % (a1.get("alarm_duty", 0) * 100))
        f.write("# 告警期翻转次数    : %s\n" % a1.get("alarm_flips", 0))
        f.write(" ".join(str(v) for v in (a1.get("led_samples") or [])) + "\n")

    with open(os.path.join(out_dir, "agent_raw.txt"), "w",
              encoding="utf-8") as f:
        f.write((act2 or {}).get("raw", "") or "")

    def section(title, art, extra=None):
        lines = ["## %s" % title, ""]
        if art is None:
            lines.append("_未执行_")
            lines.append("")
            return lines
        lines.append("**结论: %s**" % ("PASS ✔" if art.get("ok") else "FAIL ✘"))
        lines.append("")
        lines.append("| 检查项 | 结果 | 证据 |")
        lines.append("|---|---|---|")
        for c in art.get("checks", []):
            lines.append("| %s | %s | %s |" %
                         (c["name"], "✔" if c["pass"] else "✘",
                          c["detail"].replace("|", "\\|")))
        lines.append("")
        if extra:
            lines.append(extra)
            lines.append("")
        return lines

    L = ["# Agent 多步演示闭环 —— 实测报告", ""]
    L.append("- 时间: %s" % meta["timestamp"])
    L.append("- 板子: %s   Agent: %s/%s   守护: %s/%s" %
             (meta["host"], REMOTE_DIR, AGENT_BIN, PATROL_DIR, PATROL_BIN))
    L.append("- 第一幕守护参数: %s" %
             (" ".join(meta.get("daemon_flags") or []) or "(默认)"))
    L.append("- 第二幕 Agent 指令: %s" % AGENT_QUERY)
    L.append("- 第二幕 Agent 模式: %s" %
             ("--no-led (只读, 不抢灯)" if (act2 or {}).get("no_led") else "默认"))
    L.append("")
    L.append("> 判定原则: 灯是否真闪只认 brightness 实测采样; 事件是否真发生只认日志落盘;")
    L.append("> Agent 是否真调工具只认 stdout 契约行。任一项无证据即判 FAIL。")
    L.append("")

    L.append("## 第一幕 · 守护自主闭环（快层, 无 LLM）")
    L.append("")
    L.append("> 阈值临时降到 %s°C（板温实际 30~45°C，必然越限）→ 守护检测到过热 → "
             "真实爆闪 → 写事件日志。" % DEMO_TEMP_ENTER)
    L.append("> 这一步的「巡检/研判/执行」全由守护完成，**不消耗 NPU**。")
    L.append("")
    if act1:
        vals = act1.get("led_samples") or []
        ev = act1.get("events_tail") or ""
        lm = act1.get("llm_tail") or ""
        extra = []
        extra.append("**LED 是否真闪 —— 用占空比判定，不是看有没有翻转**")
        extra.append("")
        extra.append("| 状态 | 占空比 | 说明 |")
        extra.append("|---|---:|---|")
        extra.append("| heartbeat 基线 | %.1f%% | 双脉冲+长暗期 |"
                     % (act1.get("baseline_duty", 0) * 100))
        extra.append("| 告警期间 | %.1f%% | 对称方波，应显著抬升 |"
                     % (act1.get("alarm_duty", 0) * 100))
        extra.append("")
        extra.append("> 关键点：heartbeat **本身就在闪**（实测占空比约 20%），"
                     "所以「看到 brightness 在 0/非0 之间跳」根本证明不了 alarm 生效。")
        extra.append("> 唯一可靠判据是占空比是否从 ~20% 抬到 ~50%。")
        extra.append("")
        extra.append("LED 采样序列（%d 点，每 0.02s）: `%s`" % (len(vals), " ".join(
            str(v) for v in vals[:80] + (["…"] if len(vals) > 80 else []))))
        if ev:
            extra.append("")
            extra.append("`%s` 尾部:" % PATROL_EVENTS)
            extra.append("```")
            extra.append(ev.strip()[:1000])
            extra.append("```")
        if lm:
            extra.append("")
            extra.append("`%s` 尾部:" % PATROL_LLM_LOG)
            extra.append("```")
            extra.append(lm.strip()[:800])
            extra.append("```")
        L.extend(section("第一幕检查项", act1, "\n".join(extra)))
    else:
        L.extend(section("第一幕检查项", None))

    L.append("## 第二幕 · Agent 多步研判汇报（慢层, LLM）")
    L.append("")
    L.append("> 一条自然语言指令 → Agent 自主连调多个感知工具 → 综合研判 → 中文汇报。")
    L.append("> Agent 带 `--no-led`，只读硬件状态与守护事件，**不抢灯**。")
    L.append("")
    if act2:
        extra = []
        extra.append("工具调用链: `%s`" % " → ".join(
            act2.get("tools") or ["(无)"]))
        extra.append("")
        extra.append("耗时: %.1fs" % act2.get("wall_s", 0))
        if act2.get("reply"):
            extra.append("")
            extra.append("模型最终汇报:")
            extra.append("")
            extra.append("```")
            extra.append(act2["reply"][:2000])
            extra.append("```")
        L.extend(section("第二幕检查项", act2, "\n".join(extra)))
    else:
        L.extend(section("第二幕检查项", None))

    L.append("## 两幕合起来的项目意义")
    L.append("")
    L.append("- **快层守护**：5s 周期、纯 C++、零 NPU 占用，毫秒级反应硬件异常；")
    L.append("- **慢层 Agent**：按需唤醒的大模型，负责需要理解和解释的研判与汇报；")
    L.append("- 两者通过 `/root/patrol/llm_pending.jsonl` 这个**落盘契约**解耦，"
             "各自独立崩溃不拖垮对方；")
    L.append("- 这正是「让小模型每 5 秒跑一次」做不到的：NPU 被视觉占满、时延高、"
             "且 LLM 本身可能出错。")

    path = os.path.join(out_dir, "demo_report.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    return path, os.path.join(out_dir, "trace.json")


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------
def main():
    if sys.stdout and hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(
        description="RK3588 具身 Agent 多步演示闭环编排器 (纯标准库)")
    ap.add_argument("--host", default=os.environ.get("EDGEVISION_BOARD_HOST", ""))
    ap.add_argument("--act1-only", action="store_true", help="只跑第一幕(守护真实闪灯)")
    ap.add_argument("--act2-only", action="store_true", help="只跑第二幕(Agent 多步研判)")
    ap.add_argument("--keep-daemon", action="store_true",
                    help="结束后不杀守护(方便人工看灯; 务必随后手动停)")
    ap.add_argument("--agent-timeout", type=int, default=300,
                    help="第二幕 Agent 单轮超时秒数(默认300)")
    ap.add_argument("--out-dir", default=None,
                    help="结果根目录(默认 <工作区>/_test_results)")
    args = ap.parse_args()

    print("=" * 72)
    print("RK3588 具身 Agent 多步演示闭环   host=%s" % args.host)
    print("=" * 72)

    # 预检: SSH 连通
    rc, out, err = sh_run(args.host, "echo ok && uname -nm", 20)
    if rc != 0 or "ok" not in (out or ""):
        print("[FATAL] SSH 预检失败: rc=%s\n%s" % (rc, (err or "")[:500]))
        return 2
    print("[预检] SSH OK: %s\n" % (out or "").replace("ok", "").strip())

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    root = args.out_dir or os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))), "_test_results")
    out_dir = os.path.join(root, "demo_%s" % ts)

    meta = {"timestamp": ts, "host": args.host, "daemon_flags": []}

    trig_before, bright_before = read_led_state(args.host)
    print("[起点] LED trigger=%s brightness=%s\n" % (trig_before, bright_before))

    act1 = act2 = None
    try:
        if not args.act2_only:
            print("── 第一幕: 守护自主闭环（快层, 真实告警 + 真实闪灯）──")
            act1 = act1_daemon_alert(args.host)
            meta["daemon_flags"] = act1.get("daemon_flags") or []
            print("  => 第一幕: %s\n" % ("PASS ✔" if act1["ok"] else "FAIL ✘"))

        if not args.act1_only:
            print("── 第二幕: Agent 多步研判汇报（慢层, 一条自然语言跑完）──")
            act2 = act2_agent_multistep(args.host, timeout_s=args.agent_timeout)
            print("  => 第二幕: %s\n" % ("PASS ✔" if act2["ok"] else "FAIL ✘"))
    finally:
        # 无论成败都要把现场恢复干净: 停守护 + LED 回 heartbeat
        if not args.keep_daemon:
            print("── 现场恢复 ──")
            gone, leftover = stop_patrol(args.host)
            print("  守护已停止: %s%s" % (
                "✔" if gone else "✘",
                "" if gone else " (残留 pid: %s)" % leftover))
            rc2, _, _ = sh_run(args.host,
                               "echo heartbeat > %s" % LED_TRIGGER, 20)
            time.sleep(0.6)
            trig_after, bright_after = read_led_state(args.host)
            print("  LED 恢复: trigger=%s brightness=%s" % (trig_after,
                                                            bright_after))
            meta["led_restore"] = "ok" if "heartbeat" in (trig_after or "") else "failed"
        else:
            print("[--keep-daemon] 守护保持运行, 记得手动停: "
                  "kill -TERM $(cat %s)" % PATROL_PIDFILE)

    rpath, tpath = write_report(out_dir, meta, act1, act2)

    print("\n" + "=" * 72)
    a1 = "PASS" if (act1 and act1["ok"]) else ("SKIP" if act1 is None else "FAIL")
    a2 = "PASS" if (act2 and act2["ok"]) else ("SKIP" if act2 is None else "FAIL")
    print("结论: 第一幕=%s  第二幕=%s" % (a1, a2))
    print("报告: %s" % rpath)
    print("轨迹: %s" % tpath)
    print("=" * 72)

    if act1 is None and act2 is None:
        return 2
    all_ok = all(a["ok"] for a in (act1, act2) if a is not None)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
