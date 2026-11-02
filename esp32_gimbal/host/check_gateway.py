#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_gateway.py — 核对真实 9000 网关的帧格式与坐标口径

为什么单独做这个：gimbal_tracker.py 里 --img-w/--img-h 假设了推流分辨率，
但**网关推的坐标到底是原始像素还是网络输入(640x640 letterbox 后)的像素**，
必须实测才能确定。口径搞错的话，追踪会整体偏移甚至完全跑不动，
而且症状像"机械没装好"，很难查。

用法：
    python3 check_gateway.py
    python3 check_gateway.py --frames 20
"""

import argparse
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gimbal_control import (BBOX_FMT, BBOX_SIZE, COCO80, FRAME_SIZE,
                            HEADER_FMT, HEADER_SIZE, recv_full_frame_plain)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--frames", type=int, default=10)
    args = ap.parse_args()

    print("=" * 70)
    print("真实网关帧格式核对  %s:%d  取 %d 帧" % (args.host, args.port, args.frames))
    print("=" * 70)

    seen_ids = set()
    all_boxes = []
    n_ok = 0
    n_fail = 0

    for i in range(args.frames):
        try:
            s = socket.create_connection((args.host, args.port), timeout=2.0)
        except OSError as e:
            print("连接失败: %s" % e)
            return 2

        try:
            data = recv_full_frame_plain(s, timeout=1.0)
        finally:
            s.close()

        if data is None:
            n_fail += 1
            print("  [%02d] 没收到完整帧" % i)
            continue

        frame_id, count = struct_unpack_header(data)
        if count > 10:
            print("  [%02d] bbox_count=%d 越界（畸形帧）" % (i, count))
            n_fail += 1
            continue

        n_ok += 1
        seen_ids.add(frame_id)
        boxes = []
        for k in range(count):
            off = HEADER_SIZE + k * BBOX_SIZE
            cid, score, x, y, w, h = struct_unpack_bbox(data, off)
            cname = COCO80[cid] if 0 <= cid < len(COCO80) else "?%d" % cid
            boxes.append((cname, score, x, y, w, h))
            all_boxes.append((cname, score, x, y, w, h))

        desc = ", ".join("%s %.2f (%.0f,%.0f,%.0fx%.0f)" % b for b in boxes[:3])
        print("  [%02d] frame_id=%-6d n=%d  %s" % (i, frame_id, count, desc or "(无目标)"))
        time.sleep(0.05)

    print("\n" + "=" * 70)
    print("统计")
    print("=" * 70)
    print("成功帧 %d / 失败 %d" % (n_ok, n_fail))
    print("不同 frame_id %d 个（>1 说明网关在持续推流，不是静态）" % len(seen_ids))

    if not all_boxes:
        print("\n⚠️  这段时间画面里没有任何目标。")
        print("   坐标口径无法判定，请让摄像头对准有物体的画面再跑一次。")
        return 0

    xs = [b[2] for b in all_boxes]
    ys = [b[3] for b in all_boxes]
    ws = [b[4] for b in all_boxes]
    hs = [b[5] for b in all_boxes]
    rights = [b[2] + b[4] for b in all_boxes]
    bottoms = [b[3] + b[5] for b in all_boxes]

    print("\n坐标范围（全部 %d 个框）:" % len(all_boxes))
    print("  x: %.1f ~ %.1f" % (min(xs), max(xs)))
    print("  y: %.1f ~ %.1f" % (min(ys), max(ys)))
    print("  w: %.1f ~ %.1f" % (min(ws), max(ws)))
    print("  h: %.1f ~ %.1f" % (min(hs), max(hs)))
    print("  x+w 最大: %.1f" % max(rights))
    print("  y+h 最大: %.1f" % max(bottoms))

    print("\n口径判定:")
    max_right = max(rights)
    max_bottom = max(bottoms)

    # 判据不能只看"是否超过 640"。实测 2026-11-01 真网关(1280x720)：
    #   框在 (942, 4, 335x393)，x+w = 1277 —— 明显是原始像素，不是 640 网络输入坐标。
    # 但若目标恰好都在画面左半边，也会全部 ≤640，所以还要看"能否超过 640"这个必要条件。
    EXCEED = 700.0

    if max_right > EXCEED or max_bottom > EXCEED:
        print("  → 坐标出现 >%.0f 的值（最大 %.0f x %.0f）" % (EXCEED, max_right, max_bottom))
        print("     ⇒ **原始/推流分辨率坐标系**（网关已把 letterbox 逆变换回原图）")
        print("       这是本项目 2026-11-01 实测确认的行为。")
        verdict = "native"
    elif max_right <= 640.5 and max_bottom <= 640.5:
        print("  → 所有坐标 ≤640（最大 %.0f x %.0f）" % (max_right, max_bottom))
        print("     ⇒ 可能是 640x640 网络输入坐标，也可能目标恰好都在画面左上区。")
        print("       建议：让目标跑到画面右下角再测一次，看能否出现 >640 的坐标。")
        verdict = "ambiguous"
    else:
        print("  → 坐标介于 640~700 之间（最大 %.0f x %.0f），无法明确判定" % (max_right, max_bottom))
        verdict = "ambiguous"

    print("\n推荐的 gimbal_tracker 参数:")
    if verdict == "native":
        print("  --img-w <摄像头实际宽> --img-h <摄像头实际高>")
        print("  本项目实测 /dev/video81 是 1280x720 → --img-w 1280 --img-h 720（默认值已经是这个）")
    else:
        print("  先用 v4l2-ctl --list-formats-ext -d /dev/video81 确认分辨率，再据此设 --img-w/--img-h")

    return 0


def struct_unpack_header(data):
    import struct
    return struct.unpack(HEADER_FMT, data[:HEADER_SIZE])


def struct_unpack_bbox(data, off):
    import struct
    return struct.unpack(BBOX_FMT, data[off:off + BBOX_SIZE])


if __name__ == "__main__":
    sys.exit(main())
