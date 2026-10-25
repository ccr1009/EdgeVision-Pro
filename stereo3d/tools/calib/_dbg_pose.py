# -*- coding: utf-8 -*-
"""排查 sample_pose 为什么全部被拒。"""
import math
import sys

import cv2
import numpy as np

sys.path.insert(0, "tools/calib")
import make_synthetic_pairs as M

GT = M.GT
K = np.array([[GT["fx"], 0, GT["cx"]], [0, GT["fy"], GT["cy"]], [0, 0, 1]], np.float64)
D = np.array([[GT["k1"], GT["k2"], GT["p1"], GT["p2"], GT["k3"]]], np.float64)
objp = M.board_corners_3d()
pw, ph = GT["pattern"]

rng = np.random.default_rng(2026)
reasons = {}

for trial in range(2000):
    z0 = rng.uniform(0.34, 0.95)
    tx = rng.uniform(-0.055, 0.055)
    ty = rng.uniform(-0.035, 0.035)
    rx = rng.uniform(math.radians(-32), math.radians(32))
    ry = rng.uniform(math.radians(-32), math.radians(32))
    rz = rng.uniform(math.radians(-25), math.radians(25))
    R = cv2.Rodrigues(np.array([rx, ry, rz]))[0]
    board_center = np.array([(pw - 1) * GT["square_m"] / 2.0,
                             (ph - 1) * GT["square_m"] / 2.0, 0.0])
    t = np.array([tx, ty, z0]) - R @ board_center
    rvec = cv2.Rodrigues(R)[0].ravel()

    proj, _ = cv2.projectPoints(objp, cv2.Rodrigues(R)[0], t, K, D)
    p = proj.reshape(-1, 2)
    reason = []
    if p[:, 0].min() < 18 or p[:, 0].max() > GT["w"] - 18:
        reason.append("x越界")
    if p[:, 1].min() < 18 or p[:, 1].max() > GT["h"] - 18:
        reason.append("y越界")
    try:
        area2 = cv2.contourArea(p.astype(np.float32))
    except Exception as e:
        area2 = f"ERR {e}"
    try:
        area3 = cv2.contourArea(p.astype(np.float32).reshape(-1, 1, 2))
    except Exception as e:
        area3 = f"ERR {e}"
    if isinstance(area2, float) and area2 < 0.10 * GT["w"] * GT["h"]:
        reason.append(f"面积小 area(N,2)={area2:.0f} area(N,1,2)={area3 if not isinstance(area3,float) else round(area3)}")
    if not reason:
        reasons_shape = "PASS"
    else:
        reasons_shape = ",".join(reason)
    reasons[reasons_shape] = reasons.get(reasons_shape, 0) + 1

for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:8]:
    print(f"{v:5d}  {k}")
