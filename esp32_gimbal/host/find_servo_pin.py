#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
find_servo_pin.py — 自动扫描找出舵机信号线实际接在哪个 GPIO

背景：ESP32 板子上的排针只有物理序号（14、15...），不一定是 GPIO 编号。
      接错脚 = 舵机完全不动。与其猜，不如让固件挨个试。

前提：固件 v2 及以上（支持 X 扫描命令 + G 切换引脚）
用法：
    python find_servo_pin.py --port COM5

流程：
    发 X 命令 → 固件每 1.2 秒换一个 GPIO，输出 SCAN <pin>
    脚本实时打印"正在测试 GPIO xx"，你盯着舵机
    哪次动了，记下那个编号，Ctrl-C 停，然后：
        python find_servo_pin.py --port COM5 --set 14
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("缺 pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(2)


# 与固件 SCAN_PINS 保持一致
SCAN_PINS = [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
             21, 38, 39, 40, 41, 42, 47]


def main():
    ap = argparse.ArgumentParser(description="找出舵机信号线接在哪个 GPIO")
    ap.add_argument("--port", required=True)
    ap.add_argument("--set", type=int, default=None,
                    help="把 PAN 引脚设为指定 GPIO（找到之后用）")
    ap.add_argument("--set-tilt", type=int, default=None,
                    help="把 TILT 引脚设为指定 GPIO")
    ap.add_argument("--test", type=int, default=None,
                    help="只测试单个 GPIO：持续摆动 5 秒")
    args = ap.parse_args()

    try:
        s = serial.Serial(args.port, 115200, timeout=2)
    except Exception as e:
        print("打开 %s 失败: %s" % (args.port, e))
        return 2

    time.sleep(0.4)
    s.reset_input_buffer()

    def send(cmd, wait=1.5):
        s.write((cmd + "\n").encode())
        s.flush()
        deadline = time.time() + wait
        got = []
        while time.time() < deadline:
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            got.append(line)
            if line.startswith(("OK", "GPIN", "ERR", "S ")):
                break
        return got

    # ---- 模式 1：设定引脚 ----
    if args.set is not None or args.set_tilt is not None:
        if args.set is not None:
            print("设置 PAN 引脚 = GPIO%d" % args.set)
            print("  ", send("G %d" % args.set))
        if args.set_tilt is not None:
            print("设置 TILT 引脚 = GPIO%d" % args.set_tilt)
            print("  ", send("T %d" % args.set_tilt))
        print("\n当前配置:", send("I"))
        print("\n测试摆动：")
        print("  ", send("P 30 0", 0.6))
        time.sleep(0.6)
        print("  ", send("P -30 0", 0.6))
        time.sleep(0.6)
        print("  ", send("H", 0.6))
        s.close()
        return 0

    # ---- 模式 2：测试单个 GPIO ----
    if args.test is not None:
        print("把 PAN 切到 GPIO%d，摆动 6 秒 —— 看舵机动没动" % args.test)
        print("  ", send("G %d" % args.test))
        t0 = time.time()
        while time.time() - t0 < 6:
            send("P 30 0", 0.25)
            send("P -30 0", 0.25)
        print("  ", send("H", 0.5))
        s.close()
        return 0

    # ---- 模式 3：全扫描 ----
    print("=" * 64)
    print("引脚扫描：每 1.2 秒换一个 GPIO，幅度 ±30°")
    print("★ 盯着舵机，哪次动了就记住屏幕上的编号，然后 Ctrl-C")
    print("=" * 64)

    send("X", 0.5)

    current = None
    t0 = time.time()
    try:
        while time.time() - t0 < 40:
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue

            if line.startswith("SCAN"):
                parts = line.split()
                if len(parts) >= 2 and parts[1].isdigit():
                    current = int(parts[1])
                    idx = SCAN_PINS.index(current) + 1 if current in SCAN_PINS else 0
                    print("\n  [%2d/%d] 正在测试 GPIO%-2d  <<<< 舵机动了就是这个"
                          % (idx, len(SCAN_PINS), current), flush=True)
                elif "DONE" in line:
                    print("\n扫描结束。如果全程没动，检查：")
                    print("  1) 舵机信号线（橙/黄）是否插在某个排针上")
                    print("  2) 舵机电源是否上电、共地")
                    print("  3) 舵机本身是否好的")
                    break
            elif line.startswith("#"):
                print("  " + line)
            else:
                print("  " + line)
    except KeyboardInterrupt:
        print("\n\n[Ctlr-C] 停止扫描")

    print("\n如果确定了引脚，运行：")
    print("  python find_servo_pin.py --port %s --set <编号>" % args.port)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
