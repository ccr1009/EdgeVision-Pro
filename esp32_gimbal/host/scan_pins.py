#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""让固件挨个 GPIO 输出舵机信号，盯着舵机看哪个引脚能动。

为什么需要这个脚本：
    排针丝印(GPIO14)和实际焊接的引脚不一定一致，杜邦线也可能虚接。
    与其一个个手改 G 命令，不如让固件自己扫一遍。

用法：
    python scan_pins.py --port COM5
    # 看到舵机动了，记下屏幕上最后一次 "SCAN <pin>" 的数字
    # 然后：
    python scan_pins.py --port COM5 --probe 16    # 只反复测这一个脚

判断标准（很重要）：
    舵机**收到有效 PWM** 才会锁住摇臂（有阻力）或转动。
    如果你能用手轻松拨动摇臂 = 该引脚没有有效信号。
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("缺 pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(2)


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.3
    s.write_timeout = 2.0
    s.open()
    s.dtr = False
    s.rts = False
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def send(s, line, wait=0.2):
    s.write((line + "\n").encode())
    s.flush()
    time.sleep(wait)
    n = s.in_waiting
    return (s.read(n) if n else b"").decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--probe", type=int, default=None,
                    help="只反复测试这一个 GPIO（不扫描）")
    ap.add_argument("--seconds", type=float, default=25.0)
    args = ap.parse_args()

    s = open_port(args.port)

    try:
        if args.probe is not None:
            pin = args.probe
            send(s, "A")               # 停掉可能的翻转
            print(send(s, "G %d" % pin))
            print("=== 反复驱动 GPIO%d，±45° 摆动 ===" % pin)
            print("=== 盯着舵机：会动吗？摇臂有阻力吗？ ===\n")
            t0 = time.time()
            i = 0
            while time.time() - t0 < args.seconds:
                ang = 45 if (i % 2 == 0) else -45
                send(s, "P %d 0" % ang, wait=0.9)
                print("  %5.1fs  ->  %+d deg" % (time.time() - t0, ang), flush=True)
                i += 1
            send(s, "P 0 0")
            print("\n" + send(s, "Q"))
            return

        # 扫描模式
        print("=== 引脚扫描：每 1.2 秒换一个 GPIO ===")
        print("=== 盯着舵机！哪一次动了，记下那个 SCAN 编号 ===\n")
        send(s, "X")

        cur = None
        t0 = time.time()
        while time.time() - t0 < args.seconds + 5:
            n = s.in_waiting
            if n:
                chunk = s.read(n).decode("utf-8", "replace")
                for ln in chunk.splitlines():
                    ln = ln.strip()
                    if ln.startswith("SCAN "):
                        cur = ln[5:]
                        print("  >>> 正在测 GPIO %s   <<<" % cur, flush=True)
                    elif ln.startswith("SCAN DONE"):
                        print("=== 扫描结束 ===")
                        return
            time.sleep(0.05)
            if time.time() - t0 > args.seconds:
                break
        print("\n最后一轮测试的是 GPIO %s" % cur)
    finally:
        s.close()


if __name__ == "__main__":
    main()
