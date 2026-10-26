#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_bev_dashboard.py —— EdgeVision RK3588「道路实景 + 3D 线框 + 自车 BEV」看板端到端验收脚本

目的：用脚本证明「推流是真的、数据是真的」，而不是靠肉眼看网页。

只依赖 Python3 标准库（urllib / socket / json / time / argparse / sys / re / os / threading / zlib），
不下载任何图像库，不写死 host / port / boundary（boundary 从 Content-Type 头里正则提取）。

用法：
    python3 verify_bev_dashboard.py --host <board-ip> --port 8090 --seconds 10 --min-fps 8
    EDGEVISION_BOARD_HOST=<board-ip> python3 verify_bev_dashboard.py    # 板子内网 IP 不入库

    本机自测（不需要板子）：--host 127.0.0.1 --port <本地 mock 端口>

6 类检查：
    1) GET /            页面外壳是 HTML 且确实去拉真实接口（body 里出现 /api/stats）
    2) GET /api/stats   JSON 契约 + 数值合理 + frame_id 递增（间隔 1s 采两次）
    3) GET /snapshot    单帧 JPEG 完整（SOI/EOI）且与 stats.jpeg_bytes 同量级（±50%）
    4) GET /stream      连续读 --seconds 秒，解析 multipart，实测 fps / 每帧 Content-Length 与 JPEG 边界
    5) 并发可用性       拉流同时在另一线程打 /api/stats：200 + JSON + < 2000ms
    6) 帧内容变化性     多帧 crc32 互不相同（防「静态图冒充视频」）

退出码：0 = 全部通过；1 = 有 FAIL（任何异常都计为 FAIL，不会抛栈崩掉）。
"""

import argparse
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
import zlib

# ---------------------------------------------------------------- 常量 / 默认值
USER_AGENT = "edgevision-verify/1.1 (python-stdlib)"
DEFAULT_HOST = "127.0.0.1"          # 可用 --host 或环境变量 EDGEVISION_BOARD_HOST 覆盖
DEFAULT_PORT = 8090                 # 可用 --port 覆盖
DEFAULT_SECONDS = 10.0              # 拉流观测窗口（秒）
DEFAULT_MIN_FPS = 8.0               # 实测 fps 下限
DEFAULT_CONCURRENT_LIMIT_MS = 2000.0
HTTP_TIMEOUT = 8.0                  # 普通短请求超时
CONNECT_TIMEOUT = 5.0
PROBE_TIMEOUT = 6.0                 # 并发探测专用超时
PROBE_DELAY = 0.5                   # 拉流开始后多久发起并发探测
STATS_GAP = 1.0                     # 采两次 /api/stats 的间隔（frame_id 递增判据）
MAX_FRAMES = 3000                   # 最多收多少帧（防护）
MAX_PART_BYTES = 64 * 1024 * 1024   # 单个 part 的 Content-Length 上限
MAX_SCAN_BYTES = 32 * 1024 * 1024   # 缓冲区扫描上限

JPEG_SOI = b"\xff\xd8"
JPEG_EOI = b"\xff\xd9"

REQUIRED_STATS_KEYS = (
    "frame_id", "pipeline_fps", "npu_ms", "total_ms",
    "objects_total", "objects_3d", "resolution", "dets",
)

BOUNDARY_RE = re.compile(r'boundary\s*=\s*"?([^";,\s]+)"?', re.IGNORECASE)
STATUS_RE = re.compile(r"^HTTP/\d(?:\.\d)?\s+(\d{3})")
CONTENT_LENGTH_RE = re.compile(rb"(?im)^content-length\s*:\s*(\d+)\s*$")

RESULTS = []   # 每项检查的结果，用于最后汇总裁剪


# ---------------------------------------------------------------- 输出 / 小工具
def _configure_stdout():
    """板子/PC 控制台编码不一致时也尽量别因为中文崩掉。"""
    try:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def _url_host(host):
    """IPv6 字面量在 URL / Host 头里要加方括号。"""
    text = str(host).strip()
    if not text:
        return DEFAULT_HOST
    if text.startswith("[") or ":" not in text:
        return text
    return "[" + text + "]"


def _socket_host(host):
    """socket.connect 的 host 不能带方括号。"""
    text = str(host).strip()
    if text.startswith("[") and text.endswith("]"):
        return text[1:-1]
    return text


def _host_header(host, port):
    return "%s:%d" % (_url_host(host), int(port))


def _fmt(value, spec="%.2f"):
    """把可能缺失 / 非数值的字段格式化成可读字符串。"""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return "-"
    try:
        return spec % value
    except (TypeError, ValueError):
        return "-"


def _short(text, limit=160):
    text = "%s" % (text,)
    if len(text) <= limit:
        return text
    return text[:limit] + "...(截断)"


# ---------------------------------------------------------------- HTTP 短请求
class HTTPResp(object):
    __slots__ = ("status", "headers", "body", "elapsed_ms", "error")

    def __init__(self, status, headers, body, elapsed_ms, error=""):
        self.status = status
        self.headers = headers
        self.body = body
        self.elapsed_ms = elapsed_ms
        self.error = error

    @property
    def ctype(self):
        return self.headers.get("content-type", "")


def http_get(host, port, path, timeout=HTTP_TIMEOUT):
    """GET 一个 URL，返回 HTTPResp；网络异常不抛，status=0 + error 文本。"""
    url = "http://%s:%d%s" % (_url_host(host), int(port), path)
    request = urllib.request.Request(
        url,
        headers={"User-Agent": USER_AGENT, "Accept": "*/*", "Connection": "close"},
    )
    started = time.time()
    try:
        response = urllib.request.urlopen(request, timeout=timeout)
        try:
            body = response.read()
            status = getattr(response, "status", None) or response.getcode()
            headers = dict((key.lower(), value) for key, value in response.headers.items())
        finally:
            try:
                response.close()
            except Exception:
                pass
        return HTTPResp(int(status), headers, body, (time.time() - started) * 1000.0, "")
    except urllib.error.HTTPError as exc:
        body = b""
        try:
            body = exc.read()
        except Exception:
            pass
        headers = {}
        if exc.headers is not None:
            headers = dict((key.lower(), value) for key, value in exc.headers.items())
        return HTTPResp(int(exc.code), headers, body, (time.time() - started) * 1000.0, "")
    except Exception as exc:
        return HTTPResp(
            0, {}, b"", (time.time() - started) * 1000.0,
            "%s: %s" % (type(exc).__name__, exc),
        )


# ---------------------------------------------------------------- 字节源（raw / chunked）
class RawSource(object):
    """裸 socket 字节流（没有 Transfer-Encoding: chunked 时用）。"""

    def __init__(self, sock):
        self.sock = sock

    def read(self, size=65536):
        return self.sock.recv(max(1, int(size)))


class ChunkedSource(object):
    """HTTP/1.1 chunked 解码器：把分块还原成连续字节流（只拆边框，不改内容）。"""

    def __init__(self, sock, initial=b""):
        self.sock = sock
        self.pending = bytearray(initial)
        self.eof = False

    def _fill(self):
        if self.eof:
            return False
        chunk = self.sock.recv(65536)
        if not chunk:
            self.eof = True
            return False
        self.pending += chunk
        return True

    def _read_line(self):
        while b"\r\n" not in self.pending:
            if not self._fill():
                return None
        position = self.pending.find(b"\r\n")
        line = bytes(self.pending[:position])
        del self.pending[:position + 2]
        return line

    def read(self, size=65536):
        out = bytearray()
        want = max(1, int(size))
        while len(out) < want:
            line = self._read_line()
            if line is None:
                break
            token = line.split(b";", 1)[0].strip()
            if not token:
                continue
            try:
                length = int(token, 16)
            except ValueError:
                self.eof = True
                break
            if length <= 0:
                self.eof = True
                break
            while len(self.pending) < length + 2 and not self.eof:
                self._fill()
            take = min(length, len(self.pending))
            out += bytes(self.pending[:take])
            del self.pending[:take]
            if take < length:
                self.eof = True
                break
            while len(self.pending) < 2 and not self.eof:
                self._fill()
            if bytes(self.pending[:2]) == b"\r\n":
                del self.pending[:2]
        return bytes(out)


# ---------------------------------------------------------------- 流式响应头
def _read_response_head(sock, timeout):
    """从 socket 读 HTTP 响应头，返回 (status, headers, 剩余字节)。"""
    buffer = bytearray()
    deadline = time.time() + max(1.0, float(timeout))
    while b"\r\n\r\n" not in buffer:
        if time.time() > deadline:
            raise socket.timeout("等待 HTTP 响应头超时")
        chunk = sock.recv(8192)
        if not chunk:
            raise ConnectionError("连接在 HTTP 响应头之前就被关闭了")
        buffer += chunk
        if len(buffer) > 256 * 1024:
            raise ValueError("HTTP 响应头异常过大")
    head, _, rest = bytes(buffer).partition(b"\r\n\r\n")
    lines = head.decode("latin-1", "replace").split("\r\n")
    if not lines:
        raise ValueError("响应头为空")
    match = STATUS_RE.match(lines[0].strip())
    if not match:
        raise ValueError("响应首行不是合法 HTTP 状态行: %r" % _short(lines[0]))
    status = int(match.group(1))
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()
    return status, headers, rest


# ---------------------------------------------------------------- multipart 解析
def parse_multipart_stream(source, boundary, deadline, initial=b"", max_frames=0):
    """解析 multipart/x-mixed-replace 流，直到观测窗口结束 / 流关闭 / 收够帧。

    返回 dict：frames（每帧只有长度 / crc32 / JPEG 标记，不保存图像本身）、
    missing_cl、cl_mismatch、bad_jpeg、closed、final_boundary、error。
    """
    delimiter = b"--" + boundary
    buffer = bytearray(initial)
    result = {
        "frames": [], "missing_cl": 0, "cl_mismatch": 0, "bad_jpeg": 0,
        "closed": False, "final_boundary": False, "error": "",
    }

    def fill():
        chunk = source.read(65536)
        if not chunk:
            return False
        buffer.extend(chunk)
        return True

    def take(count):
        while len(buffer) < count:
            if not fill():
                return False
        return True

    def push(body, content_length):
        """记录一帧；顺带用 SOI/EOI 标记算出「真实 JPEG 字节数」来核对 Content-Length。"""
        soi = len(body) >= 2 and body[:2] == JPEG_SOI
        eoi_at = body.rfind(JPEG_EOI) if len(body) >= 2 else -1
        span = (eoi_at + 2) if (soi and eoi_at >= 0) else None
        jpeg_ok = span is not None and span == len(body)
        if not jpeg_ok:
            result["bad_jpeg"] += 1
        if content_length is not None and span is not None and span != content_length:
            result["cl_mismatch"] += 1
        result["frames"].append({
            "index": len(result["frames"]) + 1,
            "len": len(body),
            "cl": content_length,
            "crc": zlib.crc32(body) & 0xFFFFFFFF,
            "soi": soi,
            "eoi": span is not None,
            "jpeg_ok": jpeg_ok,
        })

    def recover_part_body(content_length):
        """Content-Length 不可信时的自适应裁帧。

        声明值太短（服务端把 CL 写小了）→ 截出来的 body 没有 EOI，后面紧跟真实剩余字节，
        此时继续向后扫 EOI，把一帧真正切出来，保证后面帧不错位，同时如实记一次 CL 不符。
        声明值太长（CL 写大了）→ 已在 take() 里失败，走不到这里。
        """
        eoi_at = buffer.find(JPEG_EOI)
        while eoi_at < 0:
            if len(buffer) > MAX_SCAN_BYTES:
                return None
            if not fill():
                return None
            eoi_at = buffer.find(JPEG_EOI)
        end = eoi_at + 2
        if not take(end):
            return None
        return bytes(buffer[:end])

    try:
        while True:
            if max_frames and len(result["frames"]) >= max_frames:
                break
            if time.time() >= deadline:
                break

            # ---- 1) 定位下一个 multipart 边界 ----
            while True:
                position = buffer.find(delimiter)
                if position >= 0:
                    del buffer[:position + len(delimiter)]
                    break
                if len(buffer) > MAX_SCAN_BYTES:
                    result["error"] = "缓冲区超过 %d 字节仍找不到 multipart 边界" % MAX_SCAN_BYTES
                    return result
                if not fill():
                    result["closed"] = True
                    if not result["frames"]:
                        result["error"] = "流在首个 multipart 边界之前就结束了: %r" % _short(
                            bytes(buffer[:120]))
                    return result

            # ---- 2) 是不是结束边界 "--" ----
            if not take(2):
                result["closed"] = True
                break
            if bytes(buffer[0:2]) == b"--":
                result["closed"] = True
                result["final_boundary"] = True
                break

            # ---- 3) 吃掉边界后面的 CRLF ----
            skip = 0
            while skip < len(buffer) and buffer[skip] in (0x0D, 0x0A):
                skip += 1
            if skip:
                del buffer[:skip]

            # ---- 4) part 头（到空行为止） ----
            while True:
                head_end = buffer.find(b"\r\n\r\n")
                if head_end >= 0:
                    break
                if len(buffer) > MAX_SCAN_BYTES:
                    result["error"] = "part 头超过 %d 字节仍未见结束空行" % MAX_SCAN_BYTES
                    return result
                if not fill():
                    result["closed"] = True
                    result["error"] = "part 头没读完流就结束了"
                    return result
            head = bytes(buffer[:head_end])
            del buffer[:head_end + 4]

            match = CONTENT_LENGTH_RE.search(head)
            content_length = int(match.group(1)) if match else None
            if content_length is not None and content_length > MAX_PART_BYTES:
                result["error"] = "part 的 Content-Length=%d 过大（上限 %d）" % (
                    content_length, MAX_PART_BYTES)
                return result

            # ---- 5) part body ----
            if content_length is None:
                # 契约要求每个 part 都带 Content-Length；没有时退回「扫 EOI + 边界」的兜底解析
                result["missing_cl"] += 1
                body = None
                scan_from = 0
                while body is None:
                    eoi_at = buffer.find(JPEG_EOI, scan_from)
                    if eoi_at < 0:
                        if len(buffer) > MAX_SCAN_BYTES:
                            result["error"] = "无 Content-Length 且 %d 字节内找不到 JPEG 结束符" % MAX_SCAN_BYTES
                            return result
                        if not fill():
                            result["closed"] = True
                            result["error"] = "无 Content-Length 且流在 JPEG 结束符之前中断"
                            return result
                        continue
                    end = eoi_at + 2
                    if not take(end + 2):
                        body = bytes(buffer[:end])
                        del buffer[:end]
                        break
                    if bytes(buffer[end:end + 2]) == b"\r\n":
                        body = bytes(buffer[:end])
                        del buffer[:end]
                        break
                    if not take(end + len(delimiter)):
                        body = bytes(buffer[:end])
                        del buffer[:end]
                        break
                    if bytes(buffer[end:end + len(delimiter)]) == delimiter:
                        body = bytes(buffer[:end])
                        del buffer[:end]
                        break
                    scan_from = end
                push(body, None)
            else:
                if not take(content_length):
                    result["closed"] = True
                    result["error"] = "part 声明 Content-Length=%d，但流提前结束了" % content_length
                    return result
                body = bytes(buffer[:content_length])
                del buffer[:content_length]
                # Content-Length 被写小时，按声明长度截出来的 body 尾部没有 EOI：
                # 继续向后找到真实 EOI 补全这一帧，既保证后续帧不错位，又如实记一次 CL 不符
                if len(body) >= 2 and body[:2] == JPEG_SOI and body[-2:] != JPEG_EOI:
                    extra = recover_part_body(content_length)
                    if extra is not None:
                        body = body + extra
                        del buffer[:len(extra)]
                push(body, content_length)
    except socket.timeout:
        if time.time() >= deadline - 0.5:
            pass  # 观测窗口到点的正常收尾
        else:
            result["error"] = result["error"] or "读流超时：超过 socket 超时时间没有新数据（流疑似停止）"
    except Exception as exc:
        result["error"] = result["error"] or "%s: %s" % (type(exc).__name__, exc)
    return result


# ---------------------------------------------------------------- 并发探测
def _probe_worker(cfg, info):
    """拉流进行中，另开一条连接请求 /api/stats，验证服务端没被一条流堵死。"""
    result = {
        "status": 0, "elapsed_ms": -1.0, "json_ok": False, "dict_ok": False,
        "error": "", "frames_before": 0, "frames_after": 0, "missing_keys": [],
    }
    try:
        time.sleep(max(0.0, cfg.probe_delay))
        result["frames_before"] = len(info.get("frames") or [])
        response = http_get(cfg.host, cfg.port, cfg.stats_path, cfg.probe_timeout)
        result["status"] = response.status
        result["elapsed_ms"] = response.elapsed_ms
        result["error"] = response.error
        if response.status == 200 and response.body:
            try:
                payload = json.loads(response.body.decode("utf-8", "replace"))
                result["json_ok"] = True
                result["dict_ok"] = isinstance(payload, dict)
                if isinstance(payload, dict):
                    result["missing_keys"] = [
                        key for key in REQUIRED_STATS_KEYS if key not in payload
                    ]
            except Exception as exc:
                result["error"] = "JSON 解析失败: %s" % exc
    except Exception as exc:
        result["error"] = "%s: %s" % (type(exc).__name__, exc)
    result["frames_after"] = len(info.get("frames") or [])
    info["concurrent"] = result


def _recv_timeout(cfg):
    if cfg.stream_timeout and cfg.stream_timeout > 0:
        return float(cfg.stream_timeout)
    # 至少 2s，且覆盖 --min-fps 对应帧间隔的 3 倍，避免把慢流误判成停止
    return max(2.0, min(15.0, 3.0 / max(float(cfg.min_fps), 0.1)))


# ---------------------------------------------------------------- 一次拉流会话（4/5/6 共用）
def run_stream_session(cfg):
    info = {
        "status": 0, "boundary": "", "content_type": "", "frames": [], "elapsed": 0.0,
        "bytes": 0, "missing_cl": 0, "cl_mismatch": 0, "bad_jpeg": 0,
        "closed": False, "closed_early": False, "final_boundary": False,
        "chunked": False, "error": "", "connect_ms": 0.0, "concurrent": None,
    }
    sock = None
    probe_thread = None
    try:
        started = time.time()
        sock = socket.create_connection(
            (_socket_host(cfg.host), int(cfg.port)), timeout=cfg.connect_timeout)
        sock.settimeout(_recv_timeout(cfg))
        info["connect_ms"] = (time.time() - started) * 1000.0

        request = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Accept: multipart/x-mixed-replace, */*\r\n"
            "\r\n"
        ) % (cfg.stream_path, _host_header(cfg.host, cfg.port), USER_AGENT)
        sock.sendall(request.encode("latin-1", "replace"))

        status, headers, rest = _read_response_head(sock, cfg.http_timeout)
        info["status"] = status
        info["content_type"] = headers.get("content-type", "")
        info["chunked"] = "chunked" in headers.get("transfer-encoding", "").lower()
        match = BOUNDARY_RE.search(info["content_type"])
        info["boundary"] = match.group(1) if match else ""

        if status != 200:
            info["error"] = "GET %s 返回 HTTP %s（body 开头: %r）" % (
                cfg.stream_path, status, _short(rest[:120]))
            return info
        if not info["boundary"]:
            info["error"] = "Content-Type 里取不到 multipart boundary: %r" % _short(info["content_type"])
            return info

        if info["chunked"]:
            # rest 是响应头之后「尚未解码」的 chunked 原始字节，必须交给解码器当预置缓冲
            source = ChunkedSource(sock, initial=rest)
            initial = b""
        else:
            source = RawSource(sock)
            initial = rest

        probe_thread = threading.Thread(
            target=_probe_worker, args=(cfg, info), name="stats-probe", daemon=True)
        probe_thread.start()

        begin = time.time()
        deadline = begin + cfg.seconds
        parsed = parse_multipart_stream(
            source, info["boundary"].encode("latin-1", "replace"),
            deadline, initial=initial, max_frames=cfg.max_frames)
        info["elapsed"] = max(0.0, time.time() - begin)
        info["frames"] = parsed["frames"]
        info["missing_cl"] = parsed["missing_cl"]
        info["cl_mismatch"] = parsed["cl_mismatch"]
        info["bad_jpeg"] = parsed["bad_jpeg"]
        info["closed"] = parsed["closed"]
        info["final_boundary"] = parsed["final_boundary"]
        info["bytes"] = sum(frame["len"] for frame in info["frames"])
        if parsed["error"] and not info["error"]:
            info["error"] = parsed["error"]
        info["closed_early"] = bool(info["closed"]) and info["elapsed"] < max(0.1, cfg.seconds - 0.2)
    except socket.timeout:
        info["error"] = info["error"] or "连接 / 响应头超时（%.1fs）" % cfg.http_timeout
    except Exception as exc:
        info["error"] = info["error"] or "%s: %s" % (type(exc).__name__, exc)
    finally:
        if probe_thread is not None:
            probe_thread.join(timeout=cfg.probe_delay + cfg.probe_timeout + 5.0)
            if probe_thread.is_alive():
                info["concurrent"] = {
                    "status": 0, "elapsed_ms": -1.0, "json_ok": False, "dict_ok": False,
                    "error": "并发探测线程没有在预期时间内结束", "frames_before": 0,
                    "frames_after": 0, "missing_keys": [],
                }
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass
    return info


# ---------------------------------------------------------------- 检查 1：页面外壳
def check_page(cfg):
    problems = []
    response = http_get(cfg.host, cfg.port, cfg.page_path, cfg.http_timeout)
    body = response.body or b""
    ctype = response.ctype
    lowered = body.lower()
    has_html = b"<html" in lowered
    stats_token = cfg.stats_path.encode("latin-1", "replace")
    has_stats_hook = stats_token in body

    if response.error:
        problems.append("请求失败: " + response.error)
    if response.status != 200:
        problems.append("HTTP %s != 200" % response.status)
    if "text/html" not in ctype.lower():
        problems.append("Content-Type=%r 不含 text/html" % ctype)
    if len(body) <= cfg.min_page_bytes:
        problems.append("body 只有 %d 字节（阈值 > %d）" % (len(body), cfg.min_page_bytes))
    if not has_html:
        problems.append("body 里找不到 <html（不是完整网页）")
    if not has_stats_hook:
        problems.append("body 里找不到 %s（页面没去拉真实数据）" % cfg.stats_path)

    detail = "HTTP %s, Content-Type=%r, %d 字节, 含<html>=%s, 含%s=%s, 耗时 %.1f ms" % (
        response.status, ctype, len(body), has_html, cfg.stats_path,
        has_stats_hook, response.elapsed_ms)
    return {"ok": not problems, "detail": detail, "reason": "; ".join(problems)}


# ---------------------------------------------------------------- 检查 2：/api/stats 契约
def check_stats(cfg):
    problems = []
    first = http_get(cfg.host, cfg.port, cfg.stats_path, cfg.http_timeout)
    time.sleep(max(0.0, cfg.stats_gap))
    second = http_get(cfg.host, cfg.port, cfg.stats_path, cfg.http_timeout)

    parsed = {"首次": None, "二次": None}
    for label, response in (("首次", first), ("二次", second)):
        if response.error:
            problems.append("%s请求失败: %s" % (label, response.error))
            continue
        if response.status != 200:
            problems.append("%s HTTP %s != 200" % (label, response.status))
            continue
        try:
            payload = json.loads(response.body.decode("utf-8", "replace"))
        except Exception as exc:
            problems.append("%s JSON 解析失败(%s): %r" % (label, exc, _short(response.body[:120])))
            continue
        if not isinstance(payload, dict):
            problems.append("%s JSON 顶层不是 object（实际 %s）" % (label, type(payload).__name__))
            continue
        parsed[label] = payload

    stats = parsed["二次"] or parsed["首次"] or {}
    missing = [key for key in REQUIRED_STATS_KEYS if key not in stats]
    if missing:
        problems.append("缺少字段: %s" % ",".join(missing))

    payload_dets = stats.get("dets")
    dets_count = len(payload_dets) if isinstance(payload_dets, list) else -1
    if not isinstance(payload_dets, list):
        problems.append("dets 必须是 list（实际 %s）" % type(payload_dets).__name__)
    else:
        for position, item in enumerate(payload_dets):
            if not isinstance(item, dict):
                problems.append("dets[%d] 不是 object（实际 %s）" % (position, type(item).__name__))
                break
        if dets_count > 12:
            problems.append("dets 有 %d 条，超过契约上限 12 条" % dets_count)

    def number(key):
        value = stats.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return None
        return float(value)

    fps = number("pipeline_fps")
    if fps is None:
        problems.append("pipeline_fps 缺失或不是数值")
    elif not (0.0 < fps <= 60.0):
        problems.append("pipeline_fps=%s 不在 (0, 60]" % _fmt(fps, "%.3f"))

    npu_ms = number("npu_ms")
    if npu_ms is None:
        problems.append("npu_ms 缺失或不是数值")
    elif not (0.0 < npu_ms < 200.0):
        problems.append("npu_ms=%s 不在 (0, 200)" % _fmt(npu_ms, "%.3f"))

    total_ms = number("total_ms")
    if total_ms is None:
        problems.append("total_ms 缺失或不是数值")
    elif not (0.0 < total_ms < 1000.0):
        problems.append("total_ms=%s 不在 (0, 1000)" % _fmt(total_ms, "%.3f"))

    objects_total = number("objects_total")
    objects_3d = number("objects_3d")
    if objects_total is None:
        problems.append("objects_total 缺失或不是数值")
    if objects_3d is None:
        problems.append("objects_3d 缺失或不是数值")
    if objects_total is not None and objects_3d is not None and objects_3d > objects_total:
        problems.append("objects_3d(%s) > objects_total(%s)，数据逻辑不成立" % (
            _fmt(objects_3d, "%.0f"), _fmt(objects_total, "%.0f")))

    frame_first = None
    frame_second = None
    for label, container in (("首次", parsed["首次"]), ("二次", parsed["二次"])):
        value = (container or {}).get("frame_id")
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            continue
        if label == "首次":
            frame_first = float(value)
        else:
            frame_second = float(value)
    if frame_first is None:
        problems.append("首次 frame_id 缺失或不是数值")
    if frame_second is None:
        problems.append("二次 frame_id 缺失或不是数值")
    frame_delta = None
    if frame_first is not None and frame_second is not None:
        frame_delta = frame_second - frame_first
        if frame_second < frame_first:
            problems.append("frame_id 倒退: %s -> %s（间隔 %.1fs）" % (
                _fmt(frame_first, "%.0f"), _fmt(frame_second, "%.0f"), cfg.stats_gap))

    resolution = stats.get("resolution")
    if isinstance(resolution, bool) or not isinstance(resolution, (str,)):
        if "resolution" in stats and resolution is not None:
            problems.append("resolution 不是字符串（实际 %s）" % type(resolution).__name__)

    detail = (
        "frame_id %s -> %s (Δ%s), pipeline_fps=%s, npu_ms=%s, total_ms=%s, "
        "objects_3d/objects_total=%s/%s, resolution=%r, dets=%s 条"
    ) % (
        _fmt(frame_first, "%.0f"), _fmt(frame_second, "%.0f"),
        "-" if frame_delta is None else ("%+.0f" % frame_delta),
        _fmt(fps), _fmt(npu_ms), _fmt(total_ms),
        _fmt(objects_3d, "%.0f"), _fmt(objects_total, "%.0f"),
        resolution, dets_count if dets_count >= 0 else "非list",
    )
    return {"ok": not problems, "detail": detail, "reason": "; ".join(problems),
            "stats": stats}


# ---------------------------------------------------------------- 检查 3：/snapshot 单帧
def check_snapshot(cfg, stats):
    problems = []
    response = http_get(cfg.host, cfg.port, cfg.snapshot_path, cfg.http_timeout)
    body = response.body or b""
    size = len(body)
    ctype = response.ctype
    is_soi = body[:2] == JPEG_SOI
    is_eoi = body[-2:] == JPEG_EOI if size >= 2 else False

    if response.error:
        problems.append("请求失败: " + response.error)
    if response.status != 200:
        problems.append("HTTP %s != 200" % response.status)
    if "image/jpeg" not in ctype.lower():
        problems.append("Content-Type=%r 不含 image/jpeg" % ctype)
    if not is_soi:
        problems.append("body 未以 JPEG SOI(FFD8) 开头（前 8 字节: %s）" % body[:8].hex())
    if not is_eoi:
        problems.append("body 未以 JPEG EOI(FFD9) 结尾（后 8 字节: %s）" % body[-8:].hex())
    if size <= cfg.min_snapshot_bytes:
        problems.append("长度 %d 字节 <= 阈值 %d" % (size, cfg.min_snapshot_bytes))

    reference = None
    if isinstance(stats, dict):
        value = stats.get("jpeg_bytes")
        if isinstance(value, (int, float)) and not isinstance(value, bool) and value > 0:
            reference = float(value)
    ratio = None
    if reference is None:
        ratio_note = "无可用 stats.jpeg_bytes 参照，跳过数量级比对"
    else:
        ratio = size / reference
        low, high = 1.0 - cfg.jpeg_tolerance, 1.0 + cfg.jpeg_tolerance
        if not (low <= ratio <= high):
            problems.append("与 stats.jpeg_bytes=%d 比值 %.2f 超出 ±%d%%" % (
                int(reference), ratio, int(round(cfg.jpeg_tolerance * 100))))
        ratio_note = "stats.jpeg_bytes=%d, 比值=%.2f" % (int(reference), ratio)

    detail = "HTTP %s, Content-Type=%r, %d 字节, SOI=%s, EOI=%s, %s" % (
        response.status, ctype, size, is_soi, is_eoi, ratio_note)
    return {"ok": not problems, "detail": detail, "reason": "; ".join(problems)}


# ---------------------------------------------------------------- 检查 4：/stream
def check_stream(cfg, info):
    frames = info.get("frames") or []
    count = len(frames)
    elapsed = float(info.get("elapsed") or 0.0)
    fps = (count / elapsed) if elapsed > 0 else 0.0
    problems = []

    if info.get("error"):
        problems.append(info["error"])
    if info.get("status") != 200:
        problems.append("HTTP %s != 200" % info.get("status"))
    if not info.get("boundary"):
        problems.append("Content-Type 里取不到 multipart boundary")
    if count < 2:
        problems.append("完整 JPEG 帧数 %d < 2（流不算活的）" % count)
    if fps < cfg.min_fps:
        problems.append("实测 %.2f fps < --min-fps %.2f" % (fps, cfg.min_fps))
    if info.get("missing_cl"):
        problems.append("%d 帧缺 Content-Length" % info["missing_cl"])
    if info.get("cl_mismatch"):
        problems.append("%d 帧 Content-Length 与实际 JPEG 字节数不符" % info["cl_mismatch"])
    if info.get("bad_jpeg"):
        problems.append("%d 帧不是完整 JPEG（SOI/EOI 标记不对）" % info["bad_jpeg"])
    if info.get("closed_early"):
        problems.append("服务端在观测窗口结束前关闭了流（只持续 %.2fs / %.2fs）" % (
            elapsed, cfg.seconds))

    detail = (
        "HTTP %s, boundary=%r, 帧数=%d, 实测=%.2f fps (窗口 %.2fs / 目标 %.1fs), "
        "共 %d 字节, 缺CL=%d, CL不符=%d, 非法JPEG=%d, chunked=%s"
    ) % (
        info.get("status"), info.get("boundary"), count, fps, elapsed, cfg.seconds,
        int(info.get("bytes") or 0), info.get("missing_cl"), info.get("cl_mismatch"),
        info.get("bad_jpeg"), "是" if info.get("chunked") else "否",
    )
    return {"ok": not problems, "detail": detail, "reason": "; ".join(problems)}


# ---------------------------------------------------------------- 检查 5：并发可用性
def check_concurrency(cfg, info):
    probe = info.get("concurrent")
    if not probe:
        return {
            "ok": False,
            "detail": "无并发探测结果（拉流会话没跑起来）",
            "reason": "拉流期间没能并发请求 %s" % cfg.stats_path,
        }
    problems = []
    status = probe.get("status")
    elapsed_ms = float(probe.get("elapsed_ms", -1.0))
    if status != 200:
        problems.append("并发 %s 返回 %s（%s）" % (
            cfg.stats_path, status, probe.get("error") or "无响应"))
    if not probe.get("json_ok"):
        problems.append("并发 %s 不是合法 JSON（%s）" % (
            cfg.stats_path, probe.get("error") or "解析失败"))
    if elapsed_ms < 0 or elapsed_ms >= cfg.concurrent_limit_ms:
        problems.append("并发耗时 %s ms >= 上限 %.0f ms（服务端可能被一条流堵死）" % (
            _fmt(elapsed_ms, "%.1f"), cfg.concurrent_limit_ms))
    missing = probe.get("missing_keys") or []
    detail = (
        "拉流中并发 GET %s: HTTP %s, 耗时 %s ms (上限 %.0f ms), JSON合法=%s, "
        "探测窗口内帧数 %d->%d, 缺键=%s"
    ) % (
        cfg.stats_path, status, _fmt(elapsed_ms, "%.1f"), cfg.concurrent_limit_ms,
        "是" if probe.get("json_ok") else "否",
        int(probe.get("frames_before") or 0), int(probe.get("frames_after") or 0),
        ",".join(missing) if missing else "无",
    )
    return {"ok": not problems, "detail": detail, "reason": "; ".join(problems)}


# ---------------------------------------------------------------- 检查 6：帧内容变化性
def check_variability(cfg, info):
    frames = info.get("frames") or []
    if len(frames) < 3:
        return {
            "ok": False,
            "detail": "完整帧数=%d, 可用 crc32 样本不足" % len(frames),
            "reason": "帧数不足 3 帧（实际 %d），无法判断帧内容是否在变化" % len(frames),
        }
    crcs = [frame["crc"] for frame in frames]
    distinct = len(set(crcs))
    lengths = [frame["len"] for frame in frames]
    first = frames[0]
    middle = frames[len(frames) // 2]
    last = frames[-1]
    problems = []
    if distinct < 3:
        problems.append("互不相同的 crc32 只有 %d 个（< 3），疑似静态图冒充视频" % distinct)

    detail = (
        "帧数=%d, 不同crc32=%d, 首帧=%d字节(crc=%08x), 中间帧=%d字节(crc=%08x), "
        "末帧=%d字节(crc=%08x), 长度范围=[%d, %d], 首末长度%s"
    ) % (
        len(frames), distinct, first["len"], first["crc"], middle["len"], middle["crc"],
        last["len"], last["crc"], min(lengths), max(lengths),
        "相同" if first["len"] == last["len"] else "不同",
    )
    return {"ok": not problems, "detail": detail, "reason": "; ".join(problems)}


# ---------------------------------------------------------------- 结果打印 / 汇总
def _run_check(index, name, function, *args):
    try:
        outcome = function(*args)
        if not isinstance(outcome, dict):
            outcome = {"ok": False, "detail": "检查函数返回了非法结果", "reason": "内部错误"}
    except Exception as exc:  # 任何异常都算 FAIL，绝不抛栈崩掉
        outcome = {"ok": False, "detail": "执行异常", "reason": "%s: %s" % (type(exc).__name__, exc)}

    ok = bool(outcome.get("ok"))
    detail = str(outcome.get("detail", "") or "")
    reason = str(outcome.get("reason", "") or "")
    RESULTS.append({"index": index, "name": name, "ok": ok, "detail": detail, "reason": reason})
    if ok:
        print("[PASS] %d. %s  实测: %s" % (index, name, detail))
    else:
        print("[FAIL] %d. %s  实测: %s  原因: %s" % (index, name, detail, reason))
    sys.stdout.flush()
    return outcome


def build_parser():
    parser = argparse.ArgumentParser(
        description="EdgeVision RK3588 BEV 看板端到端验收（仅 Python3 标准库）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default=os.environ.get("EDGEVISION_BOARD_HOST") or DEFAULT_HOST,
                        help="板子 IP / 主机名（默认取环境变量 EDGEVISION_BOARD_HOST，没有则 127.0.0.1）")
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("EDGEVISION_BOARD_PORT") or DEFAULT_PORT),
                        help="看板 HTTP 端口")
    parser.add_argument("--seconds", type=float, default=DEFAULT_SECONDS,
                        help="拉流观测窗口（秒）")
    parser.add_argument("--min-fps", type=float, default=DEFAULT_MIN_FPS,
                        help="实测 fps 下限，低于即 FAIL")
    parser.add_argument("--connect-timeout", type=float, default=CONNECT_TIMEOUT,
                        help="TCP 连接超时（秒）")
    parser.add_argument("--timeout", dest="http_timeout", type=float, default=HTTP_TIMEOUT,
                        help="普通短请求超时（秒）")
    parser.add_argument("--probe-timeout", type=float, default=PROBE_TIMEOUT,
                        help="并发探测请求超时（秒）")
    parser.add_argument("--stream-timeout", type=float, default=0.0,
                        help="读流单次 recv 超时（秒；0 = 按 --min-fps 自动推导）")
    parser.add_argument("--max-frames", type=int, default=MAX_FRAMES,
                        help="最多接收的帧数（防止异常高帧率占满内存）")
    parser.add_argument("--stats-gap", type=float, default=STATS_GAP,
                        help="采两次 /api/stats 的间隔（秒），用于判断 frame_id 递增")
    parser.add_argument("--probe-delay", type=float, default=PROBE_DELAY,
                        help="拉流开始多少秒后发起并发探测")
    parser.add_argument("--concurrent-limit-ms", type=float, default=DEFAULT_CONCURRENT_LIMIT_MS,
                        help="并发探测耗时上限（毫秒）")
    parser.add_argument("--page-path", default="/", help="页面外壳路径")
    parser.add_argument("--stats-path", default="/api/stats", help="指标接口路径")
    parser.add_argument("--snapshot-path", default="/snapshot", help="单帧快照路径")
    parser.add_argument("--stream-path", default="/stream", help="MJPEG 推流路径")
    parser.add_argument("--min-page-bytes", type=int, default=500,
                        help="页面 body 最小字节数")
    parser.add_argument("--min-snapshot-bytes", type=int, default=5000,
                        help="snapshot 最小字节数")
    parser.add_argument("--jpeg-tolerance", type=float, default=0.5,
                        help="snapshot 与 stats.jpeg_bytes 允许的相对偏差（0.5 = ±50%%）")
    return parser


def main(argv=None):
    _configure_stdout()
    cfg = build_parser().parse_args(argv)

    print("=" * 78)
    print("EdgeVision RK3588 BEV 看板端到端验收  verify_bev_dashboard.py")
    print("目标: http://%s:%d   页面=%s  stats=%s  snapshot=%s  stream=%s" % (
        _url_host(cfg.host), cfg.port, cfg.page_path, cfg.stats_path,
        cfg.snapshot_path, cfg.stream_path))
    print("参数: --seconds %.2f  --min-fps %.2f  --connect-timeout %.1f  --timeout %.1f" % (
        cfg.seconds, cfg.min_fps, cfg.connect_timeout, cfg.http_timeout))
    print("=" * 78)

    page_outcome = _run_check(1, "页面外壳 GET %s" % cfg.page_path, check_page, cfg)
    stats_outcome = _run_check(2, "指标接口 GET %s" % cfg.stats_path, check_stats, cfg)
    _run_check(3, "单帧快照 GET %s" % cfg.snapshot_path, check_snapshot, cfg,
               stats_outcome.get("stats"))

    info = run_stream_session(cfg)
    _run_check(4, "实时流 GET %s (连读 %.1fs)" % (cfg.stream_path, cfg.seconds),
               check_stream, cfg, info)
    _run_check(5, "并发可用性 (拉流中并发 GET %s)" % cfg.stats_path,
               check_concurrency, cfg, info)
    _run_check(6, "帧内容变化性 (crc32 防静态图)", check_variability, cfg, info)

    total = len(RESULTS)
    passed = sum(1 for item in RESULTS if item["ok"])
    print("=" * 78)
    print("汇总: %d/%d 通过" % (passed, total))
    for item in RESULTS:
        print("  %s %d. %s" % ("[PASS]" if item["ok"] else "[FAIL]", item["index"], item["name"]))
    print("-" * 78)
    if passed == total:
        print("结果: 全部通过（看板端到端验收 OK）")
        return 0
    print("结果: %d 项失败，请按上面 [FAIL] 的『原因』逐条排查" % (total - passed))
    if not page_outcome.get("ok") and info.get("status") == 0:
        print("提示: 连不上 http://%s:%d —— 确认板端看板程序已启动、端口正确、PC 与板子同网段。"
              % (_url_host(cfg.host), cfg.port))
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[FAIL] 被用户中断")
        sys.exit(1)
    except Exception as exc:  # 兜底：绝不抛栈崩掉
        print("[FAIL] 脚本异常: %s: %s" % (type(exc).__name__, exc))
        sys.exit(1)
