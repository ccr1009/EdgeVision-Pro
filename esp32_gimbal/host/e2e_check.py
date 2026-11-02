#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""e2e_check.py — 端到端联调检查：假网关 + 真实 ESP32 串口，跑 15 秒看云台是否跟随"""
import os
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8")

PY = sys.executable
HERE = os.path.dirname(os.path.abspath(__file__))
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

print("=" * 66)
print("端到端联调：fake_gateway(9000) + gimbal_tracker + ESP32(%s)" % PORT)
print("=" * 66)

gw = subprocess.Popen(
    [PY, os.path.join(HERE, "fake_gateway.py"), "--port", "9000", "--fps", "20"],
    cwd=HERE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
time.sleep(1.0)
if gw.poll() is not None:
    err = gw.stderr.read().decode("utf-8", "replace")
    print("假网关启动失败:\n%s" % err)
    sys.exit(1)
print("[1] 模拟网关已启动 (127.0.0.1:9000)")

tr = subprocess.Popen(
    [PY, "-u", os.path.join(HERE, "gimbal_tracker.py"),
     "--port", PORT, "--host", "127.0.0.1", "--tcp-port", "9000",
     "--class", "person", "--rate", "15"],
    cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    env=dict(os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8"))
print("[2] tracker 已启动，运行 %.0f 秒 ..." % DURATION)

time.sleep(DURATION)
tr.terminate()
try:
    out, _ = tr.communicate(timeout=10)
except subprocess.TimeoutExpired:
    tr.kill()
    out, _ = tr.communicate()

gw.terminate()
try:
    gw.wait(timeout=5)
except subprocess.TimeoutExpired:
    gw.kill()

text = out.decode("utf-8", "replace")
lines = [l for l in text.splitlines() if l.strip()]

print("\n[3] tracker 输出（最后 25 行）:")
for l in lines[-25:]:
    print("   " + l)

# 从输出的 "-> P <pan> <tilt>" 里抽 pan 值，看是否真的在动
import re

pans = []
for l in lines:
    m = re.search(r"-> P (-?\d+\.\d+) (-?\d+\.\d+)", l)
    if m:
        pans.append(float(m.group(1)))

print("\n[4] 判据:")
ok = True
if len(pans) < 5:
    print("   ❌ 控制指令太少（%d 条），闭环没跑起来" % len(pans))
    ok = False
else:
    lo, hi = min(pans), max(pans)
    span = hi - lo
    print("   pan 指令 %d 条，范围 [%.2f, %.2f]，跨度 %.2f°"
          % (len(pans), lo, hi, span))
    if span >= 5.0:
        print("   ✅ 云台确实在跟随目标移动（跨度 ≥5°）")
    else:
        print("   ❌ 云台几乎没动（跨度 %.2f° < 5°），跟踪可能失效" % span)
        ok = False

    distinct = len(set(round(p, 1) for p in pans))
    print("   不同 pan 取值 %d 个" % distinct)
    if distinct >= 5:
        print("   ✅ 有连续跟踪（不是来回跳一个值）")
    else:
        print("   ⚠️  取值太集中，可能卡在某个位置")

print("\n" + "=" * 66)
print("端到端联调 %s" % ("通过 ✅" if ok else "未通过 ❌"))
print("=" * 66)
sys.exit(0 if ok else 1)
