#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
calib_io.py —— 标定文件读写的小兼容层（只解决一个具体的坑）

坑：**OpenCV 的 cv2.FileStorage 在 Windows 上不支持中文路径。**
  cv2.imread("D:\\a研究生\\项目\\rk3588项目\\...\\left_000.png")   → 正常
  cv2.FileStorage("D:\\a研究生\\项目\\rk3588项目\\...\\calib.yml", WRITE)
      → [ERROR persistence.cpp:584] Can't open file: '...' in write mode
  报错看上去像「权限不够/目录不存在」，实际是路径里带非 ASCII 字符，
  OpenCV 内部用 fopen(char*) 走窄字符 API，Windows 不会自动转 UTF-16。

解法：先落到系统临时目录（一定是纯 ASCII 路径）读写，再 Python 的 shutil 搬回去。
shutil 走的是 Python 的宽字符 API，中文路径对它没影响。

用法（和 cv2.FileStorage 用法一样，只是套一层 with）：
    with file_storage_write("D:/项目/stereo_calib.yml") as fs:
        fs.write("rms", 0.0439)
    # 退出 with 时自动 release 并把文件搬到目标位置

    with file_storage_read("D:/项目/stereo_calib.yml") as fs:
        rms = fs.getNode("rms").real()
"""
import contextlib
import os
import shutil
import tempfile
from pathlib import Path

import cv2


def is_ascii_path(path):
    """路径（含盘符、目录、文件名）全是 ASCII 吗？"""
    try:
        str(path).encode("ascii")
        return True
    except UnicodeEncodeError:
        return False


def _make_temp(suffix):
    """在系统临时目录里占一个空文件名（纯 ASCII）并返回路径。"""
    fd, tmp = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    return tmp


@contextlib.contextmanager
def file_storage_write(path):
    """
    写 FileStorage（YAML/XML/JSON）。
    非 ASCII 路径 → 先写临时文件，退出时搬到目标位置。
    """
    path = Path(path).expanduser().resolve()
    actual = str(path)
    tmp = None
    if not is_ascii_path(actual):
        tmp = _make_temp(path.suffix or ".yml")
        actual = tmp

    fs = cv2.FileStorage(actual, cv2.FILE_STORAGE_WRITE)
    if not fs.isOpened():
        if tmp:
            with contextlib.suppress(OSError):
                os.unlink(tmp)
        raise OSError(f"FileStorage 打不开（写入）: {path}")

    ok = False
    try:
        yield fs
        ok = True
    finally:
        fs.release()
        if tmp:
            if ok:
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(tmp, str(path))
            else:
                with contextlib.suppress(OSError):
                    os.unlink(tmp)


@contextlib.contextmanager
def file_storage_read(path):
    """读 FileStorage。非 ASCII 路径 → 先复制到临时文件再读。"""
    path = Path(path).expanduser().resolve()
    if not path.exists():
        raise FileNotFoundError(f"文件不存在: {path}")

    actual = str(path)
    tmp = None
    if not is_ascii_path(actual):
        tmp = _make_temp(path.suffix or ".yml")
        shutil.copyfile(str(path), tmp)
        actual = tmp

    fs = cv2.FileStorage(actual, cv2.FILE_STORAGE_READ)
    if not fs.isOpened():
        if tmp:
            with contextlib.suppress(OSError):
                os.unlink(tmp)
        raise OSError(f"FileStorage 打不开（读取）: {path}")
    try:
        yield fs
    finally:
        fs.release()
        if tmp:
            with contextlib.suppress(OSError):
                os.unlink(tmp)


def read_yml_dict(path):
    """
    把整个 yml 读成一个 dict（标量→float/str，矩阵→numpy 数组，其余→字符串）。
    主要给自测脚本核对用；板端 C++ 是按键名逐个读，没这需求。
    """
    import numpy as np

    out = {}
    with file_storage_read(path) as fs:
        keys = fs.root().keys()
        for k in keys:
            node = fs.getNode(k)
            if node.isNone():
                out[k] = None
            elif node.isReal():
                out[k] = float(node.real())
            elif node.isInt():
                out[k] = int(node.int_value())
            elif node.isString():
                out[k] = node.string()
            elif node.isMap():
                m = node.mat()
                out[k] = np.asarray(m, dtype=np.float64) if m is not None else None
            else:
                m = node.mat()
                out[k] = np.asarray(m, dtype=np.float64) if m is not None else None
    return out
