# -*- coding: utf-8 -*-
"""探针：确认 cv2 5.0.0 里 findChessboardCorners / SB 的调用形式与返回形状。"""
import cv2
import numpy as np

# 造一张简单棋盘：9x6 内角点 -> 10x7 方格
sq = 40
W, H = 10 * sq, 7 * sq
img = np.zeros((H, W), np.uint8)
for j in range(7):
    for i in range(10):
        if (i + j) % 2 == 0:
            img[j * sq:(j + 1) * sq, i * sq:(i + 1) * sq] = 255
img = cv2.copyMakeBorder(img, 60, 60, 60, 60, cv2.BORDER_CONSTANT, value=110)

ps = (9, 6)

print("--- 2 positional (image, patternSize) ---")
r = cv2.findChessboardCorners(img, ps)
print(type(r), len(r) if isinstance(r, tuple) else None)
print("ret=", r[0], "corners.shape=", None if r[1] is None else np.asarray(r[1]).shape)

print("--- 3 positional (image, patternSize, flags) ---")
try:
    r3 = cv2.findChessboardCorners(img, ps, cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE)
    print("ok, ret=", r3[0], "shape=", np.asarray(r3[1]).shape)
except Exception as e:
    print("FAILED:", e)

print("--- 4 positional (image, patternSize, None, flags) ---")
try:
    r4 = cv2.findChessboardCorners(img, ps, None, cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE)
    print("ok, ret=", r4[0], "shape=", np.asarray(r4[1]).shape)
except Exception as e:
    print("FAILED:", e)

print("--- findChessboardCornersSB ---")
try:
    r5 = cv2.findChessboardCornersSB(img, ps)
    print("type=", type(r5), "len=", len(r5) if isinstance(r5, tuple) else "-")
    if isinstance(r5, tuple):
        print("ret=", r5[0], "shape=", np.asarray(r5[1]).shape)
    else:
        print("shape=", np.asarray(r5).shape)
except Exception as e:
    print("SB FAILED:", e)

print("--- findChessboardCornersSB with flags ---")
try:
    r6 = cv2.findChessboardCornersSB(img, ps, cv2.CALIB_CB_NORMALIZE_IMAGE)
    print("ok:", type(r6), (r6[0] if isinstance(r6, tuple) else None))
except Exception as e:
    print("SB2 FAILED:", e)

print("--- cornerSubPix ---")
if r[0]:
    c = np.asarray(r[1], dtype=np.float32).copy()
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
    c2 = cv2.cornerSubPix(img, c, (11, 11), (-1, -1), crit)
    print("in", c.shape, "out", np.asarray(c2).shape)

print("--- stereoCalibrate needs >= ? ---")
print("cv2.CALIB_USE_INTRINSIC_GUESS =", cv2.CALIB_USE_INTRINSIC_GUESS)
print("cv2.CALIB_ZERO_TANGENT_DIST =", cv2.CALIB_ZERO_TANGENT_DIST)
print("has initInverseRectifyMap:", hasattr(cv2, "initInverseRectifyMap"))
print("has initUndistortRectifyMap:", hasattr(cv2, "initUndistortRectifyMap"))
print("has initCameraMatrix2D:", hasattr(cv2, "initCameraMatrix2D"))
