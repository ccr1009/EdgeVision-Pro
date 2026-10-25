#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_synthetic_pairs.py —— 造「已知真值」的合成双目棋盘格图，用来验证标定链路

为什么需要它：
  真板子抓的图**没有真值**——你标出 fx=912.4、baseline=59.8mm，但没人知道对不对。
  这个脚本反过来做：先定死内参、畸变、基线，再用它们把一块 9x6 内角点的棋盘格
  投到左右视图里，生成图对。然后拿 stereo_calibrate.py 去标，
  如果标出来的 RMS < 0.5px、反算基线 ≈ 60mm、极线误差 ≈ 0，
  就证明「标定算法本身是通的」，之后真机数据只需要担心拍得好不好。

  这样就把「算法对不对」和「数据好不好」这两件事拆开了 —— 这也是自测的意义。

渲染原理（不用 OpenGL，纯 numpy + cv2）：
  1. 棋盘格是平面 Z=0，board 坐标 (X,Y,0) 到相机像素是单应 H = K·[r1 r2 t]
     反过来用 H⁻¹ 就能对每个像素反查它落在棋盘的哪个位置，去贴图上采样 ——
     这就是最朴素的纹理映射（对每个像素做一次矩阵乘）。
  2. 畸变单独一层：先用**无畸变**针孔投影渲染理想图，
     再 remap 一次得到「有畸变」的图（initUndistortRectifyMap 正好给这个映射）。
  3. 超采样 2x 再缩回 1x，边缘才不会有锯齿 —— 锯齿会让角点亚像素精化产生偏差。

用法：
    python tools/calib/make_synthetic_pairs.py --out ./_selftest/pairs --views 18
    python tools/calib/make_synthetic_pairs.py --out ./_selftest/pairs --views 18 --seed 7
    # 只打印真值不生成
    python tools/calib/make_synthetic_pairs.py --dry-run
"""
import argparse
import json
import math
import sys
from pathlib import Path

import cv2
import numpy as np

# ---------------------------------------------------------------------------
# 真值（改这里就等于换一台"虚拟相机"）
# ---------------------------------------------------------------------------
GT = {
    "w": 1280,
    "h": 720,
    "fx": 910.0,
    "fy": 912.0,      # 故意让 fx != fy，检验标定能不能分开解出来
    "cx": 641.0,
    "cy": 358.0,
    "k1": -0.105,
    "k2": 0.052,
    "p1": 0.00042,
    "p2": -0.00031,
    "k3": -0.012,
    "baseline_m": 0.060,     # 60 mm
    "square_m": 0.025,       # 方格 25 mm
    "pattern": (9, 6),       # 内角点
}
# 右目相对左目的旋转：真实双目装歪一点点（0.6 度绕 z），检验能否解出来
GT_RVEC_RIGHT = np.array([0.0, 0.0, math.radians(0.6)], dtype=np.float64)
GT_R = cv2.Rodrigues(GT_RVEC_RIGHT)[0]
GT_T = np.array([-GT["baseline_m"], 0.0, 0.0], dtype=np.float64)  # p_R = R·p_L + T

SS = 2  # 超采样倍率


def build_texture():
    """
    棋盘格贴图。方格里 255（白）/ 25（黑），外面加一圈"纸边"（230）。
    100 px = 1 个方格，坐标 → 贴图像素的换算在 make_view 里做。
    """
    pw, ph = GT["pattern"]
    cols, rows = pw + 1, ph + 1          # 10 x 7 个方格
    sq_px = 100
    margin = 70
    tex = np.full((rows * sq_px + 2 * margin, cols * sq_px + 2 * margin), 230, np.uint8)
    for j in range(rows):
        for i in range(cols):
            v = 25 if (i + j) % 2 == 0 else 255
            y0 = margin + j * sq_px
            x0 = margin + i * sq_px
            tex[y0:y0 + sq_px, x0:x0 + sq_px] = v
    return tex, sq_px, margin, cols, rows


def render_ideal(pose_rvec, pose_t, K, tex_info, size):
    """
    无畸变针孔渲染。pose_rvec/pose_t 是 board→camera 的位姿。
    size = (w, h)，内部按 SS 超采样。
    """
    tex, sq_px, margin, cols, rows = tex_info
    w, h = size
    W, H = w * SS, h * SS

    R = cv2.Rodrigues(np.asarray(pose_rvec, np.float64))[0]
    t = np.asarray(pose_t, np.float64).reshape(3, 1)

    # 相机矩阵放大到超采样分辨率
    Ks = np.array([[K[0, 0] * SS, 0, K[0, 2] * SS],
                   [0, K[1, 1] * SS, K[1, 2] * SS],
                   [0, 0, 1]], np.float64)

    # board 平面 (X,Y,1) -> 像素 的单应
    Hm = Ks @ np.column_stack([R[:, 0], R[:, 1], t.reshape(3)])
    Hinv = np.linalg.inv(Hm)

    # 每个像素反查 board 坐标（一次矩阵乘搞定 2560x1440）
    xs = np.arange(W, dtype=np.float64)
    ys = np.arange(H, dtype=np.float64)
    gx, gy = np.meshgrid(xs, ys)
    ones = np.ones_like(gx)
    pts = np.stack([gx, gy, ones], axis=0).reshape(3, -1)     # 3 x N
    bp = Hinv @ pts                                            # 3 x N
    bw = bp[2]
    with np.errstate(divide="ignore", invalid="ignore"):
        bu = bp[0] / bw                                        # board X（米）
        bv = bp[1] / bw                                        # board Y（米）

    # board 坐标（米）-> 贴图像素
    tx = (bu / GT["square_m"]) * sq_px + margin
    ty = (bv / GT["square_m"]) * sq_px + margin
    mapx = tx.reshape(H, W).astype(np.float32)
    mapy = ty.reshape(H, W).astype(np.float32)

    # 落在贴图内的像素才贴棋盘格，其余是背景（"墙"）
    inside = ((mapx >= 0) & (mapx < tex.shape[1] - 1) &
              (mapy >= 0) & (mapy < tex.shape[0] - 1) & (bw.reshape(H, W) > 0))

    rng = np.random.default_rng(1234)
    bg = np.full((H, W), 78, np.uint8)
    bg = cv2.add(bg, rng.integers(-6, 7, size=(H, W), dtype=np.int16).astype(np.uint8))

    sampled = cv2.remap(tex, mapx, mapy, cv2.INTER_LINEAR,
                        borderMode=cv2.BORDER_CONSTANT, borderValue=0)
    img = np.where(inside, sampled, bg).astype(np.uint8)
    return img


def add_distortion(ideal_ss, K, D, size):
    """
    给超采样的理想图加畸变。
    initUndistortRectifyMap(K_ss, D, None, K_ss, size_ss) 输出的 map 是
    「从无畸变图采样到畸变图」的映射，直接 remap 就得到畸变图。
    """
    w, h = size
    Ks = K.copy()
    Ks[0, 0] *= SS; Ks[1, 1] *= SS; Ks[0, 2] *= SS; Ks[1, 2] *= SS
    map1, map2 = cv2.initUndistortRectifyMap(Ks, D, None, Ks, (w * SS, h * SS), cv2.CV_32FC1)
    return cv2.remap(ideal_ss, map1, map2, cv2.INTER_LINEAR,
                     borderMode=cv2.BORDER_REPLICATE)


def board_corners_3d():
    """内角点的 board 坐标（米），顺序和 findChessboardCorners 逐行从左到右一致。"""
    pw, ph = GT["pattern"]
    objp = np.zeros((pw * ph, 3), np.float32)
    grid = np.mgrid[0:pw, 0:ph].T.reshape(-1, 2)
    objp[:, :2] = grid * GT["square_m"]
    return objp


def sample_pose(rng, objp, K, D, R_r, T_r):
    """
    随机采一个位姿，并要求棋盘格完整落在左右两幅图里（留 18px 边距）。
    返回 (rvec, tvec) —— 这是 board 在**左相机**坐标系下的位姿。
    """
    pw, ph = GT["pattern"]
    z0 = rng.uniform(0.34, 0.95)
    tx = rng.uniform(-0.055, 0.055)
    ty = rng.uniform(-0.035, 0.035)
    rx = rng.uniform(math.radians(-32), math.radians(32))   # 俯仰
    ry = rng.uniform(math.radians(-32), math.radians(32))   # 偏航
    rz = rng.uniform(math.radians(-25), math.radians(25))   # 翻滚

    # 把 board 中心放到相机前方 z0 处，再按 rx,ry,rz 转
    R = cv2.Rodrigues(np.array([rx, ry, rz]))[0]
    board_center = np.array([(pw - 1) * GT["square_m"] / 2.0,
                             (ph - 1) * GT["square_m"] / 2.0, 0.0])
    # 让 board 中心落在 (tx, ty, z0)
    t = np.array([tx, ty, z0]) - R @ board_center
    rvec = cv2.Rodrigues(R)[0].ravel()

    # 校验：左右两图都要「整个板子入镜」
    margin = 18
    for (RR, TT) in ((np.eye(3), np.zeros(3)), (R_r, T_r)):
        Rl = RR @ R
        tl = RR @ t + TT
        proj, _ = cv2.projectPoints(objp, cv2.Rodrigues(Rl)[0], tl, K, D)
        p = proj.reshape(-1, 2)
        if not np.all(np.isfinite(p)):
            return None
        if p[:, 0].min() < margin or p[:, 0].max() > GT["w"] - margin:
            return None
        if p[:, 1].min() < margin or p[:, 1].max() > GT["h"] - margin:
            return None
        # 板子投影面积不能太小（太远），也不能大到超出画面 3 倍（会退化）
        # 注意：objp 的角点是「逐行蛇形」顺序，直接喂 contourArea 会得到一个
        # 来回折返的窄条（面积≈0）。必须先求凸包，凸包才是棋盘格的真实外轮廓。
        hull = cv2.convexHull(p.astype(np.float32))
        area = abs(cv2.contourArea(hull))
        if area < 0.10 * GT["w"] * GT["h"]:
            return None
    return rvec, t


def main():
    ap = argparse.ArgumentParser(
        description="生成带真值的合成双目棋盘格图对（用于端到端自测）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--out", default="./_selftest/pairs", help="输出目录")
    ap.add_argument("--views", type=int, default=18, help="生成多少对（默认 18）")
    ap.add_argument("--seed", type=int, default=2026, help="随机种子（默认 2026，保证可复现）")
    ap.add_argument("--jpeg", action="store_true", help="存成 jpg（会掉精度，只用于对比实验）")
    ap.add_argument("--dry-run", action="store_true", help="只打印真值，不生成图")
    args = ap.parse_args()

    K = np.array([[GT["fx"], 0, GT["cx"]],
                  [0, GT["fy"], GT["cy"]],
                  [0, 0, 1]], np.float64)
    D = np.array([[GT["k1"], GT["k2"], GT["p1"], GT["p2"], GT["k3"]]], np.float64)

    print("=" * 74)
    print("合成双目棋盘格图对（已知真值）")
    print("=" * 74)
    print(f"真值内参  : fx={GT['fx']}  fy={GT['fy']}  cx={GT['cx']}  cy={GT['cy']}")
    print(f"真值畸变  : k1={GT['k1']}  k2={GT['k2']}  p1={GT['p1']}  p2={GT['p2']}  k3={GT['k3']}")
    print(f"真值基线  : {GT['baseline_m'] * 1000:.2f} mm")
    print(f"右目相对左目旋转: {np.degrees(GT_RVEC_RIGHT)} 度 (Rodrigues)")
    print(f"棋盘格    : {GT['pattern'][0]}x{GT['pattern'][1]} 内角点，方格 {GT['square_m'] * 1000:.1f} mm")
    print(f"图像      : {GT['w']}x{GT['h']}（渲染时 {SS}x 超采样）")
    print(f"输出      : {args.views} 对")

    if args.dry_run:
        print("\n--dry-run，不生成文件")
        return 0

    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    tex_info = build_texture()
    objp = board_corners_3d()
    rng = np.random.default_rng(args.seed)

    poses, gt_corners = [], []
    attempts = 0
    print("\n[1/2] 采样位姿（要求整个棋盘格在左右两图都完整入镜）...")
    while len(poses) < args.views and attempts < args.views * 400:
        attempts += 1
        got = sample_pose(rng, objp, K, D, GT_R, GT_T)
        if got is None:
            continue
        rvec, t = got
        poses.append((rvec, t))

    print(f"      采到 {len(poses)} 个位姿（用了 {attempts} 次尝试）")
    if len(poses) < args.views:
        print(f"[!] 只采到 {len(poses)} 个，放宽约束或多给 --views 少一点")

    # 打印位姿分布，证明「多样性」不是嘴上说的
    zs = [float(t[2]) for _, t in poses]
    tilts = []
    for rvec, _ in poses:
        R = cv2.Rodrigues(rvec)[0]
        # board 法向 (0,0,1) 转到相机系后的 z 分量 = cos(倾角)
        n = R @ np.array([0, 0, 1.0])
        tilts.append(math.degrees(math.acos(min(1.0, abs(n[2])))))
    print(f"      距离分布: {min(zs):.3f} ~ {max(zs):.3f} m（平均 {np.mean(zs):.3f}）")
    print(f"      倾角分布: {min(tilts):.1f} ~ {max(tilts):.1f} 度（平均 {np.mean(tilts):.1f}）")

    print("\n[2/2] 渲染 ...")
    ext = ".jpg" if args.jpeg else ".png"
    all_gt = []
    for idx, (rvec, t) in enumerate(poses):
        # 左目：board 位姿就是 (rvec, t)
        ideal_l = render_ideal(rvec, t, K, tex_info, (GT["w"], GT["h"]))
        img_l = add_distortion(ideal_l, K, D, (GT["w"], GT["h"]))
        img_l = cv2.resize(img_l, (GT["w"], GT["h"]), interpolation=cv2.INTER_AREA)

        # 右目：换到右相机系  p_R = R·p_L + T
        Rl = cv2.Rodrigues(np.asarray(rvec, np.float64))[0]
        Rr = GT_R @ Rl
        tr = GT_R @ np.asarray(t, np.float64) + GT_T
        rvec_r = cv2.Rodrigues(Rr)[0].ravel()

        ideal_r = render_ideal(rvec_r, tr, K, tex_info, (GT["w"], GT["h"]))
        img_r = add_distortion(ideal_r, K, D, (GT["w"], GT["h"]))
        img_r = cv2.resize(img_r, (GT["w"], GT["h"]), interpolation=cv2.INTER_AREA)

        cv2.imwrite(str(outdir / f"left_{idx:03d}{ext}"), img_l)
        cv2.imwrite(str(outdir / f"right_{idx:03d}{ext}"), img_r)

        # 记录真值角点（左目/右目像素坐标），供后面核对标定是否解对
        pl, _ = cv2.projectPoints(objp, rvec, t, K, D)
        pr, _ = cv2.projectPoints(objp, rvec_r, tr, K, D)
        all_gt.append({"idx": idx,
                       "corners_left": pl.reshape(-1, 2).tolist(),
                       "corners_right": pr.reshape(-1, 2).tolist()})
        print(f"      [{idx:02d}] 已生成  left_{idx:03d}{ext} / right_{idx:03d}{ext}")

    # 真值清单：标定脚本不需要它（标定只能看图像），它是给自测核对用的
    gt = {
        "note": "合成数据真值，仅用于自测核对；stereo_calibrate.py 不读这个文件",
        "image_width": GT["w"], "image_height": GT["h"],
        "K": K.tolist(),
        "D": D.ravel().tolist(),
        "R_true": GT_R.tolist(),
        "T_true": GT_T.tolist(),
        "baseline_m_true": GT["baseline_m"],
        "square_m": GT["square_m"],
        "pattern": list(GT["pattern"]),
        "seed": args.seed,
        "views": len(poses),
        "corners": all_gt,
    }
    with open(outdir / "ground_truth.json", "w", encoding="utf-8") as f:
        json.dump(gt, f, indent=2, ensure_ascii=False)

    print(f"\n[OK] {len(poses)} 对已写入 {outdir.resolve()}")
    print(f"     真值清单: {(outdir / 'ground_truth.json').resolve()}")
    print("\n下一步:")
    print(f"  python tools/calib/stereo_calibrate.py --pairs {outdir} --square-size {GT['square_m']} --out _selftest/stereo_calib.yml")
    print(f"  python tools/calib/verify_calib.py --pairs {outdir} --calib _selftest/stereo_calib.yml --out-dir _selftest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
