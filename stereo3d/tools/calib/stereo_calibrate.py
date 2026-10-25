#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stereo_calibrate.py —— 双目相机标定（读 PNG 对 → 输出 stereo_calib.yml）

流程（每一步都对应一个概念，README 里有大白话解释）：
  1. 对每对图找 9x6 内角点，再用 cornerSubPix 把角点位置精化到亚像素
     —— 棋盘格角点在图像里是一个「黑白交界」的鞍点，整数像素精度只有 ±0.5px，
        用二阶导数做亚像素拟合可以到 ±0.05px 量级，标定精度全靠这一步。
  2. cv2.calibrateCamera 分别标左、右目 → 各自的内参 K 和畸变 D（单目 RMS）
  3. cv2.stereoCalibrate 联合求两目的相对位姿 R、T
     —— 默认固定单目内参（CALIB_FIX_INTRINSIC），这是「够图就固定」的稳妥做法：
        双目联合优化自由度更高，图和内参不够好时容易把内参拉偏，反而 RMS 更好看
        但外参不对。加 --refine-intrinsics 可放开内参一起优化。
  4. cv2.stereoRectify 算出矫正矩阵 R1/R2、投影矩阵 P1/P2、视差矩阵 Q
     —— 矫正的目的：把两张图「转」成完全平行、且同名点同一行横坐标，
        从此视差 = xl - xr 就是水平差，SGBM 才有意义。

输出严格按 docs/CALIB_FORMAT.md 的键名（板端 loadCalib() 照这个读）。

用法：
    python tools/calib/stereo_calibrate.py --pairs ./pairs --square-size 0.025
    python tools/calib/stereo_calibrate.py --pairs ./pairs --pattern 9x6 --out stereo_calib.yml
    python tools/calib/stereo_calibrate.py --pairs ./pairs --alpha 0      # 不留黑边
"""
import argparse
import datetime
import sys
from pathlib import Path

import cv2
import numpy as np

# 同目录的 calib_io.py：解决 FileStorage 在 Windows 上不支持中文路径的坑
sys.path.insert(0, str(Path(__file__).resolve().parent))
from calib_io import file_storage_read, file_storage_write  # noqa: E402


# ---------------------------------------------------------------------------
# 角点检测（和 capture_pairs.py 里同一套，保证抓图和标定口径一致）
# ---------------------------------------------------------------------------
def detect_corners(gray, pattern_size):
    """返回 (54,1,2) float32 的亚像素角点，或 None。"""
    flags_sets = [
        cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE,
        cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE + cv2.CALIB_CB_FILTER_QUADS,
        cv2.CALIB_CB_ADAPTIVE_THRESH,
    ]
    for flags in flags_sets:
        ok, corners = cv2.findChessboardCorners(gray, pattern_size, flags)
        if ok and corners is not None and len(corners) == pattern_size[0] * pattern_size[1]:
            return corners.astype(np.float32)
    return None


def refine_corners(gray, corners):
    """
    亚像素精化。
    注意 cornerSubPix 在硬边界附近（窗越小越稳）表现更好，这里用 11x11 半窗，
    zeroZone 用 (-1,-1) 表示不设死区。
    """
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-4)
    return cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)


# ---------------------------------------------------------------------------
# 收集图对
# ---------------------------------------------------------------------------
def collect_pairs(pairs_dir):
    """
    按编号配对 left_%03d.png / right_%03d.png。
    用编号配对而不是「排序后按下标配对」：少一张图时下标配对会整体错位，
    错位的一对标定出来 RMS 会莫名其妙地大。
    """
    p = Path(pairs_dir)
    lm = {f.stem.split("_")[-1]: f for f in p.glob("left_*.png")}
    rm = {f.stem.split("_")[-1]: f for f in p.glob("right_*.png")}
    keys = sorted(set(lm) & set(rm))
    only_l = sorted(set(lm) - set(rm))
    only_r = sorted(set(rm) - set(lm))
    if only_l or only_r:
        print(f"[!] 有落单的图（不成对，已跳过）: 只有左 {only_l[:5]} 只有右 {only_r[:5]}")
    return [(k, lm[k], rm[k]) for k in keys]


def main():
    ap = argparse.ArgumentParser(
        description="双目相机标定 → stereo_calib.yml（键名严格按 docs/CALIB_FORMAT.md）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--pairs", default="./pairs", help="图对目录（默认 ./pairs）")
    ap.add_argument("--pattern", default="9x6", help="棋盘格内角点 COLxROW（默认 9x6）")
    ap.add_argument("--square-size", type=float, default=0.025,
                    help="棋盘格一个方格的边长，米（默认 0.025）")
    ap.add_argument("--out", default="stereo_calib.yml", help="输出 yml（默认 stereo_calib.yml）")
    ap.add_argument("--alpha", type=float, default=-1.0,
                    help="stereoRectify 的 alpha：-1=自动保留全部像素, 0=裁掉全部黑边（默认 -1）")
    ap.add_argument("--refine-intrinsics", action="store_true",
                    help="放开单目内参，让 stereoCalibrate 一起优化（默认固定）")
    ap.add_argument("--max-pairs", type=int, default=0, help="最多用多少对（0=全部）")
    ap.add_argument("--save-corners", action="store_true", help="把检测到的角点画出来存 corners_check/ 便于肉眼核对")
    args = ap.parse_args()

    # ---- 解析棋盘格 ----
    try:
        pw, ph = args.pattern.lower().split("x")
        pattern_size = (int(pw), int(ph))
    except Exception:
        print(f"[!] --pattern 格式应为 9x6，收到: {args.pattern}")
        return 2
    n_per_board = pattern_size[0] * pattern_size[1]

    pairs = collect_pairs(args.pairs)
    if args.max_pairs > 0:
        pairs = pairs[:args.max_pairs]
    if not pairs:
        print(f"[!] {args.pairs} 里没有成对的 left_*.png / right_*.png")
        return 3

    print("=" * 74)
    print("双目相机标定")
    print("=" * 74)
    print(f"图对目录 : {Path(args.pairs).resolve()}")
    print(f"棋盘格   : {pattern_size[0]}x{pattern_size[1]} 内角点 = {n_per_board} 个")
    print(f"方格边长 : {args.square_size} m")
    print(f"图对数量 : {len(pairs)}")
    print()

    # ------------------------------------------------------------------
    # 棋盘格 3D 坐标（物体坐标系，Z=0 平面，单位米）
    #   注意 i 走列、j 走行：和 findChessboardCorners 返回的角点顺序
    #   （逐行从左到右）必须一致，否则标出来的内参是错的。
    # ------------------------------------------------------------------
    objp = np.zeros((n_per_board, 3), np.float32)
    grid = np.mgrid[0:pattern_size[0], 0:pattern_size[1]].T.reshape(-1, 2)
    objp[:, :2] = grid * args.square_size

    img_size = None
    objpoints, pts_l, pts_r = [], [], []
    used = []
    failed = []

    print("[1/4] 检测角点 ...")
    for k, fl, fr in pairs:
        gl = cv2.imread(str(fl), cv2.IMREAD_GRAYSCALE)
        gr = cv2.imread(str(fr), cv2.IMREAD_GRAYSCALE)
        if gl is None or gr is None:
            failed.append((k, "读图失败"))
            continue
        if img_size is None:
            img_size = (gl.shape[1], gl.shape[0])
        if (gl.shape[1], gl.shape[0]) != img_size or (gr.shape[1], gr.shape[0]) != img_size:
            failed.append((k, f"分辨率不一致 {gl.shape} vs {img_size}"))
            continue

        cl = detect_corners(gl, pattern_size)
        cr = detect_corners(gr, pattern_size)
        if cl is None or cr is None:
            failed.append((k, f"角点检测失败 左{'OK' if cl is not None else 'XX'} 右{'OK' if cr is not None else 'XX'}"))
            continue

        cl = refine_corners(gl, cl)
        cr = refine_corners(gr, cr)

        objpoints.append(objp.copy())
        pts_l.append(cl)
        pts_r.append(cr)
        used.append(k)

    print(f"      成功 {len(used)} 对，跳过 {len(failed)} 对")
    for k, why in failed[:10]:
        print(f"        跳过 [{k}] {why}")
    if len(failed) > 10:
        print(f"        ... 另有 {len(failed) - 10} 对")

    if len(used) < 5:
        print(f"\n[!] 只有 {len(used)} 对可用，太少（标定至少要 5 对，建议 15~30 对）。")
        print("    重抓: python tools/calib/capture_pairs.py --out ./pairs --count 30")
        return 3
    if len(used) < 10:
        print(f"\n[!] 只有 {len(used)} 对，结果会不稳（建议 15~30 对，覆盖不同角度/距离/画面区域）")

    # 可选：把角点画出来存档，方便肉眼核对有没有「顺序反了/歪了」
    if args.save_corners:
        outdir = Path("corners_check")
        outdir.mkdir(exist_ok=True)
        for k, cl, cr in zip(used, pts_l, pts_r):
            for tag, img_path, pts in (("left", Path(args.pairs) / f"left_{k}.png", cl),
                                       ("right", Path(args.pairs) / f"right_{k}.png", cr)):
                img = cv2.imread(str(img_path))
                if img is None:
                    continue
                vis = cv2.drawChessboardCorners(img, pattern_size, pts, True)
                cv2.imwrite(str(outdir / f"{tag}_{k}.jpg"), vis)
        print(f"      角点可视化已存 {outdir.resolve()}/")

    print("\n[2/4] 单目标定 ...")
    h, w = img_size[1], img_size[0]
    # flags=0：K1,K2,K3,p1,p2 五个畸变参数全放开（普通镜头够用；
    # 如果是广角/鱼眼要换成 CALIB_RATIONAL_MODEL 或 fisheye 版）
    rms_l, K1, D1, rv_l, tv_l = cv2.calibrateCamera(objpoints, pts_l, (w, h), None, None, flags=0)
    rms_r, K2, D2, rv_r, tv_r = cv2.calibrateCamera(objpoints, pts_r, (w, h), None, None, flags=0)

    print(f"      左目单目 RMS = {rms_l:.4f} px")
    print(f"      右目单目 RMS = {rms_r:.4f} px")
    # 留一份单目结果原件：后面 stereoCalibrate 会就地覆盖 K1/D1/K2/D2，
    # 而逐对重投影误差必须用「单目那套 K/D + 单目 rvec/tvec」才算得对。
    K1_s, D1_s, K2_s, D2_s = K1.copy(), D1.copy(), K2.copy(), D2.copy()

    # ------------------------------------------------------------------
    # [3/4] 双目标定
    #   默认 CALIB_FIX_INTRINSIC：固定上面单目标出来的 K/D，只优化 R、T。
    #   好处是稳定；代价是如果单目标定本身有系统误差，误差会全部压到外参上
    #   （表现为基线偏差）。--refine-intrinsics 时改成 USE_INTRINSIC_GUESS，
    #   K/D 作为初值一起优化。
    # ------------------------------------------------------------------
    print("\n[3/4] 双目标定 ...")
    if args.refine_intrinsics:
        flags = cv2.CALIB_USE_INTRINSIC_GUESS
        mode = "放开内参（USE_INTRINSIC_GUESS）"
    else:
        flags = cv2.CALIB_FIX_INTRINSIC
        mode = "固定内参（FIX_INTRINSIC）"

    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 200, 1e-6)
    rms_stereo, K1, D1, K2, D2, R, T, E, F = cv2.stereoCalibrate(
        objpoints, pts_l, pts_r,
        K1, D1, K2, D2, (w, h),
        flags=flags, criteria=crit)

    print(f"      模式: {mode}")
    print(f"      双目 RMS = {rms_stereo:.4f} px")
    print(f"      R =\n{np.array2string(R, precision=6, suppress_small=False)}")
    print(f"      T = {T.ravel()} (m)")

    baseline_T = float(np.linalg.norm(T))

    # ---- 每对的重投影误差，找出拖后腿的坏图 ----
    # 用单目标定出来的 K/D + rvec/tvec 算，口径一致（stereoCalibrate 若放开了内参，
    # K1/D1 会被它改过，这里不能混用）
    per_pair = []
    for i, (k, ol, orr) in enumerate(zip(used, pts_l, pts_r)):
        pl, _ = cv2.projectPoints(objpoints[i], rv_l[i], tv_l[i], K1_s, D1_s)
        pr, _ = cv2.projectPoints(objpoints[i], rv_r[i], tv_r[i], K2_s, D2_s)
        el = float(np.mean(np.linalg.norm(pl.reshape(-1, 2) - ol.reshape(-1, 2), axis=1)))
        er = float(np.mean(np.linalg.norm(pr.reshape(-1, 2) - orr.reshape(-1, 2), axis=1)))
        per_pair.append((k, el, er))

    per_pair.sort(key=lambda x: max(x[1], x[2]), reverse=True)
    print("      重投影误差最大的 5 对（均值, 像素）:")
    for k, el, er in per_pair[:5]:
        print(f"        [{k}] 左 {el:.3f} / 右 {er:.3f}")
    if per_pair and max(per_pair[0][1], per_pair[0][2]) > 1.0:
        print("      <- 上面这几对明显偏大，通常是运动模糊 / 反光 / 棋盘格没贴平，建议删掉重拍")

    # ------------------------------------------------------------------
    # [4/4] 矫正
    #   CALIB_ZERO_DISPARITY：强制两目主点在 y 方向对齐（cy 相同），
    #   这是立体匹配能只沿 x 搜索的前提。
    # ------------------------------------------------------------------
    print("\n[4/4] 计算矫正矩阵 ...")
    R1, R2, P1, P2, Q, roi1, roi2 = cv2.stereoRectify(
        K1, D1, K2, D2, (w, h), R, T,
        flags=cv2.CALIB_ZERO_DISPARITY, alpha=args.alpha)

    # 契约 §2 的换算口径，逐项打印出来核对
    fx = float(P1[0, 0])
    fy = float(P1[1, 1])
    cx = float(P1[0, 2])
    cy = float(P1[1, 2])
    baseline_P = abs(float(P2[0, 3] / P2[0, 0]))

    print(f"      矫正后内参: fx={fx:.4f}  fy={fy:.4f}  cx={cx:.4f}  cy={cy:.4f}")
    print(f"      基线: norm(T) = {baseline_T:.6f} m   |P2[0,3]/P2[0,0]| = {baseline_P:.6f} m"
          f"   （两者应一致，差 {abs(baseline_T - baseline_P) * 1000:.3f} mm）")
    print(f"      alpha={args.alpha}  validROI_left={roi1}  validROI_right={roi2}")

    # ------------------------------------------------------------------
    # 写 yml —— 键名严格按 docs/CALIB_FORMAT.md，一个都不许改
    # ------------------------------------------------------------------
    out_path = Path(args.out).resolve()

    # D 统一成 1x5（CALIB_FORMAT.md 允许 1x5 或 1x8；板端按 5 参数 k1,k2,p1,p2,k3 读最稳）
    def as_row5(D):
        d = np.asarray(D, dtype=np.float64).ravel()
        if d.size == 4:
            d = np.append(d, 0.0)
        return d[:5].reshape(1, 5)

    D1w = as_row5(D1)
    D2w = as_row5(D2)

    # 必须走 calib_io 兼容层：项目路径含中文，cv2.FileStorage 直写会打不开
    # （persistence.cpp 走窄字符 fopen，Windows 不转 UTF-16）——踩过的坑不要再踩
    with file_storage_write(out_path) as fs:
        fs.write("image_width", int(w))
        fs.write("image_height", int(h))
        fs.write("K_left", np.asarray(K1, dtype=np.float64))
        fs.write("D_left", D1w)
        fs.write("K_right", np.asarray(K2, dtype=np.float64))
        fs.write("D_right", D2w)
        fs.write("R", np.asarray(R, dtype=np.float64))
        fs.write("T", np.asarray(T, dtype=np.float64).reshape(3, 1))
        fs.write("R1", np.asarray(R1, dtype=np.float64))
        fs.write("R2", np.asarray(R2, dtype=np.float64))
        fs.write("P1", np.asarray(P1, dtype=np.float64))
        fs.write("P2", np.asarray(P2, dtype=np.float64))
        fs.write("Q", np.asarray(Q, dtype=np.float64))
        fs.write("rms", float(rms_stereo))
        fs.write("baseline_m", float(baseline_T))
        fs.write("calib_date", datetime.datetime.now().astimezone().isoformat(timespec="seconds"))

    print(f"\n[OK] 已写出 {out_path}")
    print(f"     可在板端用 src/stereo_pipeline.cpp :: loadCalib() 直接读。")

    # ---- 回读自检：确认写进去的东西能被 OpenCV 原样读回来（同样走兼容层）----
    with file_storage_read(out_path) as fsr:
        keys = ["image_width", "image_height", "K_left", "D_left", "K_right", "D_right",
                "R", "T", "R1", "R2", "P1", "P2", "Q", "rms", "baseline_m", "calib_date"]
        missing = [k for k in keys if fsr.getNode(k).empty()]
        got_baseline = fsr.getNode("baseline_m").real()
        got_P2 = fsr.getNode("P2").mat()
        print(f"      回读自检: 16 个键{'全部存在 ✓' if len(missing) == 16 else '缺失 ' + str(missing)}"
              f"，baseline_m 回读={got_baseline:.6f}，P2 形状={None if got_P2 is None else got_P2.shape}")

    # ------------------------------------------------------------------
    # 质量判定
    # ------------------------------------------------------------------
    print("\n" + "=" * 74)
    print("质量报告")
    print("=" * 74)
    print(f"  单目 RMS（左/右） : {rms_l:.3f} / {rms_r:.3f} px")
    print(f"  双目 RMS          : {rms_stereo:.3f} px")
    print(f"  基线              : {baseline_T * 1000:.2f} mm")
    print(f"  矫正后 fx/fy      : {fx:.2f} / {fy:.2f} px")
    print(f"  矫正后 cx/cy      : {cx:.2f} / {cy:.2f} px")

    warn = []
    if rms_stereo > 1.0:
        warn.append(f"双目 RMS = {rms_stereo:.3f} px > 1.0，标定质量不合格")
    if rms_l > 1.0 or rms_r > 1.0:
        warn.append(f"单目 RMS 偏大（左 {rms_l:.3f} / 右 {rms_r:.3f} px > 1.0）")
    if abs(baseline_T - baseline_P) > 1e-3:
        warn.append(f"基线两处口径不一致（差 {abs(baseline_T - baseline_P) * 1000:.2f} mm），"
                    f"检查 stereoRectify 的 alpha 是否取到极端值")
    if abs(fx - K1[0, 0]) / K1[0, 0] > 0.15:
        warn.append(f"矫正后 fx({fx:.1f}) 与左目 fx({K1[0, 0]:.1f}) 相差超过 15%，"
                    f"alpha 会把焦距缩放到裁剪范围，属正常但需知情")
    if len(used) < 15:
        warn.append(f"只用了 {len(used)} 对图，建议 15~30 对")

    if warn:
        print("\n  [!] 警告:")
        for i, wmsg in enumerate(warn, 1):
            print(f"      {i}. {wmsg}")
        if rms_stereo > 1.0:
            print("""
      排查建议（按命中概率排序）:
        1. 有坏图：看上面「重投影误差最大的 5 对」，删掉重拍（运动模糊/反光最常见）
        2. 动作太单调：所有图都在同一个位置、同一个角度 → 外参不可观测。
           要覆盖：画面四角 + 中心、俯仰/偏航/翻滚各来几张、远近各来几张
        3. 棋盘格不共面：贴在软纸上会翘 → 必须贴硬纸板/亚克力，A4 打印后压平
        4. 棋盘格没打平：打印时「缩放以适应页面」会让方格变成非正方形 →
           一定要按 100% 打印，并拿游标卡尺量 10 个格子的总长反算真实 square-size
        5. 两目时间不同步：抓图时板子/板子上的物体在动 → 静止重抓
        6. 左右图配错：确认 left_/right_ 编号一一对应，不是拍反了""")
    else:
        print("\n  [OK] 所有指标在建议范围内")

    print(f"\n下一步: python tools/calib/verify_calib.py --pairs {args.pairs} --calib {args.out}")
    return 0 if rms_stereo <= 1.0 else 1


if __name__ == "__main__":
    sys.exit(main())
