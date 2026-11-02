#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
端侧 Agent 回归用例集 —— 一句话进，通过/失败出。

为什么需要它：
  口头说"还有很多问题"没法排序。只有把每个问题变成一条**可复现的用例**，
  才能知道"现在到底几条红的"，以及"改完之后红的是多了还是少了"。

用法（在板子上）：
  python3 /root/regress.py            # 跑全部
  python3 /root/regress.py --keep     # 跑完不自动拉起语音网关

判定：
  每轮输出的文本里出现【任一期望词】且不出现【任何禁止词】→ 通过。
  禁止词用来守"曾经修好过的坑"（例如绝不能再说 localhost）。
"""

import os
import re
import subprocess
import sys
import time

LOG = "/tmp/regress.log"
IN = "/tmp/regress_in.txt"
OUT = "/tmp/regress_result.txt"

# (输入, 期望词, 禁止词)
CASES = [
    # ---- 快速通道：必须秒回且答对 ----
    ("温度多少",                    ["温度"],                          []),
    ("灯是什么状态",                ["呼吸", "心跳", "常亮", "熄灭", "急闪"], []),
    ("现在几点",                    ["点"],                            []),
    ("语音助手日志怎么看",          ["journalctl"],                    []),   # 元问题不能被当 status
    ("看板在跑吗",                  ["运行"],                          []),
    # ---- 服务控制：回执必须带主语、区分 no-op ----
    ("关闭车辆检测",                ["本来就没在运行", "看板"],          []),
    ("打开车辆看板",                ["8090"],                          ["localhost", "127.0.0.1"]),
    ("关闭车辆看板",                ["看板"],                          []),
    ("打开推流网关",                ["推流网关", "网关"],               []),
    ("关闭推流网关",                ["推流网关", "网关"],               []),
    # ---- LED：写操作必须真的生效 ----
    ("把灯设成常亮",                ["已切换", "好了", "设好"],          []),
    ("把灯设成心跳",                ["已切换", "好了", "设好"],          []),
    # —— 下面三条由 explore.py 发现、修好后锁进回归 ——
    # 报警态是"用户态爆闪"(alarm_blink)，不是内核 trigger；曾答成"指示灯当前是alarm_blink"
    ("把灯设成报警",                ["已切换", "好了", "设好"],          []),
    ("灯是什么状态",                ["报警", "急闪"],                  ["alarm_blink"]),
    # 道谢不该触发任何硬件工具（曾连调 3 个工具、回一屏设备状态）
    ("谢谢",                        ["不客气", "嗯", "好"],            ["触发物理硬件工具"]),
    # ---- 巡检：不能拿历史当现状 ----
    ("有告警吗",                    ["告警", "正常", "巡检"],            []),
    # ---- LLM 路径：别乱调工具、别复读 ----
    # 寒暄：一个工具都不许调（曾出现"你好"→get_led_status→回答串到灯上）
    ("你好",                        ["我在", "您好", "在的"],          ["触发物理硬件工具", "control_service"]),
    # 但"寒暄+诉求"不能被寒暄分支吞掉
    ("你好，灯什么状态",            ["灯"],                            []),
    ("为什么视觉推理占用npu你会变慢", ["tok/s", "帧"],                   []),
    ("你有什么功能",                ["硬件", "温度", "服务", "监控"],    []),
]

# 从日志里剥掉工具调用、perf、装饰线，只留最终回答
NOISE = re.compile(
    r"^(=|--+|<tool_call>|</function>|<function=|</tool_call>|"
    r"\[perf\]|⚙️|🔍|🤖|👤|-> |🔥|💬|\[Agent\]|EdgeAgent>)"
)


def sh(cmd, timeout=180):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True,
                              text=True, timeout=timeout).stdout
    except subprocess.TimeoutExpired:
        return ""


def clean(seg):
    keep = []
    for line in seg.splitlines():
        s = line.strip()
        if not s or NOISE.match(s):
            continue
        keep.append(s)
    return keep


def main():
    keep_gateway_alive = "--keep" in sys.argv

    print("① 停语音网关，腾出 NPU ...")
    sh("systemctl stop voice-gateway")
    time.sleep(6)
    if sh("systemctl is-active voice-gateway").strip() == "active":
        print("   [!] 网关没停下，测试会因 NPU 不足失败")

    with open(IN, "w", encoding="utf-8") as f:
        for q, _, _ in CASES:
            f.write(q + "\n")
        f.write("exit\n")

    print("② 启动被测控制台（预热约 25s）...")
    sh("systemctl reset-failed regress 2>/dev/null")
    sh("systemd-run --unit=regress --collect --property=StandardInput=file:%s "
       "bash -c 'cd /root/rkllm_deploy && ./rkllm_embodied_agent_p0 > %s 2>&1'" % (IN, LOG))

    for _ in range(200):
        state = sh("systemctl is-active regress").strip()
        if state not in ("active", "activating"):
            break
        time.sleep(2)

    print("③ 解析结果 ...")
    raw = open(LOG, errors="replace").read()
    segs = raw.split("EdgeAgent> ")[1:]          # 每轮一个 "EdgeAgent> " 提示符
    rows, passed = [], 0

    for i, (q, expects, bans) in enumerate(CASES):
        seg = segs[i] if i < len(segs) else ""
        lines = clean(seg)
        answer = " / ".join(lines[-2:])[:56] if lines else "(无输出)"
        ok = any(e in seg for e in expects) and not any(b in seg for b in bans)
        passed += ok
        rows.append((ok, q, answer))

    total = len(CASES)
    lines = ["", "=" * 78,
             "  回归结果  %d/%d 通过（%d%%）" % (passed, total, round(100.0 * passed / total)),
             "=" * 78]
    for ok, q, ans in rows:
        lines.append("  %s  %-26s %s" % ("✓" if ok else "✗", q, ans))
    text = "\n".join(lines) + "\n"
    print(text)
    open(OUT, "w", encoding="utf-8").write(text)

    # 报警是用户态爆闪进程，测试完把灯还原成心跳（否则板子会一直闪警报）
    sh("echo heartbeat > /sys/class/leds/work/trigger")

    if not keep_gateway_alive:
        print("④ 恢复语音网关 ...")
        sh("systemctl start voice-gateway")
        print("   gateway=%s（约 70s 后就绪）" % sh("systemctl is-active voice-gateway").strip())

    print("\n失败用例明细见 %s；原始日志 %s" % (OUT, LOG))


if __name__ == "__main__":
    main()
