#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
capture_pairs.py —— PC 端「双目标定抓图调度器」

它不直接开摄像头，而是当「遥控器」：
  1. 调 ssh 在 RK3588 板上跑 tools/capture_pairs.cpp 编出来的 capture_pairs 二进制
     （摄像头长在板子上，PC 端碰不到 /dev/video81）
  2. 板端抓完，用 scp 把 left_*.png / right_*.png 拉回 PC 本地目录
  3. 对每一对图跑 findChessboardCorners 验收角点数
     —— 抓的时候就告诉你「这张能不能用」，省得拍完 30 对才发现一半是废的

为什么要在 PC 端做角点验收？
  cv2.findChessboardCorners 是「能不能标定」的直接判据：
  9x6 内角点的板子，标准应有 54 个角点。角点少 = 棋盘格没拍全 / 反光 / 太模糊，
  这种图丢进 stereoCalibrate 会把 RMS 拉高，甚至让优化发散。

用法：
    # 抓 30 对（板端自动模式，每对间隔 1.5 秒）
    python tools/calib/capture_pairs.py --out ./pairs --count 30

    # 只做角点检查，不抓图（检查本地已有的一批图）
    python tools/calib/capture_pairs.py --check-only --out ./pairs

    # 换板子地址 / 分辨率 / 棋盘格内角点
    python tools/calib/capture_pairs.py --host root@<board-ip> --w 1280 --h 720
    python tools/calib/capture_pairs.py --pattern 9x6 --count 20

    # 交互模式：板端每按一次回车抓一对（需要 -t 分配伪终端）
    python tools/calib/capture_pairs.py --interactive

    # 抓完不删板端文件（默认保留板端文件，方便复查；加 --clean-remote 删掉）
    python tools/calib/capture_pairs.py --count 30 --clean-remote
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np

# 板端默认路径（和 tools/capture_pairs.cpp 的部署位置一致）
DEFAULT_BOARD_DIR = "/root/stereo3d"
DEFAULT_BOARD_BIN = "capture_pairs"          # /root/stereo3d/capture_pairs
DEFAULT_BOARD_OUT = "/root/stereo3d/calib/captures"

# 角点少于这个数就认为这张图不可用（低于 15 个角点基本没法参与标定）
MIN_USABLE_CORNERS = 15


# ---------------------------------------------------------------------------
# 远程执行小工具
#   刻意用 subprocess.run([...]) 传「参数列表」而不是拼一整条 shell 字符串：
#   Windows 下 PowerShell / cmd 会把引号、$() 吃掉（项目里踩过，见 AGENTS.md
#   「Windows 上跑远程命令别用复杂内联」）。传列表时每个参数原样到达 ssh，
#   板端再用 `bash -lc '...'` 包一层让 $(pkg-config ...) 这类展开在板端发生。
# ---------------------------------------------------------------------------
def run(cmd, *, check=True, capture=True, timeout=None, input_text=None):
    """跑一条本地命令，返回 CompletedProcess。失败时打印 stderr 方便排错。"""
    printable = " ".join(cmd) if isinstance(cmd, (list, tuple)) else cmd
    print(f"  $ {printable}", flush=True)
    proc = subprocess.run(
        cmd,
        input=input_text,
        capture_output=capture,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )
    if check and proc.returncode != 0:
        sys.stderr.write(f"[!] 命令失败（exit={proc.returncode}）: {printable}\n")
        if proc.stdout:
            sys.stderr.write(proc.stdout[-4000:] + "\n")
        if proc.stderr:
            sys.stderr.write(proc.stderr[-4000:] + "\n")
        raise SystemExit(proc.returncode)
    return proc


def ssh(host, remote_cmd, *, check=True, timeout=None, tty=False, input_text=None):
    """在板子上执行一段命令。remote_cmd 由板端 bash 解析。"""
    cmd = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]
    if tty:
        # 交互模式要伪终端，否则板端 getline 读不到按键
        cmd.append("-tt")
    cmd += [host, remote_cmd]
    return run(cmd, check=check, timeout=timeout, input_text=input_text)


def scp_from(host, remote_glob, local_dir):
    """
    从板子拉文件到本地目录。
    远程路径写成 `host:/path/left_*.png`，scp 会把这一段交给板端 shell 展开通配符，
    所以这里**不能**给远程路径加引号（加了就变成字面量文件名，拉不到东西）。
    """
    remote_path = f"{host}:{remote_glob}"
    cmd = ["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
           remote_path, str(local_dir)]
    return run(cmd, check=False)


# ---------------------------------------------------------------------------
# 棋盘格角点验收
# ---------------------------------------------------------------------------
def detect_corners(gray, pattern_size):
    """
    检测内角点。先用 findChessboardCorners 的标准组合，
    失败再退到更激进的 ADAPTIVE_THRESH + NORMALIZE_IMAGE（对付反光/暗角）。
    返回 (corners 或 None)
    """
    flags_sets = [
        cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE,
        cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE + cv2.CALIB_CB_FILTER_QUADS,
    ]
    for flags in flags_sets:
        ok, corners = cv2.findChessboardCorners(gray, pattern_size, flags)
        if ok and corners is not None and len(corners) > 0:
            return corners
    return None


def check_pair_files(pairs_dir, pattern_size, expected_corners, verbose=True):
    """
    检查 pairs_dir 下的 left_*.png / right_*.png。
    返回 list[dict]：每对一条，含左右角点数、是否通过、文件路径。
    """
    p = Path(pairs_dir)
    lefts = sorted(p.glob("left_*.png"))
    rights = sorted(p.glob("right_*.png"))
    if not lefts and not rights:
        print(f"[!] {p} 里没找到 left_*.png / right_*.png")
        return []

    results = []
    # 按编号配对（不按排序下标配，避免左右数量不等时错位）
    left_map = {f.stem.split("_")[-1]: f for f in lefts}
    right_map = {f.stem.split("_")[-1]: f for f in rights}
    keys = sorted(set(left_map) | set(right_map))

    print(f"\n{'=' * 74}")
    print(f"角点验收：pattern={pattern_size[0]}x{pattern_size[1]} 内角点，理论 {expected_corners} 个")
    print(f"{'=' * 74}")

    ok_cnt = 0
    for i, k in enumerate(keys, 1):
        fl = left_map.get(k)
        fr = right_map.get(k)
        row = {"idx": k, "left": fl, "right": fr, "nL": 0, "nR": 0, "ok": False}

        def one(fpath):
            if fpath is None or not fpath.exists():
                return 0, None
            img = cv2.imread(str(fpath), cv2.IMREAD_GRAYSCALE)
            if img is None:
                return 0, None
            c = detect_corners(img, pattern_size)
            return (0 if c is None else len(c)), c

        row["nL"], _ = one(fl)
        row["nR"], _ = one(fr)
        row["ok"] = (row["nL"] >= expected_corners and row["nR"] >= expected_corners)
        if row["ok"]:
            ok_cnt += 1
            mark = "OK"
        elif row["nL"] < MIN_USABLE_CORNERS or row["nR"] < MIN_USABLE_CORNERS:
            mark = "XX"
        else:
            mark = "!#"

        results.append(row)
        if verbose or not row["ok"]:
            tag = {"OK": "✓", "XX": "✗", "!#": "△"}[mark]
            extra = ""
            if mark == "XX":
                extra = "  <- 角点太少，这张要重拍（是没拍全？反光？还是太模糊？）"
            elif mark == "!#":
                extra = "  <- 角点数不全但够用，建议补拍"
            print(f"第 {i:>2} 对 [{k}]: 左 {row['nL']:>2} 角点 / 右 {row['nR']:>2} 角点 {tag}{extra}")

    print(f"{'-' * 74}")
    print(f"通过 {ok_cnt} / {len(keys)} 对（要求左右都 >= {expected_corners} 个角点）")
    bad = len(keys) - ok_cnt
    if bad:
        print(f"[建议] {bad} 对不合格。标定建议 15 对以上合格图，"
              f"脚本会对不合格的自动跳过，不用手动删文件。")
    return results


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="RK3588 双摄标定抓图调度器（PC 端遥控板端抓图 + 角点验收）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--host", default=os.environ.get("STEREO3D_BOARD", ""),
                    help="板子 SSH 目标，如 root@<board-ip>（也可用环境变量 STEREO3D_BOARD；IP 不入库）")
    ap.add_argument("--out", default="./pairs", help="本地输出目录（默认 ./pairs）")
    ap.add_argument("--count", type=int, default=30, help="抓多少对（默认 30）")
    ap.add_argument("--interval", type=float, default=1.5, help="自动模式每对间隔秒（默认 1.5）")
    ap.add_argument("--pattern", default="9x6", help="棋盘格内角点 COLxROW（默认 9x6）")
    ap.add_argument("--left", default="/dev/video81", help="板端左摄设备（默认 /dev/video81）")
    ap.add_argument("--right", default="/dev/video83", help="板端右摄设备（默认 /dev/video83）")
    ap.add_argument("--w", type=int, default=1280, help="宽（默认 1280）")
    ap.add_argument("--h", type=int, default=720, help="高（默认 720）")
    ap.add_argument("--fps", type=int, default=30, help="帧率（默认 30）")
    ap.add_argument("--board-dir", default=DEFAULT_BOARD_DIR, help=f"板端部署目录（默认 {DEFAULT_BOARD_DIR}）")
    ap.add_argument("--board-out", default=DEFAULT_BOARD_OUT, help=f"板端抓图输出目录（默认 {DEFAULT_BOARD_OUT}）")
    ap.add_argument("--interactive", action="store_true", help="交互模式：板端每回车抓一对（需伪终端）")
    ap.add_argument("--check-only", action="store_true", help="跳过抓图，只对本地已有图做角点验收")
    ap.add_argument("--no-pull", action="store_true", help="只在板端抓图并验收，不拉回本地")
    ap.add_argument("--clean-remote", action="store_true", help="抓完删除板端输出目录里的图")
    ap.add_argument("--skip-check", action="store_true", help="跳过角点验收（只抓不检查）")
    args = ap.parse_args()

    # 解析棋盘格内角点
    try:
        pw, ph = args.pattern.lower().split("x")
        pattern_size = (int(pw), int(ph))
    except Exception:
        print(f"[!] --pattern 格式应为 9x6 这种，收到: {args.pattern}")
        return 2
    expected_corners = pattern_size[0] * pattern_size[1]

    local_dir = Path(args.out).expanduser().resolve()
    local_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 74)
    print("RK3588 双目标定抓图调度器")
    print("=" * 74)
    print(f"板子     : {args.host}")
    print(f"本地目录 : {local_dir}")
    print(f"棋盘格   : {pattern_size[0]}x{pattern_size[1]} 内角点（= {expected_corners} 个角点）")
    print(f"分辨率   : {args.w}x{args.h} @ {args.fps}fps")

    # ---------------- 只检查 ----------------
    if args.check_only:
        results = check_pair_files(local_dir, pattern_size, expected_corners)
        good = sum(1 for r in results if r["ok"])
        print(f"\n结论: {good} 对可用" + ("，可以直接跑 stereo_calibrate.py" if good >= 15 else "，太少，建议再抓"))
        return 0 if good > 0 else 3

    # ---------------- 1. 检查板端环境 ----------------
    if not args.host:
        print("[FATAL] 没有目标板地址。请用 --host root@<board-ip> 或设置环境变量 "
              "STEREO3D_BOARD=root@<board-ip>（板子内网 IP 不入库）")
        return 2
    print("\n[1/5] 检查板子连通性和板端程序 ...")
    who = ssh(args.host, "uname -m; which pkg-config >/dev/null && echo pkgconfig-ok || echo pkgconfig-missing", check=False)
    if who.returncode != 0:
        print(f"[!] 连不上板子 {args.host}。检查: 网线插了没 / 该地址能直接 ssh 登吗")
        return 4
    print("      " + who.stdout.strip().replace("\n", " / "))

    bin_path = f"{args.board_dir}/{DEFAULT_BOARD_BIN}"
    chk = ssh(args.host, f'test -x "{bin_path}" && echo present || echo missing', check=False)
    if "present" not in chk.stdout:
        print(f"[!] 板端没有可执行文件 {bin_path}")
        print("    先在板上编一次（Windows 上别用内联 ssh 拼命令，容易掉引号）:")
        print(f"      scp tools/capture_pairs.cpp {args.host}:{args.board_dir}/")
        print(f"      ssh {args.host} 'cd {args.board_dir} && "
              f"g++ -O2 -std=c++17 capture_pairs.cpp $(pkg-config --cflags --libs opencv4) -o capture_pairs'")
        return 5
    print(f"      板端程序就绪: {bin_path}")

    # ---------------- 2. 摄像头占用检查 ----------------
    print("\n[2/5] 检查摄像头是否被占用 ...")
    fuser_cmd = (f'for d in "{args.left}" "{args.right}"; do '
                 f'echo -n "$d: "; fuser -v "$d" 2>&1 | tail -n +2 | head -3 || true; '
                 f'echo; done')
    fu = ssh(args.host, fuser_cmd, check=False)
    print("      " + (fu.stdout.strip() or "(无占用)").replace("\n", "\n      "))

    # ---------------- 3. 板端抓图 ----------------
    print("\n[3/5] 板端抓图 ...")
    ssh(args.host, f'mkdir -p "{args.board_out}" && rm -f "{args.board_out}"/left_*.png "{args.board_out}"/right_*.png', check=False)

    if args.interactive:
        print("      交互模式：板端每按一次回车抓一对，输入 q 回车结束。")
        print("      板端提示会实时显示格式为 `[000] 同步差 grab_skew = ...` 的实测数字。")
        cap_args = (f'./{DEFAULT_BOARD_BIN} --left "{args.left}" --right "{args.right}" '
                    f'--out "{args.board_out}" --w {args.w} --h {args.h} --fps {args.fps}')
        ssh(args.host, f'cd "{args.board_dir}" && {cap_args}', check=False, tty=True)
    else:
        cap_args = (f'./{DEFAULT_BOARD_BIN} --left "{args.left}" --right "{args.right}" '
                    f'--out "{args.board_out}" --w {args.w} --h {args.h} --fps {args.fps} '
                    f'--auto {args.count} --interval {args.interval}')
        # 抓图可能要几分钟（30 对 x 1.5s + 预热），给足超时
        timeout = max(120, int(args.count * (args.interval + 1.0) + 120))
        r = ssh(args.host, f'cd "{args.board_dir}" && {cap_args}', check=False, timeout=timeout)
        if r.returncode != 0:
            print(f"[!] 板端抓图返回 {r.returncode}，看上面的报错。常见原因:")
            print("    - 摄像头被别的进程占用（fuser -v /dev/video81 查）")
            print("    - 设备号变了（ls /dev/video* 看看）")
            return 6

    # ---------------- 4. 拉回本地 ----------------
    if args.no_pull:
        print("\n[4/5] --no-pull：跳过拉回，只检查板端文件")
        check_dir = None
    else:
        print("\n[4/5] 从板子拉回本地 ...")
        for tag in ("left", "right"):
            scp_from(args.host, f'{args.board_out}/{tag}_*.png', local_dir)
        n_left = len(list(local_dir.glob("left_*.png")))
        n_right = len(list(local_dir.glob("right_*.png")))
        print(f"      本地现有: left {n_left} 张 / right {n_right} 张")
        if args.clean_remote:
            ssh(args.host, f'rm -f "{args.board_out}"/*.png "{args.board_out}"/capture_log.txt', check=False)
            print("      已清理板端图片（--clean-remote）")
        check_dir = local_dir

    # ---------------- 5. 角点验收 ----------------
    if args.skip_check or check_dir is None:
        print("\n[5/5] 跳过角点验收")
        print("\n下一步: python tools/calib/stereo_calibrate.py --pairs " + str(local_dir))
        return 0

    print("\n[5/5] 角点验收 ...")
    results = check_pair_files(check_dir, pattern_size, expected_corners)
    good = sum(1 for r in results if r["ok"])
    print()
    if good >= 15:
        print(f"[OK] {good} 对可用。下一步:")
        print(f"     python tools/calib/stereo_calibrate.py --pairs {local_dir} --square-size 0.025")
        return 0
    print(f"[!] 只有 {good} 对可用（标定建议 >= 15 对）。再抓一批，或检查:")
    print("    - 棋盘格是否完整入镜（角点少通常是没拍全）")
    print("    - 是否有反光/过曝/太暗（打印在硬纸板上、别用光面纸）")
    print("    - 距离是否变化（别一直同一个位置）")
    return 3


if __name__ == "__main__":
    sys.exit(main())
