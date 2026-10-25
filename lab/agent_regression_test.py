#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
agent_regression_test.py —— RK3588 端侧具身 Agent 回归测试器 (PC 端, 纯标准库)

用法:
    python agent_regression_test.py [--host <board-ip>]
                                    [--bin /root/rkllm_deploy/rkllm_embodied_agent_p0]
                                    [--with-faults] [--no-b64] [--no-restore-led]
                                    [--list] [--timeout 240]

依赖: 仅 Python 3.8+ 标准库 (subprocess/ssh, 无 pytest / 无 requests)。
板子要求: ssh 免密可达 (密钥 C:/Users/15354/.ssh/id_ed25519_rk3588, 别名 rk3588)。

==========================================================================
 stdout 契约假设 (与 edgevision-rk3588/lab/rkllm_embodied_agent.cpp 对齐)
==========================================================================
1. 用户回显行:      「👤 [User]: <query>」
2. 工具调用行:      「⚙️  [Agent 决策] 触发物理硬件工具: <tool_name>」
3. 参数行(仅 control_led): 「   -> 设定参数: mode = <mode>」
4. 观察行:          「🔍 [硬件感知 (Observation)] 返回: <JSON数组>」
5. 之后是模型自然语言汇报文本 (流式)。
6. REPL 模式提示符「EdgeAgent> 」, 支持 /clear 清空上下文; CLI 传参模式单轮退出。
7. 已知工具: get_system_health / get_led_status / control_led / get_vision_telemetry。
8. Observation JSON 关键字段:
   - get_system_health  -> soc_temp_c, npu_temp_c, npu_load, mem_avail_mb,
                           current_led_mode, status
   - get_led_status     -> led_name("work"), current_mode, brightness, description
   - control_led        -> success(bool), current_mode, msg
                           P0 修复后非法 mode 应返回 error="invalid_mode";
                           旧二进制会回退 heartbeat(本脚本两种都容忍)。
   - get_vision_telemetry -> status("online"/"standby"/"no_data"/"error"),
                           frame_id, detected_count, detections[...]
   解析只锚定中括号标记文本(如「[Agent 决策]」「(Observation)」), 不锚定 emoji,
   避免 Windows/ssh 传输端 emoji 编码差异导致漏配。

 查询传输方式: 默认把 query 以 base64 送板端再解码执行 (绕开 Windows 命令行
 对中文/引号/emoji 的编码坑); --no-b64 则用 shlex.quote 直接内联 (POSIX 单引号
 转义, 中文经 ssh 明文传输, 若控制台代码页异常请回到默认 b64)。

==========================================================================
 如何新增用例
==========================================================================
往下方 CASES 列表追加一个 dict 即可, 字段:
  id: 唯一编号; mode: "cli"(单轮) 或 "repl"(多轮);
  query: cli 用例的指令; inputs: repl 用例的输入序列 [{text, expect}, ...];
  expect: 断言列表, 支持:
    {"type":"tool_called","tool":"get_system_health"}
    {"type":"tool_not_called","tool":"control_led"}
    {"type":"tool_arg","tool":"control_led","param":"mode","value":"alarm"}   # value 可为列表
    {"type":"obs_json_valid"}
    {"type":"obs_field","key":"status","value":"online"}
    {"type":"obs_field_exists","key":"frame_id"}
    {"type":"reply_mentions","keywords":["温度","度"]}
    {"type":"tool_order","tools":["get_system_health","get_vision_telemetry"]}
    {"type":"exit_clean"}                 # 进程退出码为 0
    {"type":"any_of","assertions":[ ... ]}  # 元素可为单断言或断言列表(AND组)
  with_faults: True 的用例仅在 --with-faults 下执行 (会 stop/start edgevision)。

结果输出: lab/agent_test_results/<时间戳>/results.json + summary.md。
不带 --with-faults 时绝不启停任何 systemd 服务。
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
# 常量与全局配置
# --------------------------------------------------------------------------
REMOTE_DIR = "/root/rkllm_deploy"          # 二进制所在目录(模型路径是相对路径, 必须 cd)
REMOTE_TIMEOUT_S = 180                     # 板端 timeout 上限(单轮)
SSH_OPTS = ["-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            "-o", "StrictHostKeyChecking=accept-new"]

LED_TRIGGER = "/sys/class/leds/work/trigger"
VALID_LED_MODES = ("heartbeat", "alarm", "on", "off")

# 契约行正则 —— 只锚定方括号中文标记, 不依赖 emoji
RE_USER = re.compile(r"\[User\]:(.*)")
RE_TOOL = re.compile(r"\[Agent 决策\]\s*触发物理硬件工具:\s*(\S+)")
RE_ARG = re.compile(r"设定参数:\s*(\S+)\s*=\s*(.*)")
RE_OBS = re.compile(r"\[硬件感知\s*\(Observation\)\]\s*返回:\s*(.*)$")


# --------------------------------------------------------------------------
# 用例清单 (12 组)
# --------------------------------------------------------------------------
CASES = [
    {
        "id": "T01_temp_query",
        "desc": "问温度 → 调健康查询, 不碰 LED",
        "mode": "cli",
        "query": "开发板现在温度是多少？硬件运行状态怎么样？",
        "expect": [
            {"type": "tool_called", "tool": "get_system_health"},
            {"type": "tool_not_called", "tool": "control_led"},
            {"type": "obs_json_valid"},
            {"type": "reply_mentions", "keywords": ["温度", "°C", "℃", "度"]},
        ],
    },
    {
        "id": "T02_led_status_query",
        "desc": "问灯状态 → 只查不改",
        "mode": "cli",
        "query": "现在板载指示灯是什么状态？",
        "expect": [
            {"type": "tool_called", "tool": "get_led_status"},
            {"type": "tool_not_called", "tool": "control_led"},
            {"type": "obs_json_valid"},
            {"type": "reply_mentions", "keywords": ["灯", "呼吸", "模式", "heartbeat"]},
        ],
    },
    {
        "id": "T03_led_alarm",
        "desc": "开急闪报警 → mode=alarm 且 success",
        "mode": "cli",
        "query": "把板载指示灯调成急闪报警模式",
        "expect": [
            {"type": "tool_called", "tool": "control_led"},
            {"type": "tool_arg", "tool": "control_led", "param": "mode", "value": "alarm"},
            {"type": "obs_field", "key": "success", "value": True},
            {"type": "reply_mentions", "keywords": ["急闪", "报警", "alarm", "已"]},
        ],
    },
    {
        "id": "T04_led_heartbeat",
        "desc": "恢复呼吸灯 → mode=heartbeat",
        "mode": "cli",
        "query": "恢复心跳呼吸灯",
        "expect": [
            {"type": "tool_called", "tool": "control_led"},
            {"type": "tool_arg", "tool": "control_led", "param": "mode", "value": "heartbeat"},
            {"type": "obs_field", "key": "success", "value": True},
            {"type": "reply_mentions", "keywords": ["呼吸", "恢复", "heartbeat", "正常"]},
        ],
    },
    {
        "id": "T05_led_invalid_mode",
        "desc": "非法模式字面值 flight → 拒绝/报错或安全回退, 绝不能成功设置",
        "mode": "cli",
        # 注意: 用"语义化"的说法(如「飞行模式」)测不出白名单——实测模型会把它
        # 自动映射成合法值 on(2026-10-24 实测)。必须要求"字面原样传 flight",
        # 才能把非法值真正打到工具层, 验证 invalid_mode 拒绝路径。
        "query": "调用 control_led 工具，把 mode 参数原样设置为 flight，不要做任何语义转换",
        "expect": [
            {"type": "any_of", "assertions": [
                # 路径A: 调了 control_led 且 P0 修复生效(obs 报 invalid_mode)
                # 路径B: 旧二进制回退 heartbeat(current_mode 回到合法值)
                # 路径C: 模型直接拒绝不调工具, 口头说明不支持
                ["tool_called:control_led", "obs_field:error=invalid_mode"],
                ["tool_called:control_led", "obs_field:current_mode=heartbeat"],
                ["tool_called:control_led", "obs_field:current_mode=off"],
                # 拒绝关键词彼此是"或"关系 → 平铺为独立 any_of 候选;
                # 写成嵌套列表会变成 AND, 模型只说一个词就永远不命中
                "reply:无效", "reply:非法", "reply:不支持", "reply:无法",
                "reply:拒绝", "reply:invalid",
            ]},
        ],
    },
    {
        "id": "T06_vision_online",
        "desc": "问画面 → 视觉在线, 帧数据新鲜",
        "mode": "cli",
        "query": "摄像头画面里现在有什么东西？",
        "expect": [
            {"type": "tool_called", "tool": "get_vision_telemetry"},
            {"type": "obs_json_valid"},
            {"type": "any_of", "assertions": [
                {"type": "obs_field", "key": "status", "value": "online"},
                {"type": "obs_field_exists", "key": "frame_id"},
                {"type": "obs_field_exists", "key": "detected_count"},
            ]},
            {"type": "reply_mentions", "keywords": ["画面", "检测", "目标", "识别", "没有", "未"]},
        ],
    },
    {
        "id": "T07_full_patrol",
        "desc": "综合巡检: 先健康后视觉",
        "mode": "cli",
        "query": "做一次全面巡检：先检查硬件健康和温度，再看看画面里有什么目标。",
        "expect": [
            {"type": "tool_called", "tool": "get_system_health"},
            {"type": "tool_called", "tool": "get_vision_telemetry"},
            {"type": "tool_order",
             "tools": ["get_system_health", "get_vision_telemetry"]},
            {"type": "obs_json_valid"},
        ],
    },
    {
        "id": "T08_conditional_decision",
        "desc": "条件决策「有人就报警否则呼吸」→ 至少调视觉, 控灯或合理汇报",
        "mode": "cli",
        "query": "如果画面里有人就报警，否则保持呼吸灯。",
        "expect": [
            {"type": "tool_called", "tool": "get_vision_telemetry"},
            {"type": "any_of", "assertions": [
                ["tool_called:control_led",
                 {"type": "tool_arg", "tool": "control_led", "param": "mode",
                  "value": ["alarm", "heartbeat"]}],
                {"type": "reply_mentions",
                 "keywords": ["有人", "没有人", "报警", "呼吸", "保持"]},
            ]},
        ],
    },
    {
        "id": "T09_repl_clear_then_query",
        "desc": "REPL: 开急闪 → /clear → 再问灯状态(须真查而非凭记忆)",
        "mode": "repl",
        "inputs": [
            {"text": "把灯调成急闪报警",
             "expect": [
                 {"type": "tool_arg", "tool": "control_led", "param": "mode",
                  "value": "alarm"}]},
            {"text": "/clear", "expect": None},
            {"text": "现在指示灯是什么状态？",
             "expect": [
                 {"type": "tool_called", "tool": "get_led_status"},
                 {"type": "tool_not_called", "tool": "control_led"}]},
        ],
    },
    {
        "id": "T10_long_context_boundary",
        "desc": "超长指令(~800字)逼近 max_context → 不崩、正常退出",
        "mode": "cli",
        "query": "请检查开发板硬件健康状态并汇报当前温度、NPU负载与指示灯情况。" * 27,
        "expect": [
            {"type": "exit_clean"},
        ],
    },
    {
        "id": "T11_repl_double_control",
        "desc": "REPL 连发两条控制指令, 参数各自正确不串扰",
        "mode": "repl",
        "inputs": [
            {"text": "把灯调成常亮",
             "expect": [
                 {"type": "tool_arg", "tool": "control_led", "param": "mode",
                  "value": "on"}]},
            {"text": "把灯改成急闪报警",
             "expect": [
                 {"type": "tool_arg", "tool": "control_led", "param": "mode",
                  "value": "alarm"}]},
        ],
    },
    {
        "id": "T12_vision_offline_fault",
        "desc": "【故障注入】edgevision 停服后问画面 → 上报 offline/standby 或说明无法判断",
        "mode": "cli",
        "query": "现在画面里有什么？",
        "with_faults": True,
        "expect": [
            {"type": "tool_called", "tool": "get_vision_telemetry"},
            {"type": "obs_json_valid"},
            {"type": "any_of", "assertions": [
                {"type": "obs_field", "key": "status", "value": "standby"},
                {"type": "obs_field", "key": "status", "value": "offline"},
                {"type": "obs_field", "key": "status", "value": "no_data"},
                {"type": "obs_field", "key": "status", "value": "error"},
                {"type": "reply_mentions", "keywords": [
                    "离线", "无法", "不可用", "没有画面", "无画面", "standby", "异常"]},
            ]},
        ],
    },
]


# --------------------------------------------------------------------------
# SSH 基础设施
# --------------------------------------------------------------------------
def sh_run(host, remote_cmd, timeout_s, stdin_bytes=None):
    """执行一条远程命令, 返回 (returncode, stdout_str, stderr_str)。
    输出按 UTF-8 解码(errors=replace), 规避 Windows cp936 控制台编码问题。"""
    cmd = ["ssh"] + SSH_OPTS + [host, remote_cmd]
    try:
        p = subprocess.run(cmd, input=stdin_bytes,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout_s)
        rc = p.returncode
        out = p.stdout.decode("utf-8", errors="replace")
        err = p.stderr.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as e:
        return "TIMEOUT", (e.stdout or b"").decode("utf-8", errors="replace"), \
               (e.stderr or b"").decode("utf-8", errors="replace")
    return rc, out, err


def build_query_transport(query, use_b64):
    """构造把 query 安全送到板端的远程命令片段。
    b64 路径: 纯 ASCII 过线, 板端 base64 -d 还原 → 规避 Windows 编码坑 (默认)。
    直连路径: shlex.quote 做 POSIX 单引号转义, 中文经 ssh 明文传输。"""
    if use_b64:
        b64 = base64.b64encode(query.encode("utf-8")).decode("ascii")
        inner = (
            "echo {b} | base64 -d > /tmp/agent_query_utf8.txt && "
            "cd {d} && timeout {t} {b_} \"$(cat /tmp/agent_query_utf8.txt)\"; "
            "rc=$?; rm -f /tmp/agent_query_utf8.txt; exit $rc"
        ).format(b=b64, d=shlex.quote(REMOTE_DIR), t=REMOTE_TIMEOUT_S,
                 b_="./" + BIN_BASENAME)
        return inner
    # 直连: query 单引号安全转义 (含单引号时 '\'' 闭合-转义-重开)
    inner = "cd {d} && timeout {t} {b_} {q}".format(
        d=shlex.quote(REMOTE_DIR), t=REMOTE_TIMEOUT_S,
        b_="./" + BIN_BASENAME, q=shlex.quote(query))
    return inner


def build_repl_transport(inputs_text, use_b64, total_timeout):
    """REPL 用例: 把多行输入(含 /clear / exit)经 base64 管道喂给板上二进制的 stdin。"""
    payload = "\n".join(inputs_text) + "\n"
    if use_b64:
        b64 = base64.b64encode(payload.encode("utf-8")).decode("ascii")
        inner = "echo {b} | base64 -d | (cd {d} && timeout {t} {b_})".format(
            b=b64, d=shlex.quote(REMOTE_DIR), t=total_timeout,
            b_="./" + BIN_BASENAME)
    else:
        # 直连路径下用 here-doc 不可靠, 退化为 printf + 单引号转义
        esc = payload.replace("'", "'\\''")
        inner = "printf '%s' '{p}' | (cd {d} && timeout {t} {b_})".format(
            p=esc, d=shlex.quote(REMOTE_DIR), t=total_timeout,
            b_="./" + BIN_BASENAME)
    return inner


# --------------------------------------------------------------------------
# stdout 解析: 按契约切回合, 抽取工具/参数/Observation JSON
# --------------------------------------------------------------------------
def parse_turns(stdout_text):
    """把 stdout 按「[User]:」切回合。每回合:
    query / tools(有序) / args[(tool,param,value)] / obs_objects / reply_text / obs_ok"""
    turns = []
    cur = None

    def new_turn(query):
        return {"query": query, "tools": [], "args": [],
                "obs_objects": [], "obs_ok": True, "obs_lines": [],
                "body_lines": [], "has_obs_line": False}

    raw_lines = stdout_text.splitlines()
    for idx, raw_line in enumerate(raw_lines):
        line = raw_line.rstrip("\r")
        m = RE_USER.search(line)
        if m:
            if cur is not None:
                turns.append(cur)
            cur = new_turn(m.group(1).strip())
            continue
        if cur is None:
            continue  # banner / 加载日志
        mt = RE_TOOL.search(line)
        if mt:
            cur["tools"].append(mt.group(1).strip())
            cur["body_lines"].append(line)
            continue
        ma = RE_ARG.search(line)
        if ma:
            val = ma.group(2).strip()
            if not val:
                # 兼容"值跑到了下一行"的旧版输出格式:
                #   设定参数: mode =
                #   alarm
                # 往下找第一行非空、且不是契约标记的内容当作参数值。
                for j in range(idx + 1, min(idx + 4, len(raw_lines))):
                    cand = raw_lines[j].strip()
                    if not cand:
                        continue
                    if (RE_USER.search(cand) or RE_TOOL.search(cand) or
                            RE_ARG.search(cand) or RE_OBS.search(cand)):
                        break
                    val = cand
                    break
            cur["args"].append((mt_last_tool(cur), ma.group(1).strip(), val))
            continue
        mo = RE_OBS.search(line)
        if mo:
            cur["has_obs_line"] = True
            payload = mo.group(1).strip()
            cur["obs_lines"].append(payload)
            objs = try_parse_obs_json(payload)
            if objs is None:
                cur["obs_ok"] = False
            else:
                cur["obs_objects"].extend(objs)
            continue
        cur["body_lines"].append(line)

    if cur is not None:
        turns.append(cur)
    for t in turns:
        t["reply_text"] = "\n".join(strip_contract_lines(t["body_lines"]))
    return turns


def mt_last_tool(cur):
    """参数行归属最近一次调用的工具(契约: 仅 control_led 打印参数行)。"""
    return cur["tools"][-1] if cur["tools"] else "?"


def try_parse_obs_json(payload):
    """Observation 行 payload 形如 [{...}, {...}]; 截取首个'['到末个']'再解析。"""
    if "[" not in payload or "]" not in payload:
        return None
    seg = payload[payload.index("["): payload.rindex("]") + 1]
    try:
        data = json.loads(seg)
    except (ValueError, TypeError):
        return None
    if not isinstance(data, list):
        data = [data]
    return [o for o in data if isinstance(o, dict)]


CONTRACT_MARKERS = ("[Agent 决策]", "设定参数:", "(Observation)]",
                    "==========", "----------", "[Edge Agent",
                    "[User]:")


def strip_contract_lines(lines):
    out = []
    for ln in lines:
        if any(mk in ln for mk in CONTRACT_MARKERS):
            continue
        out.append(ln)
    return out


# --------------------------------------------------------------------------
# 断言求值 (支持 "tool_called:x" / "reply:kw" 紧凑写法用于 any_of 组)
# --------------------------------------------------------------------------
def eval_assert(a, turn, ctx):
    """返回 (passed: bool, detail: str)。"""
    if isinstance(a, str):
        if a.startswith("tool_called:"):
            a = {"type": "tool_called", "tool": a.split(":", 1)[1]}
        elif a.startswith("reply:"):
            a = {"type": "reply_mentions", "keywords": [a.split(":", 1)[1]]}
        elif a.startswith("obs_field:"):
            k, v = a.split(":", 1)[1].split("=", 1)
            if v == "true":
                v = True
            elif v == "false":
                v = False
            a = {"type": "obs_field", "key": k, "value": v}
        else:
            return False, "未知断言字符串: %r" % a

    t = a["type"]

    if t == "tool_called":
        tool = a["tool"]
        ok = turn is not None and tool in turn["tools"]
        seen = turn["tools"] if turn else []
        return ok, "调用到的工具=%s" % (seen or ["(无)"])

    if t == "tool_not_called":
        tool = a["tool"]
        ok = turn is not None and tool not in turn["tools"]
        seen = turn["tools"] if turn else []
        return ok, "调用到的工具=%s" % (seen or ["(无)"])

    if t == "tool_arg":
        tool, param, value = a["tool"], a["param"], a["value"]
        want = value if isinstance(value, list) else [value]
        got = [(tl, p, v) for (tl, p, v) in (turn["args"] if turn else [])]
        hit = [v for (tl, p, v) in got if tl == tool and p == param
               and v in [str(w) for w in want]]
        ok = bool(hit)
        return ok, "参数行=%s, 期望 %s=%s" % (got or "(无)", param, want)

    if t == "obs_json_valid":
        if turn is None:
            return False, "无回合可检查"
        if not turn["has_obs_line"]:
            return False, "未出现 Observation 行"
        return turn["obs_ok"], "obs行=%d条, 全部为合法JSON数组=%s" % (
            len(turn["obs_lines"]), turn["obs_ok"])

    if t == "obs_field":
        key, value = a["key"], a["value"]
        vals = []
        for o in (turn["obs_objects"] if turn else []):
            if key in o:
                vals.append(o[key])
                if field_eq(o[key], value):
                    return True, "%s=%r 命中" % (key, o[key])
        return False, "obs 中 %s 实际值=%s, 期望=%r" % (key, vals or "(缺字段)", value)

    if t == "obs_field_exists":
        key = a["key"]
        for o in (turn["obs_objects"] if turn else []):
            if key in o:
                return True, "%s 存在=%r" % (key, o[key])
        return False, "obs 中不存在字段 %s" % key

    if t == "reply_mentions":
        kws = a["keywords"]
        text = turn["reply_text"] if turn else ""
        hit = [k for k in kws if k.lower() in text.lower()]
        return bool(hit), "命中关键词=%s (候选=%s)" % (hit, kws)

    if t == "tool_order":
        want = a["tools"]
        got = turn["tools"] if turn else []
        it = iter(got)
        ok = all(any(x == y for y in it) for x in want)
        return ok, "工具顺序实际=%s 期望子序列=%s" % (got, want)

    if t == "exit_clean":
        ok = ctx.get("rc") == 0
        return ok, "进程退出码=%s" % ctx.get("rc")

    if t == "any_of":
        tried = []
        for item in a["assertions"]:
            group = item if isinstance(item, list) else [item]
            results = [eval_assert(sub, turn, ctx) for sub in group]
            if all(r[0] for r in results):
                return True, "任一路径命中: %s" % \
                    "; ".join(r[1] for r in results)
            tried.append("[%s]" % "; ".join(r[1] for r in results))
        return False, "所有候选路径均未命中: " + " || ".join(tried)

    return False, "未知断言类型: %r" % t


def field_eq(actual, expected):
    if isinstance(expected, bool):
        return isinstance(actual, bool) and actual == expected
    if actual is None:
        return False
    if isinstance(actual, (int, float)) and isinstance(expected, str):
        return str(actual).lower() == expected.lower()
    return str(actual).lower() == str(expected).lower()


# --------------------------------------------------------------------------
# 用例执行
# --------------------------------------------------------------------------
def run_cli_case(case, args):
    remote_cmd = build_query_transport(case["query"], use_b64=not args.no_b64)
    t0 = time.time()
    rc, out, err = sh_run(args.host, remote_cmd,
                          timeout_s=args.timeout + 30)
    wall = time.time() - t0
    return rc, out, err, wall


def run_repl_case(case, args):
    query_inputs = [x for x in case["inputs"] if not x["text"].startswith("/")]
    total_timeout = REMOTE_TIMEOUT_S * max(1, len(query_inputs)) + 60
    texts = [x["text"] for x in case["inputs"]] + ["exit"]
    remote_cmd = build_repl_transport(texts, use_b64=not args.no_b64,
                                      total_timeout=total_timeout)
    t0 = time.time()
    rc, out, err = sh_run(args.host, remote_cmd,
                          timeout_s=total_timeout + 30)
    wall = time.time() - t0
    return rc, out, err, wall


def evaluate_case(case, rc, out, err, wall):
    """执行断言, 返回结果 dict (不落盘)。"""
    turns = parse_turns(out)
    res = {"id": case["id"], "desc": case.get("desc", ""),
           "mode": case["mode"], "rc": rc, "wall_s": round(wall, 1),
           "assertions": [], "pass": False, "note": ""}

    if rc == "TIMEOUT":
        res["note"] = "SSH/远程执行超时"
        res["assertions"].append({"type": "_transport", "pass": False,
                                  "detail": "进程超时被强杀"})
        return res
    if rc == 255:
        res["note"] = "SSH 连接失败(网络/免密失效)"
        res["assertions"].append({"type": "_transport", "pass": False,
                                  "detail": err.strip()[:300]})
        return res

    ctx = {"rc": rc}
    if case["mode"] == "cli":
        turn = turns[0] if turns else None
        if turn is None:
            res["note"] = "stdout 中未找到任何 [User] 回合(二进制未按契约输出或崩溃)"
        for a in case["expect"]:
            ok, detail = eval_assert(a, turn, ctx)
            res["assertions"].append(
                {"type": a if isinstance(a, str) else a.get("type"),
                 "pass": ok, "detail": detail})
        # 契约兜底: CLI 单轮必须解析出至少一个 [User] 回合
        if turn is None:
            res["pass"] = False
            return res
    else:  # repl
        query_inputs = [x for x in case["inputs"]
                        if not x["text"].startswith("/")]
        for idx, inp in enumerate(query_inputs):
            turn = turns[idx] if idx < len(turns) else None
            if turn is None:
                res["note"] += "第%d个输入缺少对应回合; " % (idx + 1)
            for a in (inp.get("expect") or []):
                ok, detail = eval_assert(a, turn, ctx)
                res["assertions"].append(
                    {"type": "%s#%d:%s" % (case["id"], idx + 1, a.get("type")),
                     "pass": ok, "detail": detail})
        got_turns = len(turns)
        if got_turns != len(query_inputs):
            res["note"] += "回合数不符: 期望%d 实际%d; " % (
                len(query_inputs), got_turns)

    res["pass"] = all(x["pass"] for x in res["assertions"]) \
        and "SSH" not in res["note"]
    res["stdout"] = out
    res["stderr_tail"] = err[-2000:] if err else ""
    return res


# --------------------------------------------------------------------------
# 故障注入 (--with-faults 专用) 与 LED 恢复
# --------------------------------------------------------------------------
def service_is_active(host):
    rc, out, _ = sh_run(host, "systemctl is-active edgevision", 15)
    return rc == 0 and out.strip() == "active"


def stop_edgevision(host):
    rc, out, err = sh_run(host, "systemctl stop edgevision", 30)
    return rc == 0


def start_edgevision(host):
    rc, out, err = sh_run(host, "systemctl start edgevision", 60)
    return rc == 0


def restore_led_heartbeat(host):
    """测试会话结束后把 LED 拉回 heartbeat (直接写 sysfs, 不经过模型)。

    踩过的坑(两条, 都导致过"假恢复"):
    1. 只写 trigger 而不清用户态爆闪子进程 —— 爆闪循环仍在 100ms 翻转 brightness,
       会把 heartbeat 的亮度覆盖掉, 看起来写了却"没生效";
    2. 判据用 `"heartbeat" in cat_trigger` —— trigger 列表里**所有候选名**都含
       heartbeat 字样, 真正生效的是 [none] 时也会判 ok=True。
       必须匹配 `[heartbeat]` 这个带方括号的生效标记。
    """
    # 1. 先停用户态爆闪(pid 文件 + 兜底模式匹配), 否则它会一直覆盖 brightness
    sh_run(host, "P=$(cat /tmp/edgevision_led_alarm.pid 2>/dev/null); "
                 "if [ -n \"$P\" ] && [ \"$P\" -gt 1 ] 2>/dev/null; then "
                 "kill -TERM $P 2>/dev/null; sleep 1; kill -KILL $P 2>/dev/null; fi; "
                 "pkill -f 'while true; do echo 255 > /sys/class/leds/work/brightness' "
                 "2>/dev/null; rm -f /tmp/edgevision_led_alarm.pid", 20)
    # 2. 亮度归位再切 trigger(某些内核在 brightness=0 时 none 会赖住)
    sh_run(host, "echo 255 > /sys/class/leds/work/brightness; sleep 0.2; "
                 "echo none > /sys/class/leds/work/trigger; sleep 0.2; "
                 "echo heartbeat > /sys/class/leds/work/trigger", 20)
    time.sleep(0.8)

    # 3. 回读判据: 必须看到生效标记 [heartbeat], 且爆闪进程已清空
    rc, out, err = sh_run(host, "cat /sys/class/leds/work/trigger", 15)
    rc2, blink, _ = sh_run(host, "ps -eo args | grep -c '[b]rightness'", 15)
    applied = "[heartbeat]" in (out or "")
    blink_left = 0
    try:
        blink_left = int((blink or "0").strip() or 0)
    except ValueError:
        blink_left = -1
    ok = rc == 0 and applied and blink_left == 0
    detail = "applied=%s blink_left=%s trigger=%s" % (
        applied, blink_left, (out or err).strip()[:160])
    return ok, detail


# --------------------------------------------------------------------------
# 结果落盘
# --------------------------------------------------------------------------
def write_results(out_dir, meta, case_results):
    os.makedirs(out_dir, exist_ok=True)
    results_path = os.path.join(out_dir, "results.json")
    with open(results_path, "w", encoding="utf-8") as f:
        json.dump({"meta": meta, "cases": case_results}, f,
                  ensure_ascii=False, indent=2)

    total = len(case_results)
    passed = sum(1 for r in case_results if r["pass"])
    skipped = sum(1 for r in case_results if r.get("skip"))
    judged = total - skipped
    rate = (passed / judged * 100.0) if judged else 0.0

    lines = []
    lines.append("# Agent 回归测试摘要")
    lines.append("")
    lines.append("- 时间: %s" % meta["timestamp"])
    lines.append("- 板子: %s  二进制: %s" % (meta["host"], meta["bin"]))
    lines.append("- 故障注入: %s" % ("是" if meta["with_faults"] else "否"))
    lines.append("- 用例: %d 条, 通过 %d, 跳过 %d, **通过率 %.1f%%**" %
                 (total, passed, skipped, rate))
    lines.append("- 端到端成功率(断言全过占比)口径见 docs/AGENT_TEST_PLAN.md")
    lines.append("")
    lines.append("| ID | 用例 | 模式 | 结果 | 耗时(s) | 失败断言 |")
    lines.append("|---|---|---|---|---:|---|")
    for r in case_results:
        if r.get("skip"):
            status = "SKIP"
        elif r["pass"]:
            status = "PASS"
        else:
            status = "FAIL"
        failed = [a["type"] for a in r["assertions"] if not a["pass"]]
        q = r.get("desc", "")
        lines.append("| %s | %s | %s | %s | %s | %s |" %
                     (r["id"], q, r["mode"], status, r["wall_s"],
                      ", ".join(failed) if failed else "-"))
    summary_path = os.path.join(out_dir, "summary.md")
    with open(summary_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return results_path, summary_path, rate


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------
def main():
    if sys.stdout and hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(
        description="RK3588 端侧具身 Agent 回归测试器 (纯标准库)")
    ap.add_argument("--host", default=os.environ.get("EDGEVISION_BOARD_HOST", ""))
    ap.add_argument("--bin", default="/root/rkllm_deploy/rkllm_embodied_agent_p0")
    ap.add_argument("--with-faults", action="store_true",
                    help="允许停/启 edgevision 服务做视觉离线用例(结束后恢复)")
    ap.add_argument("--no-b64", action="store_true",
                    help="query 直连传输(shlex.quote 转义)而非 base64")
    ap.add_argument("--no-restore-led", action="store_true",
                    help="结束后不强制把 LED 写回 heartbeat")
    ap.add_argument("--timeout", type=int, default=240,
                    help="单条用例 PC 侧超时秒数(默认240)")
    ap.add_argument("--list", action="store_true", help="仅列出用例不执行")
    ap.add_argument("--out-dir", default=None,
                    help="结果输出根目录(默认 <脚本目录>/agent_test_results)")
    ap.add_argument("--only", default=None,
                    help="只跑指定用例,逗号分隔(T03,T04 或 T03_led_alarm)")
    args = ap.parse_args()

    global BIN_BASENAME, REMOTE_DIR
    BIN_BASENAME = os.path.basename(args.bin)
    REMOTE_DIR = os.path.dirname(args.bin) or REMOTE_DIR

    if args.list:
        for c in CASES:
            print("%s [%s] %s" % (c["id"], c["mode"], c.get("desc", "")))
        return 0

    if not args.host:
        print("[FATAL] 没有目标板 IP。请用 --host <board-ip> 或设置环境变量 "
              "EDGEVISION_BOARD_HOST=<board-ip>（板子内网 IP 不入库）")
        return 2

    print("=" * 64)
    print("RK3588 Agent 回归测试  host=%s  bin=%s" % (args.host, args.bin))
    print("故障注入=%s  查询传输=%s" %
          ("开" if args.with_faults else "关",
           "明文+shlex.quote" if args.no_b64 else "base64"))
    print("=" * 64)

    # 预检: SSH 连通性
    rc, out, err = sh_run(args.host, "echo ok && cd %s && ls ./%s" %
                          (shlex.quote(REMOTE_DIR), shlex.quote(BIN_BASENAME)), 20)
    if rc != 0 or "ok" not in out:
        print("[FATAL] SSH 预检失败, 无法继续: rc=%s\n%s" % (rc, err[:500]))
        return 2
    print("[预检] SSH OK, 板端二进制可见。\n")

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    results_root = args.out_dir or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "agent_test_results")
    out_dir = os.path.join(results_root, timestamp)
    meta = {"timestamp": timestamp, "host": args.host, "bin": args.bin,
            "with_faults": bool(args.with_faults), "transport":
            "plain" if args.no_b64 else "base64"}

    fault_case_ids = {c["id"] for c in CASES if c.get("with_faults")}
    case_results = []

    # --only 过滤: 只重跑指定用例(支持 T03 或 T03_led_alarm 两种写法)
    active_cases = CASES
    if args.only:
        wanted_ids = set()
        for tok in args.only.split(","):
            tok = tok.strip()
            if not tok:
                continue
            hit = [c for c in CASES
                   if c["id"] == tok or c["id"].split("_")[0] == tok]
            if not hit:
                print("[FATAL] --only 未匹配到用例: %s" % tok)
                return 2
            wanted_ids.update(c["id"] for c in hit)
        active_cases = [c for c in CASES if c["id"] in wanted_ids]
        print("[过滤] 仅执行 %d 条: %s" %
              (len(active_cases), ",".join(c["id"] for c in active_cases)))

    for case in active_cases:
        is_fault = case["id"] in fault_case_ids
        if is_fault and not args.with_faults:
            print("[%-32s] SKIP  (需 --with-faults)" % case["id"])
            case_results.append({"id": case["id"], "desc": case.get("desc", ""),
                                 "mode": case["mode"], "skip": True,
                                 "pass": False, "rc": None, "wall_s": 0,
                                 "assertions": [], "note": "未启用故障注入"})
            continue

        service_ctx = None
        if is_fault:
            if not service_is_active(args.host):
                print("[%-32s] SKIP  (edgevision 未在运行, 无法做离线注入)" %
                      case["id"])
                case_results.append({"id": case["id"],
                                     "desc": case.get("desc", ""),
                                     "mode": case["mode"], "skip": True,
                                     "pass": False, "rc": None, "wall_s": 0,
                                     "assertions": [],
                                     "note": "edgevision 非 active"})
                continue
            print("[故障注入] systemctl stop edgevision ...")
            if not stop_edgevision(args.host):
                print("[%-32s] SKIP  (stop 失败, 放弃该用例)" % case["id"])
                case_results.append({"id": case["id"],
                                     "desc": case.get("desc", ""),
                                     "mode": case["mode"], "skip": True,
                                     "pass": False, "rc": None, "wall_s": 0,
                                     "assertions": [], "note": "stop 失败"})
                continue
            service_ctx = "stopped"

        print("[%-32s] RUN   ..." % case["id"], flush=True)
        res = None
        service_restore_failed = False
        try:
            if case["mode"] == "repl":
                rc, out, err, wall = run_repl_case(case, args)
            else:
                rc, out, err, wall = run_cli_case(case, args)
            res = evaluate_case(case, rc, out, err, wall)
        except Exception as exc:  # 单条用例崩溃不影响整体回归
            res = {"id": case["id"], "desc": case.get("desc", ""),
                   "mode": case["mode"], "rc": None, "wall_s": 0,
                   "pass": False, "note": "用例执行异常: %r" % exc,
                   "assertions": [{"type": "_exception", "pass": False,
                                   "detail": repr(exc)}]}
        finally:
            if service_ctx == "stopped":
                ok = start_edgevision(args.host)
                active = service_is_active(args.host) if ok else False
                if ok and active:
                    print("[故障恢复] edgevision 已恢复 active ✔")
                else:
                    service_restore_failed = True
                    print("!" * 70)
                    print("!! 大警告: edgevision 服务恢复失败! 请立即手动执行:")
                    print("!!   ssh %s 'systemctl start edgevision'" % args.host)
                    print("!!   ssh %s 'systemctl status edgevision'" % args.host)
                    print("!" * 70)
                if res is not None:
                    res.setdefault("fault_recovery",
                                   {"step": "finally_restore",
                                    "ok": bool(ok and active)})
        if service_restore_failed:
            # 恢复失败: 立即终止整场回归, 防止后续用例在服务缺失下产生假结果
            case_results.append(res)
            print("[FATAL] 服务恢复失败, 提前终止回归。已跑完 %d 条。"
                  % len(case_results))
            break

        status = "PASS" if res["pass"] else "FAIL"
        print("[%s] %s  (%.1fs, rc=%s)" % (case["id"], status, res["wall_s"],
                                           res["rc"]))
        for a in res["assertions"]:
            if not a["pass"]:
                print("    ✗ %s: %s" % (a["type"], a["detail"]))
        case_results.append(res)
        # 单条用例间稍歇, 让 NPU/服务状态回稳
        time.sleep(2)

    # LED 恢复 (alarm/off 常态化会影响板子使用)
    if not args.no_restore_led:
        ok, detail = restore_led_heartbeat(args.host)
        print("[LED恢复] %s (%s)" % ("已写回 heartbeat ✔" if ok else "失败!",
                                     detail))
        meta["led_restore"] = "ok" if ok else "failed"

    rpath, spath, rate = write_results(out_dir, meta, case_results)
    judged = [r for r in case_results if not r.get("skip")]
    passed = sum(1 for r in judged if r["pass"])
    print("\n" + "=" * 64)
    print("摘要: %d/%d 通过 (%.1f%%), 跳过 %d 条" %
          (passed, len(judged), rate,
           len(case_results) - len(judged)))
    print("明细: %s" % rpath)
    print("摘要: %s" % spath)
    print("=" * 64)
    return 0 if passed == len(judged) and judged else 1


if __name__ == "__main__":
    sys.exit(main())
