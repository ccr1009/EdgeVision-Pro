#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_contention_sampler.py —— RK3588 NPU 竞争实测采样器 (PC 端, 纯标准库)

用途: 在「仅YOLO网关 / 仅LLM / 同时跑」三种场景下, 以固定间隔远程采集板子:
  - /sys/class/thermal/thermal_zone{0,1,2}/temp   (毫摄氏度 → ℃)
  - /sys/kernel/debug/rknpu/load                  (三核负载 %, 原样解析出数字)
  - /proc/meminfo 的 MemAvailable                 (kB → MB)
结果追加写入 CSV, 列固定为:
  ts,zone0_c,zone1_c,zone2_c,npu_core0,npu_core1,npu_core2,mem_avail_mb
  (ts = 本地时间 ISO8601 含毫秒; 采样失败时该行数值字段留空, 行仍保留,
   以保证按时长对齐, 便于三场景横向对比)

用法:
    python bench_contention_sampler.py --duration 120 --interval 1 \
        [--host <board-ip>] [--out bench_xxx.csv]

依赖: 仅 Python 3.8+ 标准库 (subprocess+ssh, 无 paramiko / 无 requests)。
板子要求: ssh 免密可达。

实现说明(简单可靠优先):
  - 建立一条持久 SSH 连接: `ssh <host> bash --noprofile --norc`, 之后每个采样
    周期向其 stdin 写一行采集命令 + 唯一哨兵 echo, 从 stdout 读到哨兵为止。
    避免每秒新建 ssh 的握手开销与 Windows 命令行引号问题(命令是纯 ASCII)。
  - 连接死亡自动重连(最多连续 MAX_RECONNECT 次); 单次采样超时 SAMPLE_TIMEOUT
    秒记为空行, 不中断整体时长。
  - Ctrl+C 优雅退出: 关连接、关文件、打印已采样本数与输出路径。
"""

import argparse
import csv
import os
import queue
import re
import subprocess
import sys
import threading
import time
from datetime import datetime

# --------------------------------------------------------------------------
# 配置
# --------------------------------------------------------------------------
SSH_OPTS = ["-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            "-o", "ServerAliveInterval=15",
            "-o", "StrictHostKeyChecking=accept-new"]

SAMPLE_CMD = (
    "cat /sys/class/thermal/thermal_zone0/temp"
    " /sys/class/thermal/thermal_zone1/temp"
    " /sys/class/thermal/thermal_zone2/temp 2>/dev/null; "
    "cat /sys/kernel/debug/rknpu/load 2>/dev/null; "
    "grep -F MemAvailable /proc/meminfo 2>/dev/null; "
    "echo __SAMPLE_DONE__$?"
)
SENTINEL_PREFIX = "__SAMPLE_DONE__"

SAMPLE_TIMEOUT = 8.0      # 单次采样读回超时(秒)
MAX_RECONNECT = 3         # 连续重连上限, 超过则终止
PROGRESS_EVERY = 10       # 每 N 行打印一次进度

CSV_HEADER = ["ts", "zone0_c", "zone1_c", "zone2_c",
              "npu_core0", "npu_core1", "npu_core2", "mem_avail_mb"]


# --------------------------------------------------------------------------
# 持久 SSH shell
# --------------------------------------------------------------------------
class SshShell:
    """一条持久 ssh 连接, exec_command 等价物: 远端跑 bash, 我们循环发命令。"""

    def __init__(self, host):
        self.host = host
        self.proc = None
        self.reader = None
        self.lines = queue.Queue()

    def connect(self):
        cmd = ["ssh"] + SSH_OPTS + [self.host, "bash --noprofile --norc"]
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, bufsize=0)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()
        # 探活: 发一条 echo 确认通道就绪
        probe = "__SHELL_PROBE_%d__" % time.monotonic_ns()
        self.send_command("echo " + probe)
        try:
            self.wait_sentinel(probe, timeout=15.0)
        except RuntimeError:
            self.close()
            raise IOError("SSH 通道探活失败(免密/网络?)")

    def _pump(self):
        """后台线程: 把远端 stdout 逐行塞进队列。"""
        try:
            for raw in iter(self.proc.stdout.readline, b""):
                self.lines.put(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
        except (OSError, ValueError):
            pass
        finally:
            self.lines.put(None)  # EOF 哨兵

    def send_command(self, command):
        self.proc.stdin.write((command + "\n").encode("ascii"))
        self.proc.stdin.flush()

    def wait_sentinel(self, sentinel, timeout):
        """从队列读行直到出现 sentinel, 返回其之前累计的行(不含哨兵行)。"""
        got = []
        deadline = time.monotonic() + timeout
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise RuntimeError("等待哨兵 %s 超时" % sentinel[:30])
            try:
                line = self.lines.get(timeout=min(remain, 1.0))
            except queue.Empty:
                continue
            if line is None:
                raise RuntimeError("SSH stdout 已关闭(连接断开)")
            if line.startswith(sentinel):
                return got
            got.append(line)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def close(self):
        try:
            if self.proc and self.proc.stdin:
                self.proc.stdin.write(b"exit\n")
                self.proc.stdin.flush()
                self.proc.stdin.close()
        except OSError:
            pass
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    self.proc.kill()
                except OSError:
                    pass


# --------------------------------------------------------------------------
# 解析
# --------------------------------------------------------------------------
def parse_sample(raw_lines):
    """把一次采样的原始行解析成 CSV 行的数值字段(缺项留空)。"""
    temps, npu_pcts, mem_mb = ["", "", ""], ["", "", ""], ""
    # 1) 温区: 每行一个纯整数(毫摄氏度), cat 3 个文件按序输出
    tvals = []
    for ln in raw_lines:
        s = ln.strip()
        if s.isdigit() and len(s) >= 4 and len(s) <= 6:
            tvals.append(s)
        if len(tvals) == 3:
            break
    for i in range(min(3, len(tvals))):
        temps[i] = "%.2f" % (int(tvals[i]) / 1000.0)
    # 2) NPU load: 形如 "load core0:49% core1:12% ..." → 抽取前 3 个百分数
    for ln in raw_lines:
        if "core" in ln or "%" in ln:
            pcts = re.findall(r"(\d+)%", ln)
            if pcts:
                for i in range(min(3, len(pcts))):
                    npu_pcts[i] = pcts[i]
                break
    # 3) MemAvailable: xx kB
    for ln in raw_lines:
        if "MemAvailable" in ln:
            digits = re.findall(r"(\d+)", ln)
            if digits:
                mem_mb = "%.1f" % (int(digits[0]) / 1024.0)
            break
    return temps + npu_pcts + [mem_mb]


def take_sample(shell):
    """执行一次采样, 返回数值字段列表(失败字段留空)。抛异常=连接不可用。"""
    sent = "%s%d" % (SENTINEL_PREFIX, time.monotonic_ns())
    # SAMPLE_CMD 末尾是 "echo __SAMPLE_DONE__$?"; 把哨兵替换为唯一值
    shell.send_command(SAMPLE_CMD.replace(
        "echo %s$?" % SENTINEL_PREFIX, "echo %s$?" % sent))
    raw = shell.wait_sentinel(sent, timeout=SAMPLE_TIMEOUT)
    return parse_sample(raw)


def open_shell_with_retry(host):
    shell = SshShell(host)
    shell.connect()
    return shell


# --------------------------------------------------------------------------
# 主循环
# --------------------------------------------------------------------------
def main():
    if sys.stdout and hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(
        description="RK3588 温区/NPU负载/内存 竞争采样器 (纯标准库)")
    ap.add_argument("--host", default=os.environ.get("EDGEVISION_BOARD_HOST", ""))
    ap.add_argument("--duration", type=int, default=60,
                    help="采样总时长(秒), 默认 60")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="采样间隔(秒), 默认 1.0")
    ap.add_argument("--out", default=None,
                    help="CSV 输出路径, 默认 bench_contention_<时间戳>.csv")
    args = ap.parse_args()

    if args.interval < 0.2:
        print("[FATAL] interval 最小 0.2s (纯标准库 ssh 通道的现实下限)")
        return 2

    out_path = args.out or os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "bench_contention_%s.csv" % datetime.now().strftime("%Y%m%d_%H%M%S"))

    print("=" * 64)
    print("RK3588 竞争采样  host=%s  duration=%ss  interval=%ss" %
          (args.host, args.duration, args.interval))
    print("输出: %s" % out_path)
    print("=" * 64)

    # 预检
    try:
        shell = open_shell_with_retry(args.host)
    except (IOError, RuntimeError, OSError) as e:
        print("[FATAL] SSH 连接失败: %s" % e)
        return 2
    print("[预检] SSH 持久通道 OK, 开始采样(Ctrl+C 优雅退出)。")

    n_ok, n_fail = 0, 0
    start = time.monotonic()
    next_t = start
    reconnect_streak = 0

    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(CSV_HEADER)
        f.flush()

        while True:
            now = time.monotonic()
            if now - start >= args.duration:
                break
            ts = datetime.now().isoformat(timespec="milliseconds")
            try:
                fields = take_sample(shell)
                reconnect_streak = 0
            except (RuntimeError, OSError, ValueError) as e:
                fields = [""] * 7
                n_fail += 1
                reconnect_streak += 1
                print("[采样失败] %s (%s)" % (ts, str(e)[:120]))
                if reconnect_streak >= MAX_RECONNECT:
                    print("[FATAL] 连续 %d 次采样失败(通道不可恢复), 提前结束。"
                          % reconnect_streak)
                    break
                try:
                    shell.close()
                except OSError:
                    pass
                time.sleep(1.0)
                try:
                    shell = open_shell_with_retry(args.host)
                    print("[重连] SSH 通道已恢复。")
                except (IOError, RuntimeError, OSError) as e2:
                    print("[重连失败] %s" % str(e2)[:120])

            row = [ts] + fields
            w.writerow(row)
            f.flush()
            if fields[0] != "":
                n_ok += 1
            if (n_ok + n_fail) % PROGRESS_EVERY == 0:
                z0 = fields[0] or "?"
                print("  [%d 行] 最新: ts=%s zone0=%s℃" %
                      (n_ok + n_fail, ts, z0))

            # 定点节流: 下一采样时刻 = 本周期起点 + interval
            next_t += args.interval
            sleep_s = next_t - time.monotonic()
            if sleep_s > 0:
                time.sleep(sleep_s)
            else:
                # 已经落后(采样耗时>间隔), 追平节奏避免连续突发
                next_t = time.monotonic()

    shell.close()
    total = n_ok + n_fail
    print("\n完成: %d 行 (成功 %d / 失败 %d) → %s" %
          (total, n_ok, n_fail, out_path))
    if n_ok == 0:
        print("警告: 没有成功样本, 请检查 ssh 免密与 /sys/kernel/debug 权限。")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
