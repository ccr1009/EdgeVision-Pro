#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gimbal_control.py — 云台控制律与协议解析（纯函数，无 I/O）

单独抽出来是为了让 test_loopback.py 能**直接 import 真实实现**，
而不是在测试里重写一遍公式。

这个教训很实际：第一版测试自己抄了一份控制律，后来主程序把符号方向改对了、
测试里那份没改，于是测试"通过"但测的是过期逻辑。
测试和实现各写一遍 = 两个都不可信。
"""

import struct

# ---------------- 9000 网关协议（与 lab/agent/rkllm_embodied_agent.cpp 完全一致）----------------
MAX_BBOXES = 10
HEADER_FMT = "<II"                       # uint32 frame_id, uint32 bbox_count
BBOX_FMT = "<ifffff"                     # int32 class_id, float score, x, y, w, h
HEADER_SIZE = struct.calcsize(HEADER_FMT)          # 8
BBOX_SIZE = struct.calcsize(BBOX_FMT)              # 24
FRAME_SIZE = HEADER_SIZE + MAX_BBOXES * BBOX_SIZE  # 248

COCO80 = [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard",
    "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
    "scissors", "teddy bear", "hair drier", "toothbrush",
]


def parse_frame(data, img_w, img_h):
    """解析 248 字节定长帧 → (frame_id, [bbox dict...])。坐标归一化到 [0,1]。"""
    if len(data) < FRAME_SIZE:
        raise ValueError("短帧: %d < %d" % (len(data), FRAME_SIZE))
    frame_id, count = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])
    if count > MAX_BBOXES:
        return frame_id, []        # 畸形帧，丢弃（严禁越界读）
    out = []
    for i in range(count):
        off = HEADER_SIZE + i * BBOX_SIZE
        cid, score, x, y, w, h = struct.unpack(BBOX_FMT, data[off:off + BBOX_SIZE])
        out.append({
            "class_id": cid,
            "score": score,
            "x": x / img_w,
            "y": y / img_h,
            "w": w / img_w,
            "h": h / img_h,
            "cx": (x + w / 2.0) / img_w,
            "cy": (y + h / 2.0) / img_h,
        })
    return frame_id, out


def build_frame(frame_id, boxes):
    """打包一帧（PC 端模拟网关用）。boxes 为 (cid, score, x, y, w, h) 列表。"""
    assert len(boxes) <= MAX_BBOXES
    buf = struct.pack(HEADER_FMT, frame_id, len(boxes))
    for b in boxes:
        buf += struct.pack(BBOX_FMT, *b)
    buf += b"\x00" * (FRAME_SIZE - len(buf))
    return buf


def recv_full_frame_plain(sock, timeout=0.5):
    """收满 248 字节才算一帧（最朴素的版本，供诊断脚本用）。

    必须循环收：TCP 是字节流，单次 recv 只保证"给你一部分"，不保证"给你全部"。
    直接 recv(248) 然后解析，网络抖一下拿到的就是半帧乱码。

    所有错误收敛成 None——接收侧不该因对端断开就整个崩掉。
    """
    import socket as _socket
    buf = bytearray()
    sock.settimeout(timeout)
    while len(buf) < FRAME_SIZE:
        try:
            chunk = sock.recv(FRAME_SIZE - len(buf))
        except (_socket.timeout, ConnectionResetError, ConnectionAbortedError, OSError):
            return None
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def pick_target(bboxes, want_class, min_score):
    """挑追踪目标：优先指定类别，同类里取面积最大。

    为什么按面积不按分数：画面里可能同时有几个人，面积大 = 离得近 = 用户大概率指的那个；
    而相邻目标的分数常常很接近，排序不稳定。
    """
    cands = [b for b in bboxes if b["score"] >= min_score]
    if want_class is not None:
        cands = [b for b in cands
                 if 0 <= b["class_id"] < len(COCO80) and COCO80[b["class_id"]] == want_class]
    if not cands:
        return None
    return max(cands, key=lambda b: b["w"] * b["h"])


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def compute_control(target_cx, target_cy,
                    pan, tilt,
                    kp_pan, kp_tilt,
                    fov_pan, fov_tilt,
                    pan_range, tilt_range,
                    deadzone, max_step,
                    invert_tilt=False):
    """由目标在画面中的位置算出新的 pan/tilt。

    返回 (new_pan, new_tilt, in_deadzone, dx, dy)

    符号约定（搞反了云台往反方向跑，表现得像"抽风"）：
      dx > 0 = 目标在画面右侧 = 目标在相机当前朝向的右边
              → 相机要向右转 → pan 增大          → d_pan = +err_pan
      dy > 0 = 目标在画面下方 = 目标在视轴下方
              → 相机要低头 → tilt 减小（约定正 tilt = 抬头）→ d_tilt = -err_tilt
    物理上"向右转"对应舵机往哪边，由固件 PAN_INVERT 兜底，这一层只管语义方向。

    先把像素偏差换成角度偏差再乘 kp，好处是 kp 有物理意义（0.5 = 每度偏差补 0.5 度），
    换摄像头/换分辨率不用重新瞎调。
    """
    dx = target_cx - 0.5
    dy = target_cy - 0.5

    if abs(dx) < deadzone:
        dx = 0.0
    if abs(dy) < deadzone:
        dy = 0.0
    if dx == 0.0 and dy == 0.0:
        return pan, tilt, True, dx, dy

    err_pan_deg = dx * fov_pan
    err_tilt_deg = dy * fov_tilt

    d_pan = err_pan_deg * kp_pan
    d_tilt = -err_tilt_deg * kp_tilt
    if invert_tilt:
        d_tilt = -d_tilt

    # 单帧限速：目标突跳时云台不猛甩（机械冲击 = 扫齿风险）
    d_pan = clamp(d_pan, -max_step, max_step)
    d_tilt = clamp(d_tilt, -max_step, max_step)

    new_pan = clamp(pan + d_pan, -pan_range, pan_range)
    new_tilt = clamp(tilt + d_tilt, -tilt_range, tilt_range)
    return new_pan, new_tilt, False, dx, dy
