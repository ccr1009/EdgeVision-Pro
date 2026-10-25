#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync_and_build.py —— 把本机 stereo3d-rk3588 同步到板上并编译（Windows 版）

用法（在 PC 的 PowerShell 里）：
    python deploy/sync_and_build.py
    python deploy/sync_and_build.py --board root@<board-ip> --remote /root/stereo3d
    python deploy/sync_and_build.py --run          # 编完直接跑

为什么不用交叉编译：
    PC 若是 Ubuntu 22（glibc 2.35），板子是 Debian 11（glibc 2.31），
    交叉编出来的二进制板上报 "GLIBC_2.34 not found"。
    板子 8 核，整个工程原生编译 < 1 分钟，不值得为它配工具链。
"""
import argparse
import os
import subprocess
import sys

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ["include", "src", "tools"]


def run(cmd, check=True, capture=False):
    print("  $ " + " ".join(cmd))
    # encoding="utf-8" + errors="replace"：板端输出可能混中英文，Windows 默认 GBK 解码会炸
    r = subprocess.run(cmd, capture_output=capture, text=True,
                       encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        if capture and r.stderr:
            print(r.stderr[-3000:])
        sys.exit(f"命令失败: {' '.join(cmd)}")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default=os.environ.get("STEREO3D_BOARD", ""),
                    help="板子 SSH 目标，如 root@<board-ip>（也可用环境变量 STEREO3D_BOARD；IP 不入库）")
    ap.add_argument("--remote", default="/root/stereo3d")
    ap.add_argument("--run", action="store_true", help="编译完直接启动 demo")
    ap.add_argument("--no-build", action="store_true", help="只同步不编译")
    args = ap.parse_args()

    if not args.board:
        print("[FATAL] 没有目标板地址。请用 --board root@<board-ip> 或设置环境变量 "
              "STEREO3D_BOARD=root@<board-ip>（板子内网 IP 不入库）")
        return 2

    print(f"[1/3] 创建板端目录 {args.board}:{args.remote}")
    subdirs = " ".join(f"{args.remote}/{d}" for d in
                       ["include", "src", "tools", "calib", "captures", "models"])
    run(["ssh", "-o", "BatchMode=yes", args.board, f"mkdir -p {subdirs}"])

    print(f"[2/3] 同步源码")
    for d in DIRS:
        src = os.path.join(PROJ, d)
        if os.path.isdir(src):
            run(["scp", "-q", "-r", src, f"{args.board}:{args.remote}/"])
    cmake = os.path.join(PROJ, "CMakeLists.txt")
    if os.path.isfile(cmake):
        run(["scp", "-q", cmake, f"{args.board}:{args.remote}/"])

    if args.no_build:
        print("[3/3] 跳过编译（--no-build）")
        return

    print("[3/3] 板上原生编译")
    build = (
        f"cd {args.remote} && "
        f"cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/tmp/cmake.log 2>&1 "
        f"|| (tail -40 /tmp/cmake.log; exit 1); "
        f"cmake --build build -j$(nproc) 2>&1 | tail -50"
    )
    r = run(["ssh", "-o", "BatchMode=yes", args.board, build], capture=True)
    print(r.stdout)

    # 检查产物
    chk = run(["ssh", "-o", "BatchMode=yes", args.board,
               f"ls -la {args.remote}/build/stereo3d_demo"], check=False, capture=True)
    if chk.returncode != 0:
        sys.exit("[-] 编译产物不存在，看上面的编译错误")
    print("[+] " + chk.stdout.strip())

    if args.run:
        print("[+] 启动 demo（Ctrl+C 退出）")
        os.execvp("ssh", ["ssh", "-t", args.board,
                          f"cd {args.remote} && ./build/stereo3d_demo --no-calib"])


if __name__ == "__main__":
    sys.exit(main())
