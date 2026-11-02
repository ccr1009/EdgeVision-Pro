#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gimbal_tracker.py — RK3588 视觉目标追踪 → ESP32 云台闭环

数据源：项目已有的 TCP:9000 视觉网关（248 字节定长帧，8B 头 + 10×24B BBox）
执行器：ESP32-S3 串口云台（见 ../README.md 的协议定义）

闭环形状：
    9000 拿 BBox → 算目标中心相对画面中心的偏差
                 → P 控制算角度增量 → 串口发 P pan tilt
                 → 下一帧重新检测 → 偏差收敛到死区内

为什么是 P 不是 PID：舵机行程有界（Pan ±90°、Tilt ±45°），积分项会累积出超调，
表现成「转过头再慢慢飘回来」，且多一个参数要调。死区 + 单帧限速已能抑制振荡。

用法：
    python3 gimbal_tracker.py --port /dev/ttyACM0
    python3 gimbal_tracker.py --port /dev/ttyACM0 --class person --kp-pan 0.4
    python3 gimbal_tracker.py --port /dev/ttyACM0 --dry-run     # 只算不发，调参用
"""

import argparse
import socket
import sys
import time

try:
    import serial
except ImportError:
    print("缺少 pyserial。板上装：pip3 install pyserial", file=sys.stderr)
    sys.exit(2)

from gimbal_control import (COCO80, FRAME_SIZE, clamp, compute_control,
                            parse_frame, pick_target)


def recv_full_frame(sock, timeout=0.3):
    """收满 248 字节才算一帧。

    必须循环收：TCP 是字节流，单次 recv 只保证"给你一部分"，不保证"给你全部"。
    直接 recv(248) 然后解析，网络抖一下拿到的就是半帧乱码。

    所有错误都收敛成 None，由调用方决定下一步——接收侧不该因为对端断开就崩。
    """
    buf = bytearray()
    sock.settimeout(timeout)
    while len(buf) < FRAME_SIZE:
        try:
            chunk = sock.recv(FRAME_SIZE - len(buf))
        except (socket.timeout, ConnectionResetError, ConnectionAbortedError, OSError):
            return None
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


class SerialGimbal:
    """ESP32 云台串口封装。带重连与错误统计。"""

    def __init__(self, port, baud=115200):
        self.port = port
        self.baud = baud
        self.ser = None
        self.pan = 0.0
        self.tilt = 0.0
        self.errors = 0
        self._open()

    def _open(self):
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=0.5)
            time.sleep(0.3)
            self.ser.reset_input_buffer()
            return True
        except Exception as e:
            print("\n[gimbal] 打开串口失败 %s: %s" % (self.port, e), file=sys.stderr)
            self.ser = None
            return False

    def _send(self, line):
        if self.ser is None:
            if not self._open():
                return None
        try:
            self.ser.write((line + "\n").encode())
            self.ser.flush()
            deadline = time.time() + 0.5
            while time.time() < deadline:
                raw = self.ser.readline()
                if not raw:
                    continue
                resp = raw.decode("utf-8", "replace").strip()
                if not resp or resp.startswith("#"):
                    continue
                return resp
            return None
        except Exception as e:
            print("\n[gimbal] 写串口失败: %s（下轮重连）" % e, file=sys.stderr)
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            return None

    def set_angles(self, pan, tilt):
        resp = self._send("P %.2f %.2f" % (pan, tilt))
        if resp is None:
            self.errors += 1
            return False
        if resp.startswith("OK"):
            parts = resp.split()
            if len(parts) >= 3:
                try:
                    self.pan = float(parts[1])
                    self.tilt = float(parts[2])
                except ValueError:
                    pass
            return True
        # ERR 1 out_of_range：固件已夹紧，不算致命错误，但要让调用方知道
        if "out_of_range" in resp:
            return True
        self.errors += 1
        return False

    def center(self):
        return self._send("H") is not None

    def close(self):
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description="RK3588 视觉追踪云台闭环")
    ap.add_argument("--port", required=True, help="ESP32 串口，如 /dev/ttyACM0")
    ap.add_argument("--host", default="127.0.0.1", help="9000 网关地址")
    ap.add_argument("--tcp-port", type=int, default=9000)
    ap.add_argument("--class", dest="want_class", default=None,
                    help="只追某一类，如 person / car（默认追面积最大的目标）")
    ap.add_argument("--min-score", type=float, default=0.40)
    ap.add_argument("--kp-pan", type=float, default=0.45,
                    help="Pan 比例系数（每度偏差补多少度）")
    ap.add_argument("--kp-tilt", type=float, default=0.40, help="Tilt 比例系数")
    ap.add_argument("--fov-pan", type=float, default=60.0,
                    help="摄像头水平视场角（度），决定像素偏差→角度偏差的换算")
    ap.add_argument("--fov-tilt", type=float, default=36.0,
                    help="摄像头垂直视场角（度）。720p/60°水平视场时约 36°")
    ap.add_argument("--deadzone", type=float, default=0.05,
                    help="画面中心死区（归一化，0.05 = ±5%%）")
    ap.add_argument("--max-step", type=float, default=6.0, help="单帧最大角度变化（度）")
    ap.add_argument("--pan-range", type=float, default=90.0)
    ap.add_argument("--tilt-range", type=float, default=45.0)
    ap.add_argument("--lost-timeout", type=float, default=1.5, help="丢失多久后停住（秒）")
    ap.add_argument("--img-w", type=int, default=1280, help="网关推流分辨率宽")
    ap.add_argument("--img-h", type=int, default=720, help="网关推流分辨率高")
    ap.add_argument("--invert-tilt", action="store_true",
                    help="Tilt 方向反了加这个（目标在上方但云台往下转时用）")
    ap.add_argument("--rate", type=float, default=20.0, help="控制频率上限 Hz")
    ap.add_argument("--dry-run", action="store_true", help="只算角度，不发给舵机")
    args = ap.parse_args()

    print("=" * 68)
    print("视觉追踪云台闭环  source=%s:%d  sink=%s%s"
          % (args.host, args.tcp_port, args.port, "  [DRY-RUN]" if args.dry_run else ""))
    print("类别=%s  kp=(%.2f, %.2f)  FOV=(%.0f°, %.0f°)  deadzone=%.3f  max_step=%.1f°/帧"
          % (args.want_class or "任意", args.kp_pan, args.kp_tilt,
             args.fov_pan, args.fov_tilt, args.deadzone, args.max_step))
    print("=" * 68)

    gimbal = None
    if not args.dry_run:
        gimbal = SerialGimbal(args.port)
        if gimbal.ser is None:
            print("[gimbal] 串口打不开，退出。先跑 gimbal_probe.py 排查。")
            return 2
        print("[gimbal] 串口已打开，回中 ...")
        gimbal.center()
        time.sleep(0.4)

    pan, tilt = 0.0, 0.0
    last_seen = 0.0
    frames = 0
    moved = 0
    t_start = time.time()
    min_period = 1.0 / args.rate
    last_ctrl = 0.0
    last_frame_id = None
    last_print = 0.0

    try:
        while True:
            now = time.time()
            if now - last_ctrl < min_period:
                time.sleep(0.005)
                continue
            last_ctrl = now

            # ---- 取一帧 ----
            try:
                s = socket.create_connection((args.host, args.tcp_port), timeout=0.5)
            except OSError as e:
                print("\n[9000] 连不上视觉网关：%s" % e)
                print("       先确认 systemctl status edgevision 在跑")
                time.sleep(1.0)
                continue

            try:
                data = recv_full_frame(s, timeout=0.3)
            finally:
                s.close()

            if data is None:
                print("\r[9000] 没收到完整帧（超时/半帧）          ", end="")
                continue

            frames += 1
            frame_id, bboxes = parse_frame(data, args.img_w, args.img_h)

            if frame_id == last_frame_id:
                # 网关没在推新帧。不能拿旧帧反复算控制量，否则云台会持续漂移。
                continue
            last_frame_id = frame_id

            target = pick_target(bboxes, args.want_class, args.min_score)

            if target is None:
                # 目标丢失：短暂保持，超时就停住（不要乱找）
                now2 = time.time()
                if last_seen and (now2 - last_seen) > args.lost_timeout:
                    if now2 - last_print > 0.5:
                        print("\r[追踪] 目标丢失 %.1fs，保持 pan=%.1f tilt=%.1f          "
                              % (now2 - last_seen, pan, tilt), end="")
                        last_print = now2
                continue

            last_seen = time.time()

            new_pan, new_tilt, in_dz, dx, dy = compute_control(
                target["cx"], target["cy"], pan, tilt,
                args.kp_pan, args.kp_tilt, args.fov_pan, args.fov_tilt,
                args.pan_range, args.tilt_range,
                args.deadzone, args.max_step, args.invert_tilt)

            if in_dz:
                if time.time() - last_print > 0.5:
                    print("\r[追踪] 锁定 frame=%d 目标居中 pan=%.1f tilt=%.1f          "
                          % (frame_id, pan, tilt), end="")
                    last_print = time.time()
                continue

            if abs(new_pan - pan) < 0.05 and abs(new_tilt - tilt) < 0.05:
                continue

            pan, tilt = new_pan, new_tilt
            moved += 1

            if args.dry_run:
                print("\r[追踪][dry] frame=%d dx=%+.3f dy=%+.3f -> pan=%.1f tilt=%.1f          "
                      % (frame_id, dx, dy, pan, tilt), end="")
            else:
                ok = gimbal.set_angles(pan, tilt)
                print("\r[追踪] frame=%d dx=%+.3f dy=%+.3f -> P %.1f %.1f  %s          "
                      % (frame_id, dx, dy, pan, tilt, "ok" if ok else "SER_FAIL"), end="")

    except KeyboardInterrupt:
        print("\n\n[退出] Ctrl-C")
    finally:
        elapsed = max(time.time() - t_start, 1e-6)
        print("\n" + "=" * 68)
        print("运行 %.1fs  收到 %d 帧  下发 %d 次控制" % (elapsed, frames, moved))
        print("控制频率 %.1f Hz" % (moved / elapsed))
        if gimbal:
            print("串口错误 %d 次" % gimbal.errors)
            print("回中 ...")
            gimbal.center()
            gimbal.close()
        print("=" * 68)

    return 0


if __name__ == "__main__":
    sys.exit(main())
