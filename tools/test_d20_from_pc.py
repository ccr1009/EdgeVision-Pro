#!/usr/bin/env python3
"""从电脑连板子的 D20 边缘网关(9000 端口), 按【定长包头 + 结构体包体】解包验证。

包格式(与 w3_d20_edge_gateway.cpp 一致):
    DetectionHeader: uint32 frame_id, uint32 bbox_count   (8 字节, 小端)
    BBox * bbox_count: int32 class_id, float score, float x,y,w,h  (24 字节/个)
"""
import socket
import struct
import os
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("BOARD_IP", "")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
MAX_FRAMES = int(sys.argv[3]) if len(sys.argv) > 3 else 6

HEADER = struct.Struct("<II")
BBOX = struct.Struct("<ifffff")
CLASS_NAMES = {0: "行人", 1: "汽车"}


def main() -> int:
    sock = socket.create_connection((HOST, PORT), timeout=15)
    print(f"[+] 已连接板子网关 {HOST}:{PORT}")

    frames = 0
    while frames < MAX_FRAMES:
        head = b""
        while len(head) < HEADER.size:
            chunk = sock.recv(HEADER.size - len(head))
            if not chunk:
                print(f"[*] 网关关闭了连接, 共收到 {frames} 帧")
                sock.close()
                return 0
            head += chunk
        frame_id, count = HEADER.unpack(head)

        need = BBOX.size * count
        body = b""
        while len(body) < need:
            chunk = sock.recv(need - len(body))
            if not chunk:
                print("[!] 包体不完整, 网关断开")
                return 1
            body += chunk

        frames += 1
        parts = []
        for i in range(count):
            cid, score, x, y, w, h = BBOX.unpack_from(body, i * BBOX.size)
            name = CLASS_NAMES.get(cid, str(cid))
            parts.append(f"{name} ({x:.0f},{y:.0f}) {w:.0f}x{h:.0f} conf={score:.2f}")
        print(f"[帧 {frame_id:3d}] {count} 个目标 | " + " | ".join(parts))

    print(f"[*] 已收 {frames} 帧, 主动断开")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
