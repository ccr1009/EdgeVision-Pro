#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_calib.py —— 双目矫正验收：「这份标定到底行不行」的量化证据

标定脚本只吐一个 RMS，RMS 小不等于矫正对了（可能是过拟合，或者左右配错了）。
这个脚本把标定结果真正用起来，量三件事：

  ① 极线对齐误差（核心指标）
     同一块棋盘格在左图第 i 个角点和右图第 i 个角点，是空间中的同一个物理点
     （"同名点"）。矫正理想时，它们在两张矫正图里的 **y 坐标必须相等**。
     所以量 |y_left - y_right| 的中位数：
       矫正前：几像素到几十像素（取决于两摄像头装得多歪）
       矫正后：应接近 0（本项目实测 0.1~0.3 px 量级，小于 0.5 px 就算优秀）
     这是唯一一个「能证明矫正真的生效」的数字，比 RMS 有说服力。

  ② 视差有效性
     矫正后跑 SGBM，统计有效视差像素占比。太低说明两图还没对齐（匹配不上），
     或者纹理太少、曝光差太大。

  ③ 可视化
     verify_epipolar.jpg  矫正后左右图 + 水平极线网格（同名点应该落在同一条线上）
     verify_rectified.jpg 左 | 右 | 视差 三联图

用法：
    python tools/calib/verify_calib.py --pairs ./pairs --calib stereo_calib.yml
    python tools/calib/verify_calib.py --pairs ./pairs --calib stereo_calib.yml --num-disp 128
    python tools/calib/verify_calib.py --pairs ./pairs --calib stereo_calib.yml --no-sgbm
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np


# ---------------------------------------------------------------------------
# 读标定文件（键名严格按 docs/CALIB_FORMAT.md）
# ---------------------------------------------------------------------------
def load_calib(path):
    fs = cv2.FileStorage(str(path), cv2.FILE_STORAGE_READ)
    if not fs.isOpened():
        raise SystemExit(f"[!] 打不开标定文件 {path}（先跑 stereo_calibrate.py）")

    def mat(key):
        m = fs.getNode(key).mat()
        if m is None:
            raise SystemExit(f"[!] 标定文件缺键: {key}（检查是否偏离 docs/CALIB_FORMAT.md）")
        return np.asarray(m, dtype=np.float64)

    def scalar(key):
        n = fs.getNode(key)
        if n.empty():
            raise SystemExit(f"[!] 标定文件缺键: {key}")
        # cv2.FileNode 在 OpenCV 5.0 绑定里没有 int_value()，real() 对 int 节点也适用
        return n.real() if n.isReal() else float(n.real())

    calib = {
        "w": int(scalar("image_width")),
        "h": int(scalar("image_height")),
        "K1": mat("K_left"), "D1": mat("D_left").reshape(-1),
        "K2": mat("K_right"), "D2": mat("D_right").reshape(-1),
        "R": mat("R"), "T": mat("T").reshape(-1, 1),
        "R1": mat("R1"), "R2": mat("R2"),
        "P1": mat("P1"), "P2": mat("P2"), "Q": mat("Q"),
        "rms": float(scalar("rms")),
        "baseline_m": float(scalar("baseline_m")),
    }
    node = fs.getNode("calib_date")
    calib["date"] = node.string() if not node.empty() else "(未记录)"
    fs.release()
    return calib


# ---------------------------------------------------------------------------
# 角点检测（和 stereo_calibrate.py 口径一致）
# ---------------------------------------------------------------------------
def detect_corners(gray, pattern_size):
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


def refine(gray, corners):
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-4)
    return cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), crit)


def epipolar_stats(pts_l, pts_r):
    """
    同名点 y 差。pts_* 是 (N,1,2) 或 (N,2)。
    返回 dict: median_abs（中位数，核心指标）, mean, p95, max, signed_median（有符号，看系统性偏移）
    """
    a = np.asarray(pts_l, dtype=np.float64).reshape(-1, 2)
    b = np.asarray(pts_r, dtype=np.float64).reshape(-1, 2)
    if len(a) != len(b) or len(a) == 0:
        return None
    dy = a[:, 1] - b[:, 1]
    ady = np.abs(dy)
    return {
        "n": len(dy),
        "median_abs": float(np.median(ady)),
        "mean": float(np.mean(ady)),
        "p95": float(np.percentile(ady, 95)),
        "max": float(np.max(ady)),
        "signed_median": float(np.median(dy)),
    }


# ---------------------------------------------------------------------------
# 画极线网格
# ---------------------------------------------------------------------------
def draw_epipolar_grid(img, step=40, color=(0, 200, 0)):
    """在图上画水平线：矫正正确时，左右图的同名点应该落在同一条水平线上。"""
    out = img.copy()
    h, w = out.shape[:2]
    for y in range(step, h, step):
        cv2.line(out, (0, y), (w - 1, y), color, 1, cv2.LINE_AA)
    # 每 5 条加一条粗线方便数
    for y in range(step * 5, h, step * 5):
        cv2.line(out, (0, y), (w - 1, y), (0, 120, 255), 1, cv2.LINE_AA)
    return out


def colorize_disparity(disp, num_disp, min_disp=0):
    """视差伪彩色。无效像素（<= min_disp 或 NaN）涂黑，别让它们混进颜色里冒充好点。"""
    valid = disp > min_disp
    vis = np.zeros((disp.shape[0], disp.shape[1], 3), np.uint8)
    if not np.any(valid):
        return vis, valid
    v = disp[valid]
    lo, hi = float(np.percentile(v, 1)), float(np.percentile(v, 99))
    if hi - lo < 1e-6:
        hi = lo + 1.0
    norm = np.clip((disp - lo) / (hi - lo), 0.0, 1.0)
    norm8 = (norm * 255).astype(np.uint8)
    colored = cv2.applyColorMap(norm8, cv2.COLORMAP_JET)
    vis[valid] = colored[valid]
    return vis, valid


def main():
    ap = argparse.ArgumentParser(
        description="双目矫正验收：极线对齐误差 + 视差有效性 + 可视化",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--pairs", default="./pairs", help="图对目录（默认 ./pairs）")
    ap.add_argument("--calib", default="stereo_calib.yml", help="标定文件（默认 stereo_calib.yml）")
    ap.add_argument("--pattern", default="9x6", help="棋盘格内角点（默认 9x6）")
    ap.add_argument("--num-disp", type=int, default=128, help="SGBM 视差搜索范围，须为 16 的倍数（默认 128）")
    ap.add_argument("--max-pairs", type=int, default=5, help="验收多少对图（默认 5，够看趋势）")
    ap.add_argument("--out-dir", default=".", help="图片输出目录（默认当前目录）")
    ap.add_argument("--no-sgbm", action="store_true", help="跳过 SGBM（只算极线误差，快很多）")
    ap.add_argument("--full-res-sgbm", action="store_true",
                    help="SGBM 在全分辨率跑（默认降到宽 1280，慢；小图默认全分辨率）")
    args = ap.parse_args()

    calib = load_calib(args.calib)
    W, H = calib["w"], calib["h"]

    try:
        pw, ph = args.pattern.lower().split("x")
        pattern_size = (int(pw), int(ph))
    except Exception:
        print(f"[!] --pattern 格式应为 9x6，收到: {args.pattern}")
        return 2

    print("=" * 74)
    print("双目矫正验收 verify_calib")
    print("=" * 74)
    print(f"标定文件 : {Path(args.calib).resolve()}")
    print(f"标定日期 : {calib['date']}")
    print(f"图像尺寸 : {W}x{H}")
    print(f"标定 RMS : {calib['rms']:.4f} px")
    print(f"基线     : {calib['baseline_m'] * 1000:.2f} mm")
    print(f"棋盘格   : {pattern_size[0]}x{pattern_size[1]} 内角点")

    # ---- 收集图对 ----
    p = Path(args.pairs)
    lm = {f.stem.split("_")[-1]: f for f in p.glob("left_*.png")}
    rm = {f.stem.split("_")[-1]: f for f in p.glob("right_*.png")}
    keys = sorted(set(lm) & set(rm))[:args.max_pairs]
    if not keys:
        print(f"[!] {p} 里没有成对的 left_*.png / right_*.png")
        return 3
    print(f"验收图对 : {len(keys)} 对 ({keys[0]} ... {keys[-1]})\n")

    # ------------------------------------------------------------------
    # 矫正映射表：把「原始畸变图」映射到「理想平行双目图」
    #   P1/P2 是矫正后的投影矩阵，决定了矫正图的焦距和主点；
    #   传 (W,H) 表示输出和原图同尺寸。
    #   注意这里一算一次、复用到所有图 —— 这就是板端离线预计算 mapL1/mapL2 的原理，
    #   remap 一次只要几毫秒，而初始化映射表要几百毫秒，不能每帧算。
    # ------------------------------------------------------------------
    print("[1/3] 生成矫正映射表 ...")
    mapL1, mapL2 = cv2.initUndistortRectifyMap(
        calib["K1"], calib["D1"], calib["R1"], calib["P1"], (W, H), cv2.CV_16SC2)
    mapR1, mapR2 = cv2.initUndistortRectifyMap(
        calib["K2"], calib["D2"], calib["R2"], calib["P2"], (W, H), cv2.CV_16SC2)
    print(f"      mapL 形状={mapL1.shape}  mapR 形状={mapR1.shape}")

    # ------------------------------------------------------------------
    # 逐对处理
    # ------------------------------------------------------------------
    before_all, after_all = [], []
    last_triplet = None
    sgbm_valid_ratios = []
    sgbm_median_disps = []

    # SGBM 参数：和 probe_dual_cam.cpp / stereo_pipeline.cpp 保持同一套思路
    #   numDisparities 必须是 16 的倍数；blockSize 必须奇数
    if not args.no_sgbm:
        nd = int(args.num_disp)
        nd = max(16, (nd // 16) * 16)
        sgbm = cv2.StereoSGBM_create(
            minDisparity=0,
            numDisparities=nd,
            blockSize=5,
            P1=8 * 5 * 5,
            P2=32 * 5 * 5,
            disp12MaxDiff=1,
            uniquenessRatio=10,
            speckleWindowSize=100,
            speckleRange=32,
            mode=cv2.StereoSGBM_MODE_SGBM_3WAY,
        )
        print(f"      SGBM: numDisparities={nd} blockSize=5 mode=SGBM_3WAY")
    else:
        nd = args.num_disp
        sgbm = None

    print("\n[2/3] 逐对矫正 + 极线误差 ...")
    print(f"      {'编号':<6}{'矫正前 y 差中位数':>20}{'矫正后 y 差中位数':>20}{'改善倍数':>12}")
    print("      " + "-" * 60)

    for k in keys:
        gl = cv2.imread(str(lm[k]), cv2.IMREAD_GRAYSCALE)
        gr = cv2.imread(str(rm[k]), cv2.IMREAD_GRAYSCALE)
        if gl is None or gr is None:
            print(f"      [{k}] 读图失败，跳过")
            continue
        if gl.shape[0] != H or gl.shape[1] != W:
            gl = cv2.resize(gl, (W, H), interpolation=cv2.INTER_AREA)
            gr = cv2.resize(gr, (W, H), interpolation=cv2.INTER_AREA)

        # ---- 矫正前：直接在原图上找角点，量 y 差 ----
        cl0 = detect_corners(gl, pattern_size)
        cr0 = detect_corners(gr, pattern_size)
        st0 = None
        if cl0 is not None and cr0 is not None:
            st0 = epipolar_stats(refine(gl, cl0), refine(gr, cr0))

        # ---- 矫正 ----
        rl = cv2.remap(gl, mapL1, mapL2, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
        rr = cv2.remap(gr, mapR1, mapR2, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)

        # ---- 矫正后：在矫正图上找角点，量 y 差 ----
        cl1 = detect_corners(rl, pattern_size)
        cr1 = detect_corners(rr, pattern_size)
        st1 = None
        if cl1 is not None and cr1 is not None:
            st1 = epipolar_stats(refine(rl, cl1), refine(rr, cr1))
            # 画角点连线，肉眼也能看出同名点是否在同一水平线
        if st0:
            before_all.append(st0["median_abs"])
        if st1:
            after_all.append(st1["median_abs"])

        if st0 and st1:
            ratio = st0["median_abs"] / st1["median_abs"] if st1["median_abs"] > 1e-9 else float("inf")
            print(f"      {k:<6}{st0['median_abs']:>17.4f} px{st1['median_abs']:>17.4f} px{ratio:>11.1f}x")
        elif st1:
            print(f"      {k:<6}{'原件检测失败':>20}{st1['median_abs']:>17.4f} px{'—':>12}")
        else:
            print(f"      {k:<6}{'(该对没检测到完整棋盘格，跳过极线指标)':>50}")

        # ---- SGBM ----
        disp_vis = None
        if sgbm is not None:
            # 全分辨率 1280x720 跑 SGBM 在 PC 上约 1~2 秒/对，可接受；
            # 大分辨率（比如 4K）才需要降采样。
            if not args.full_res_sgbm and W > 1280:
                sc = 1280.0 / W
                rl_s = cv2.resize(rl, None, fx=sc, fy=sc, interpolation=cv2.INTER_AREA)
                rr_s = cv2.resize(rr, None, fx=sc, fy=sc, interpolation=cv2.INTER_AREA)
            else:
                rl_s, rr_s, sc = rl, rr, 1.0

            disp = sgbm.compute(rl_s, rr_s).astype(np.float32) / 16.0
            valid = disp > 0
            ratio = float(valid.mean())
            sgbm_valid_ratios.append(ratio)
            if np.any(valid):
                sgbm_median_disps.append(float(np.median(disp[valid])))
            disp_vis, _ = colorize_disparity(disp, nd)
            # 叠加一点左图信息，方便看出「视差大的地方是近处物体」
            tint = cv2.cvtColor(rl_s, cv2.COLOR_GRAY2BGR)
            disp_show = cv2.addWeighted(disp_vis, 0.75, tint, 0.25, 0)
            disp_show = cv2.putText(disp_show, f"valid {ratio * 100:.1f}%", (14, 38),
                                    cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2, cv2.LINE_AA)
        else:
            disp_show = np.zeros((H, W, 3), np.uint8)

        # 三联图（只在最后一对保存，避免刷屏）
        l3 = cv2.cvtColor(rl, cv2.COLOR_GRAY2BGR)
        r3 = cv2.cvtColor(rr, cv2.COLOR_GRAY2BGR)
        if disp_show.shape[:2] != (H, W):
            disp_show = cv2.resize(disp_show, (W, H), interpolation=cv2.INTER_NEAREST)
        triplet = np.hstack([l3, r3, disp_show])
        for x, label in ((10, "rectified LEFT"), (W + 10, "rectified RIGHT"), (2 * W + 10, "disparity")):
            cv2.putText(triplet, label, (x, H - 16), cv2.FONT_HERSHEY_SIMPLEX, 0.9,
                        (0, 255, 255), 2, cv2.LINE_AA)
        last_triplet = triplet

        # 极线网格图（带角点）
        if st1:
            grid_l = draw_epipolar_grid(l3)
            grid_r = draw_epipolar_grid(r3)
            a = np.asarray(cl1).reshape(-1, 2)
            b = np.asarray(cr1).reshape(-1, 2)
            for pt in a:
                cv2.circle(grid_l, (int(round(pt[0])), int(round(pt[1]))), 4, (0, 0, 255), -1)
            for pt in b:
                cv2.circle(grid_r, (int(round(pt[0])), int(round(pt[1]))), 4, (0, 0, 255), -1)
            # 量一下两点之间的 y 偏移，画成黄线（越短越对齐）
            for i in range(min(len(a), len(b))):
                y1, y2 = int(round(a[i][1])), int(round(b[i][1]))
                x = int(round(a[i][0]))
                if abs(y1 - y2) >= 1:
                    cv2.line(grid_l, (x - 14, y1), (x - 14, y2), (0, 255, 255), 2)
            epi = np.hstack([grid_l, grid_r])
            cv2.putText(epi, f"pair {k}  |dy| median = {st1['median_abs']:.3f} px",
                        (14, 34), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 255), 2, cv2.LINE_AA)
            outdir = Path(args.out_dir)
            outdir.mkdir(parents=True, exist_ok=True)
            cv2.imwrite(str(outdir / "verify_epipolar.jpg"), epi)

    # ------------------------------------------------------------------
    # 汇总
    # ------------------------------------------------------------------
    print("\n[3/3] 汇总 ...")
    print("\n" + "=" * 74)
    print("验收结果")
    print("=" * 74)

    def agg(v):
        a = np.asarray(v, dtype=float)
        return (float(np.median(a)), float(np.mean(a)), float(np.max(a))) if a.size else (0, 0, 0)

    if before_all:
        bm, bmean, bmax = agg(before_all)
        print(f"  矫正前 同名点 |Δy| 中位数 : {bm:.3f} px   (均值 {bmean:.3f}, 最大 {bmax:.3f})")
    if after_all:
        am, amean, amax = agg(after_all)
        print(f"  矫正后 同名点 |Δy| 中位数 : {am:.3f} px   (均值 {amean:.3f}, 最大 {amax:.3f})  <- 核心指标")
        if before_all:
            bm = agg(before_all)[0]
            print(f"  极线对齐改善              : {bm / am:.1f}x" if am > 1e-9 else "  极线对齐改善: ∞")
        print()
        if am <= 0.5:
            print("  [结论] 极线对齐优秀（< 0.5 px），矫正正确，可以拿去跑 SGBM 测深度")
        elif am <= 1.0:
            print("  [结论] 极线对齐可用（0.5~1.0 px），深度会有约 1%~2% 的 y 方向串扰误差")
        else:
            print("  [结论] 极线对齐不合格（> 1.0 px）！排查:")
            print("         1. 标定图对不够/姿势太单调 → 加图重标（15~30 对，覆盖画面各区域）")
            print("         2. 标定后挪动过摄像头 → 必须重新标定")
            print("         3. 左右图配错（left_/right_ 反了或编号错位）")
            print("         4. 棋盘格打印变形（非正方形）→ 量 10 格总长重算 --square-size")

    if sgbm_valid_ratios:
        vm, _, vmax = agg(sgbm_valid_ratios)
        print(f"\n  SGBM 有效视差像素占比    : {vm * 100:.1f}%  (最高 {vmax * 100:.1f}%)")
        if sgbm_median_disps:
            print(f"  SGBM 视差中位数           : {np.median(sgbm_median_disps):.2f} px")
            fx = float(calib["P1"][0, 0])
            z = fx * calib["baseline_m"] / max(np.median(sgbm_median_disps), 1e-6)
            print(f"  由视差反算的深度中位数    : {z:.3f} m  (Z = fx*B/d, fx={fx:.1f}, B={calib['baseline_m']:.4f} m)")
            print(f"    ↑ 这只是「画面里最多像素对应的深度」，用来判断量级是否合理（比如拍 1m 外的板子，")
            print(f"      结果 0.3m 或 5m 就说明视差/内参对不上）")
        if vm < 0.30:
            print("  [!] 有效视差偏低（<30%），常见原因: 画面无纹理（白墙）、两目曝光差太大、")
            print("      棋盘格已移走但图还是它（棋盘格本身就是高对比纹理，占比应该不低）")

    # ---- 存图 ----
    outdir = Path(args.out_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    if last_triplet is not None:
        trip_path = outdir / "verify_rectified.jpg"
        cv2.imwrite(str(trip_path), last_triplet)
        print(f"\n  已保存: {trip_path.resolve()}  ({last_triplet.shape[1]}x{last_triplet.shape[0]})")
    epi_path = outdir / "verify_epipolar.jpg"
    if epi_path.exists():
        print(f"  已保存: {epi_path.resolve()}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
