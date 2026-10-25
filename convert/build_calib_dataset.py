# ==============================================================================
# EdgeVision-RK3588: 校准集重建（修复 INT8 量化掉点）
# ------------------------------------------------------------------------------
# 问题诊断（2026-10-21）：
#   旧校准集 data/calib/*.jpg 是从「合成色块动画视频」抽的帧 —— 纯色矩形 + 纯灰背景，
#   真实激活值分布极窄。用它做 PTQ 标定，一遇到真实摄像头画面（宿舍/实验室，
#   有纹理、有噪声、有高光）就会大量溢出/截断 → 量化掉点。
#
# 修复方案：
#   1. 主体用板端双摄实拍的真实场景图（data/calib_real/，50 张）
#   2. 每张做光照/对比度/伽马增强，扩大激活值覆盖（模拟不同曝光）→ 约 150 张
#   3. 保留少量合成图（约 20 张），避免原合成场景退化
#   4. 全部 resize 到 640x640、uint8 JPG（与部署时预处理一致）
#
# 用法（PC 侧）：
#   python convert/build_calib_dataset.py
#   python convert/build_calib_dataset.py --path-prefix /home/yankai/edgevision_calib
#   （toolkit 在 WSL 里跑时，必须给 WSL 内可见的路径前缀，中文路径建议先拷进 WSL 家目录）
# ==============================================================================
import os
import sys
import glob
import random
import argparse
import numpy as np
from PIL import Image, ImageEnhance

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
REAL_DIR = os.path.join(PROJECT_ROOT, 'data', 'calib_real')
SYNTH_DIR = os.path.join(PROJECT_ROOT, 'data', 'calib')
OUT_DIR = os.path.join(PROJECT_ROOT, 'data', 'calib_v2')
DATASET_TXT = os.path.join(OUT_DIR, 'dataset.txt')

IMGSZ = 640
SEED = 2026


def load_and_square(path, size=IMGSZ):
    """等比缩放 + 居中填充（letterbox），与部署侧预处理保持一致。"""
    img = Image.open(path).convert('RGB')
    w, h = img.size
    s = size / max(w, h)
    nw, nh = int(round(w * s)), int(round(h * s))
    img = img.resize((nw, nh), Image.BILINEAR)
    canvas = Image.new('RGB', (size, size), (114, 114, 114))   # YOLO 标准灰边
    canvas.paste(img, ((size - nw) // 2, (size - nh) // 2))
    return canvas


def augment(img, kind, rng):
    """光照/对比度/伽马扰动，覆盖真实世界曝光差异。"""
    if kind == 'bright':
        return ImageEnhance.Brightness(img).enhance(1.0 + 0.35 * rng.random())
    if kind == 'dark':
        return ImageEnhance.Brightness(img).enhance(0.55 + 0.15 * rng.random())
    if kind == 'contrast':
        return ImageEnhance.Contrast(img).enhance(1.25 + 0.45 * rng.random())
    if kind == 'gamma':
        g = rng.choice([0.65, 0.8, 1.3, 1.6])
        arr = np.asarray(img, dtype=np.float32) / 255.0
        arr = np.clip((arr ** g) * 255.0, 0, 255).astype(np.uint8)
        return Image.fromarray(arr)
    if kind == 'noise':
        arr = np.asarray(img, dtype=np.int16)
        sigma = rng.uniform(4, 10)
        noise = rng.normal(0, sigma, arr.shape).astype(np.int16)
        return Image.fromarray(np.clip(arr + noise, 0, 255).astype(np.uint8))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--path-prefix', default=None,
                    help="dataset.txt 里使用的路径前缀（如 WSL 内的 /home/yankai/edgevision_calib）")
    ap.add_argument('--aug-per-image', type=int, default=2, help="每张真实图生成的增强变体数")
    ap.add_argument('--synth-keep', type=int, default=20, help="保留的合成图数量")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    for f in glob.glob(os.path.join(OUT_DIR, '*.jpg')):
        os.remove(f)

    rng = random.Random(SEED)
    nprng = np.random.default_rng(SEED)
    written = []

    real = sorted(glob.glob(os.path.join(REAL_DIR, '*.jpg')))
    if not real:
        print(f"[-] 没有找到真实场景图: {REAL_DIR}")
        print("    先在板子上跑 _tools/collect_calib.sh，再 tar 回来解压到 data/calib_real/")
        sys.exit(1)
    print(f"[*] 真实图 {len(real)} 张 -> 生成原图 + 每张 {args.aug_per_image} 个增强变体")

    kinds = ['bright', 'dark', 'contrast', 'gamma', 'noise']
    idx = 0
    for p in real:
        base = load_and_square(p)
        out = os.path.join(OUT_DIR, f'real_{idx:03d}.jpg')
        base.save(out, quality=92, subsampling=0)
        written.append(out)
        idx += 1
        for _ in range(args.aug_per_image):
            k = rng.choice(kinds)
            aug = augment(base, k, nprng)
            out = os.path.join(OUT_DIR, f'real_{idx:03d}.jpg')
            aug.save(out, quality=92, subsampling=0)
            written.append(out)
            idx += 1

    synth = sorted(glob.glob(os.path.join(SYNTH_DIR, '*.jpg')))
    if synth and args.synth_keep > 0:
        step = max(1, len(synth) // args.synth_keep)
        picked = synth[::step][:args.synth_keep]
        print(f"[*] 合成图保留 {len(picked)} 张（原 {len(synth)} 张里等距抽样）")
        for p in picked:
            load_and_square(p).save(os.path.join(OUT_DIR, f'synth_{idx:03d}.jpg'), quality=92, subsampling=0)
            written.append(os.path.join(OUT_DIR, f'synth_{idx:03d}.jpg'))
            idx += 1

    # dataset.txt：路径前缀决定 toolkit 能不能读到
    lines = []
    for p in written:
        if args.path_prefix:
            lines.append(os.path.join(args.path_prefix, os.path.basename(p)))
        else:
            lines.append(os.path.abspath(p).replace('\\', '/'))
    with open(DATASET_TXT, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    print(f"[+] 生成 {len(written)} 张 640x640 校准图 -> {OUT_DIR}")
    print(f"[+] dataset.txt -> {DATASET_TXT}")
    print(f"    首行示例: {lines[0]}")
    if not args.path_prefix:
        print("[!] 未指定 --path-prefix。若 toolkit 在 WSL/Linux 里跑，请用 WSL 内可见路径重新生成，")
        print("    或把整个 calib_v2 目录拷进 WSL 后加 --path-prefix 重跑。")


if __name__ == '__main__':
    main()
