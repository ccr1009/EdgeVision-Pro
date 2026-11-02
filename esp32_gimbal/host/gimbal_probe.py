#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gimbal_probe.py — ESP32 云台连通性与机械限位自检

跑追踪闭环之前先跑这个。它按顺序验证：
  1. 串口能打开、能收到 BOOT 横幅
  2. Q 查询有回应
  3. 小角度扫动确实被接受（OK 回执）
  4. 超限角度被拒绝（ERR 1 out_of_range）
  5. 回中

为什么单独做这个：出问题时你必须能区分「串口没通 / 固件没刷 / 机械卡住 / 代码算错角度」。
没有独立自检，这四种故障在追踪日志里看起来一模一样。

用法：
    python3 gimbal_probe.py --port /dev/ttyACM0
    python3 gimbal_probe.py --port COM5          # Windows
    python3 gimbal_probe.py --port /dev/ttyACM0 --no-move   # 只测通信不动舵机
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("缺少 pyserial。板上装：pip3 install pyserial", file=sys.stderr)
    sys.exit(2)


class Gimbal:
    def __init__(self, port, baud=115200, timeout=1.0):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.pan = 0.0
        self.tilt = 0.0

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _readline(self, deadline):
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            if line.startswith("#"):
                continue          # 调试注释行，忽略
            return line
        return None

    def wait_boot(self, timeout=3.0):
        """等上电横幅。给 ESP32 留出复位时间。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._readline(deadline)
            if line and line.startswith("BOOT"):
                return line
        return None

    def cmd(self, text, timeout=1.0):
        self.ser.write((text + "\n").encode())
        self.ser.flush()
        return self._readline(time.time() + timeout)

    def set_angles(self, pan, tilt):
        """返回 (success, message, actual_pan, actual_tilt)"""
        resp = self.cmd("P %.2f %.2f" % (pan, tilt))
        if resp is None:
            return False, "no response", self.pan, self.tilt
        if resp.startswith("OK"):
            parts = resp.split()
            if len(parts) >= 3:
                try:
                    self.pan = float(parts[1])
                    self.tilt = float(parts[2])
                except ValueError:
                    pass
            return True, resp, self.pan, self.tilt
        return False, resp, self.pan, self.tilt

    def center(self):
        resp = self.cmd("H")
        self.pan = self.tilt = 0.0
        return resp

    def query(self):
        return self.cmd("Q")


def main():
    ap = argparse.ArgumentParser(description="ESP32 云台自检")
    ap.add_argument("--port", required=True, help="/dev/ttyACM0 或 COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-move", action="store_true",
                    help="只测通信，不发运动指令")
    args = ap.parse_args()

    print("=" * 60)
    print("ESP32 云台自检  port=%s baud=%d" % (args.port, args.baud))
    print("=" * 60)

    try:
        g = Gimbal(args.port, args.baud)
    except Exception as e:
        print("[FAIL] 打不开串口：%s" % e)
        print("       检查：板子插好没 / 有没有别处占用 / 权限（板子上 sudo usermod -aG dialout $USER）")
        return 2

    failures = 0

    # ---- 1. BOOT 横幅 ----
    print("\n[1/5] 检查上电横幅 ...")
    banner = g.wait_boot(3.0)
    if banner:
        print("      OK: %s" % banner)
    else:
        # 没收到横幅不一定是坏事：ESP32 可能已经启动过了，不会重发 BOOT。
        # 用 Q 查询兜底验证链路是否活着。
        print("      未收到 BOOT（ESP32 可能早已启动），改用 Q 验证链路 ...")
        q = g.query()
        if q and q.startswith("S "):
            print("      OK: 链路正常，%s" % q)
        else:
            print("      [FAIL] 链路无响应")
            print("      检查：USB CDC On Boot 是否 Enabled（没开则串口没输出）")
            failures += 1

    # ---- 2. 状态查询 ----
    print("\n[2/5] 查询状态 ...")
    q = g.query()
    if q and q.startswith("S "):
        print("      OK: %s" % q)
    else:
        print("      [FAIL] Q 无正确回应：%r" % q)
        failures += 1

    if args.no_move:
        print("\n--no-move：跳过运动测试")
        g.close()
        return 0 if failures == 0 else 1

    # ---- 3. 小角度扫动 ----
    print("\n[3/5] 小角度扫动（看舵机动不动）...")
    seq = [(0, 0), (15, 0), (-15, 0), (0, 10), (0, -10), (0, 0)]
    for pan, tilt in seq:
        ok, msg, ap_, at = g.set_angles(pan, tilt)
        flag = "OK  " if ok else "FAIL"
        print("      %s P %6.1f %6.1f  -> %-24s 实际 %.1f %.1f" %
              (flag, pan, tilt, msg, ap_, at))
        if not ok:
            failures += 1
        time.sleep(0.45)

    # ---- 4. 限位测试 ----
    print("\n[4/5] 超限测试（应被拒绝，不应该真转过去）...")
    for pan, tilt, desc in [(200, 0, "pan 超上限"),
                            (0, 120, "tilt 超上限")]:
        ok, msg, ap_, at = g.set_angles(pan, tilt)
        if (not ok) and "out_of_range" in (msg or ""):
            print("      OK   %s：被正确拦截 -> %s" % (desc, msg))
        else:
            print("      [FAIL] %s：未被拦截 -> %r" % (desc, msg))
            failures += 1
        time.sleep(0.3)

    # ---- 5. 回中 ----
    print("\n[5/5] 回中 ...")
    r = g.center()
    print("      %s" % r)
    time.sleep(0.5)
    q = g.query()
    print("      最终状态：%s" % q)

    g.close()

    print("\n" + "=" * 60)
    if failures == 0:
        print("全部通过 ✅  可以跑 gimbal_tracker.py 了")
    else:
        print("有 %d 项失败 ❌  先修这里再跑追踪" % failures)
    print("=" * 60)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
