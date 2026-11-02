#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_loopback.py — 闭环逻辑自测（无硬件）

直接 import gimbal_control 里的真实实现来测，不在测试里重写公式。

为什么强调这点：最初这版测试自己抄了一份控制律，后来主程序把符号方向改对了、
测试里那份没改，于是测试"全绿"但测的是过期逻辑。测试和实现各写一遍 = 两个都不可信。

覆盖：
  1. 248 字节定长帧的打包/解包（错一个字节整个追踪都会拿到乱码）
  2. 目标选择（面积优先、类别过滤、阈值）
  3. P 控制符号方向、限速、死区
  4. 闭环收敛仿真（含"摄像头装在云台上"的物理模型）
"""

import sys

sys.stdout.reconfigure(encoding="utf-8")

from gimbal_control import (BBOX_FMT, BBOX_SIZE, COCO80, FRAME_SIZE,
                            HEADER_FMT, HEADER_SIZE, build_frame,
                            compute_control, parse_frame, pick_target)

# 与 gimbal_tracker.py 默认参数一致
KP_PAN, KP_TILT = 0.45, 0.40
FOV_PAN, FOV_TILT = 60.0, 36.0
PAN_R, TILT_R = 90.0, 45.0
DEADZONE, MAX_STEP = 0.05, 6.0

FAIL = 0


def check(cond, msg):
    global FAIL
    if cond:
        print("    ✅ %s" % msg)
    else:
        print("    ❌ %s" % msg)
        FAIL += 1


print("=" * 70)
print("闭环逻辑自测（无硬件）")
print("=" * 70)

# ---------- 1. 协议尺寸 ----------
print("\n[1] 248 字节定长帧协议")
check(HEADER_SIZE == 8, "header = 8 字节（实际 %d）" % HEADER_SIZE)
check(BBOX_SIZE == 24, "bbox = 24 字节（实际 %d）" % BBOX_SIZE)
check(FRAME_SIZE == 248, "整帧 = 248 字节（实际 %d）" % FRAME_SIZE)

# 打包→解包往返，验证字节偏移与字段顺序
raw = build_frame(0xAABBCCDD, [(0, 0.87, 640.0, 360.0, 100.0, 200.0)])
check(len(raw) == 248, "build_frame 输出 248 字节")

fid, boxes = parse_frame(raw, 1280, 720)
check(fid == 0xAABBCCDD, "frame_id 往返正确 (0x%X)" % fid)
check(len(boxes) == 1, "检出 1 个框")
if boxes:
    b = boxes[0]
    check(b["class_id"] == 0 and COCO80[b["class_id"]] == "person", "类别 = person")
    check(abs(b["score"] - 0.87) < 1e-6, "score = 0.87")
    # 中心 (640+50)/1280 = 0.5391, (360+100)/720 = 0.6389
    check(abs(b["cx"] - 690.0 / 1280.0) < 1e-6, "cx 归一化正确 (%.4f)" % b["cx"])
    check(abs(b["cy"] - 460.0 / 720.0) < 1e-6, "cy 归一化正确 (%.4f)" % b["cy"])

# 畸形帧：bbox_count 超过上限必须被丢弃，不能越界读
bad = build_frame(1, [(0, 0.9, 1.0, 1.0, 1.0, 1.0)])
bad = bad[:4] + (99).to_bytes(4, "little") + bad[8:]
fid_bad, boxes_bad = parse_frame(bad, 1280, 720)
check(boxes_bad == [], "bbox_count=99 的畸形帧被丢弃（不越界读）")

# ---------- 2. 目标选择 ----------
print("\n[2] 目标选择（面积优先）")
boxes = [
    {"class_id": 0, "score": 0.90, "w": 0.05, "h": 0.10, "cx": 0.20, "cy": 0.50},  # person 小
    {"class_id": 0, "score": 0.60, "w": 0.20, "h": 0.40, "cx": 0.70, "cy": 0.50},  # person 大
    {"class_id": 2, "score": 0.80, "w": 0.30, "h": 0.30, "cx": 0.50, "cy": 0.50},  # car 最大
]
t = pick_target(boxes, "person", 0.4)
check(t is not None and abs(t["cx"] - 0.70) < 1e-9,
      "只追 person → 选中面积大的那个 (cx=0.70)")
t = pick_target(boxes, None, 0.4)
check(t is not None and abs(t["cx"] - 0.50) < 1e-9,
      "不限类别 → 选中面积最大的 car (cx=0.50)")
check(pick_target(boxes, "person", 0.95) is None, "阈值 0.95 → 无目标")
check(pick_target([], "person", 0.4) is None, "空列表 → 无目标")

# ---------- 3. P 控制符号方向 ----------
print("\n[3] P 控制符号方向")

# 目标在画面右侧 dx>0 → 相机要向右转 → pan 增大
np_, nt_, dz, dx, dy = compute_control(
    0.50 + 0.30, 0.50, 0.0, 0.0,
    KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP)
check(np_ > 0, "目标偏右 dx=%+.2f → pan 增大到 %+.2f（向右转）" % (dx, np_))
check(abs(nt_) < 1e-9, "目标垂直居中 → tilt 不变")

# 目标在画面左侧 → pan 减小
np_, nt_, _, dx, _ = compute_control(
    0.50 - 0.30, 0.50, 0.0, 0.0,
    KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP)
check(np_ < 0, "目标偏左 dx=%+.2f → pan 减小到 %+.2f（向左转）" % (dx, np_))

# 目标在画面下方 dy>0 → 相机要低头 → tilt 减小（正 tilt = 抬头）
np_, nt_, _, _, dy = compute_control(
    0.50, 0.50 + 0.20, 0.0, 0.0,
    KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP)
check(nt_ < 0, "目标偏下 dy=%+.2f → tilt 减小到 %+.2f（低头）" % (dy, nt_))

# 目标在画面下方 + invert_tilt → 方向反过来
np_, nt_inv, _, _, _ = compute_control(
    0.50, 0.50 + 0.20, 0.0, 0.0,
    KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP,
    invert_tilt=True)
check(nt_inv > 0, "--invert-tilt 后 tilt 反向 (%+.2f)" % nt_inv)

# ---------- 4. 死区与限速 ----------
print("\n[4] 死区与限速")
_, _, in_dz, _, _ = compute_control(
    0.53, 0.53, 10.0, 5.0,
    KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP)
check(in_dz, "dx=dy=0.03 < deadzone 0.05 → 判定居中，不动")

np_, nt_, _, _, _ = compute_control(
    1.0, 1.0, 0.0, 0.0,
    KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP)
check(abs(np_) <= MAX_STEP + 1e-9 and abs(nt_) <= MAX_STEP + 1e-9,
      "dx=dy=0.5 极端偏差 → 被 max_step 夹到 pan=%+.2f tilt=%+.2f（%d°/帧）"
      % (np_, nt_, MAX_STEP))
check(abs(np_ - MAX_STEP) < 1e-9 or abs(nt_ - MAX_STEP) < 1e-9,
      "确实顶到了限速上界")

# 行程夹紧
np_, nt_, _, _, _ = compute_control(
    1.0, 1.0, PAN_R, TILT_R,
    KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP)
check(np_ <= PAN_R and nt_ <= TILT_R, "已在极限位时不越界")

# ---------- 5. 闭环收敛仿真 ----------
print("\n[5] 闭环收敛仿真（摄像头随云台动）")
# 物理模型：目标在世界上固定于 pan_world。摄像头视场 FOV_PAN，
# 画面中心对应云台当前 pan，目标归一化偏移 dx = (pan_world - pan)/FOV_PAN
# 注意符号：目标在右边（相对相机朝向）→ dx > 0 → pan_world > pan
pan = 0.0
pan_world = 25.0
log = []
for _ in range(200):
    dx_world = (pan_world - pan) / FOV_PAN
    target_cx = 0.5 + dx_world
    new_pan, _, in_dz, _, _ = compute_control(
        target_cx, 0.5, pan, 0.0,
        KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP)
    log.append(pan)
    if in_dz:
        break
    pan = new_pan

print("    目标世界角度 %.1f°，迭代 %d 步后 pan=%.2f°" % (pan_world, len(log), pan))
print("    轨迹（前 8 步）：%s" % ["%.1f" % v for v in log[:8]])
err = abs(pan - pan_world)
deadzone_deg = DEADZONE * FOV_PAN
print("    最终误差 %.2f°（死区对应 %.2f°）" % (err, deadzone_deg))
check(pan > 0, "收敛方向正确（目标在 +25°，云台往正方向转）")
check(err <= deadzone_deg + 1e-6, "误差落在死区内")
check(len(log) < 60, "收敛速度合理（%d 步，未发散）" % len(log))

# 反向目标
pan = 0.0
pan_world = -30.0
log2 = []
for _ in range(200):
    dx_world = (pan_world - pan) / FOV_PAN
    new_pan, _, in_dz, _, _ = compute_control(
        0.5 + dx_world, 0.5, pan, 0.0,
        KP_PAN, KP_TILT, FOV_PAN, FOV_TILT, PAN_R, TILT_R, DEADZONE, MAX_STEP)
    log2.append(pan)
    if in_dz:
        break
    pan = new_pan
check(pan < 0 and abs(pan - pan_world) <= deadzone_deg + 1e-6,
      "负方向目标 %.0f° 也能收敛（pan=%.2f°）" % (pan_world, pan))

print("\n" + "=" * 70)
if FAIL == 0:
    print("自测全部通过 ✅  (%d 项)" % 18)
else:
    print("有 %d 项失败 ❌" % FAIL)
print("=" * 70)
sys.exit(0 if FAIL == 0 else 1)
