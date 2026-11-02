#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fake_gateway.py — 模拟项目的 TCP:9000 视觉网关（PC 端自测用）

在没有 RK3588 的时候验证 gimbal_tracker.py 的端到端行为。
推的是 **真实协议**（248 字节定长帧），不是自造格式——这样接线之后
只要把 --host 指向板子，代码一行不用改。

场景：一个目标在画面里左右来回走（模拟人走动），20Hz 推帧。
"""

import argparse
import math
import socket
import struct
import sys
import time

sys.stdout.reconfigure(encoding="utf-8")

from gimbal_control import build_frame

MAX_BBOXES = 10


def main():
    ap = argparse.ArgumentParser(description="模拟 9000 视觉网关")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--fps", type=float, default=20.0)
    ap.add_argument("--img-w", type=int, default=1280)
    ap.add_argument("--img-h", type=int, default=720)
    ap.add_argument("--pattern", default="sweep",
                    choices=["sweep", "static", "random"],
                    help="目标运动模式")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(4)
    print("=" * 60)
    print("模拟网关监听 127.0.0.1:%d  协议=248B 定长帧  %.0f fps"
          % (args.port, args.fps))
    print("模式: %s   画面 %dx%d" % (args.pattern, args.img_w, args.img_h))
    print("=" * 60)

    period = 1.0 / args.fps
    frame_id = 0
    t0 = time.time()
    conn_count = 0

    while True:
        try:
            conn, addr = srv.accept()
        except KeyboardInterrupt:
            break
        conn_count += 1
        conn.settimeout(2.0)
        print("[conn #%d] %s" % (conn_count, addr))

        try:
            while True:
                t = time.time() - t0
                if args.pattern == "sweep":
                    # 目标左右扫动（周期约 6 秒），垂直略微起伏
                    cx_norm = 0.5 + 0.35 * math.sin(t * 1.05)
                    cy_norm = 0.5 + 0.10 * math.sin(t * 0.7)
                elif args.pattern == "static":
                    cx_norm, cy_norm = 0.72, 0.45
                else:
                    import random
                    cx_norm = 0.5 + 0.35 * (random.random() * 2 - 1)
                    cy_norm = 0.5 + 0.15 * (random.random() * 2 - 1)

                # 造一个 person 框，宽高固定，中心在 (cx, cy)
                bw, bh = 90.0, 200.0
                cx_px = cx_norm * args.img_w
                cy_px = cy_norm * args.img_h
                x = cx_px - bw / 2
                y = cy_px - bh / 2

                frame = build_frame(frame_id, [
                    (0, 0.87, x, y, bw, bh),        # person
                ])
                conn.sendall(frame)
                frame_id += 1
                time.sleep(period)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError,
                socket.timeout, OSError) as e:
            print("[conn #%d] 断开: %s" % (conn_count, type(e).__name__))
        except KeyboardInterrupt:
            break
        finally:
            try:
                conn.close()
            except Exception:
                pass

    srv.close()
    print("\n网关退出，共服务 %d 次连接，推送 %d 帧" % (conn_count, frame_id))


if __name__ == "__main__":
    main()
