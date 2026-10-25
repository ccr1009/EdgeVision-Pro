# ==============================================================================
# EdgeVision-RK3588: mAP@0.5 评估（COCO128，128 张带标注）
# ------------------------------------------------------------------------------
# 为什么要有这个脚本：
#   简历里"500 张实景验证集 mAP 掉点 <0.8%"在项目里查无实据（无标注、无 mAP 代码），
#   而"9 张 top-1 一致性"样本量太小且是分类指标，不能代表检测精度。
#   这里用公开带标注的 COCO128 做**真 mAP@0.5**，可复现、可现场演示。
#
# 两种用法（口径必须一致，否则对比无意义）：
#   1) PC 端 FP32 基准：
#        python convert/eval_map.py --model data/models/yolov8n_dflfree.onnx
#   2) 板端 INT8（先在板上导出预测 txt，再用同一份 GT 算）：
#        python convert/eval_map.py --preds int8_preds.txt
#
# 统一口径：
#   letterbox(640, 灰边114) 预处理；conf=0.01（mAP 要保留低分预测）；NMS IoU=0.45；
#   坐标还原到原图绝对像素；AP 用 COCO 101 点插值。
# ==============================================================================
import os
import sys
import glob
import argparse
import numpy as np
from PIL import Image

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, PROJECT_ROOT)

from convert.export_dflfree_onnx import make_anchor_points, dfl_decode_numpy  # noqa: E402

CONF = 0.01
NMS_IOU = 0.45
IMGSZ = 640


def letterbox(img):
    w, h = img.size
    s = min(IMGSZ / w, IMGSZ / h)
    nw, nh = int(round(w * s)), int(round(h * s))
    canvas = Image.new('RGB', (IMGSZ, IMGSZ), (114, 114, 114))
    pad = ((IMGSZ - nw) // 2, (IMGSZ - nh) // 2)
    canvas.paste(img.resize((nw, nh), Image.BILINEAR), pad)
    return canvas, s, pad


def nms_classwise(boxes, scores, cids, iou_thr):
    keep = []
    order = np.argsort(-scores)
    suppressed = np.zeros(len(boxes), dtype=bool)
    for i in order:
        if suppressed[i]:
            continue
        keep.append(i)
        x1, y1, x2, y2 = boxes[i]
        area_i = max(0.0, x2 - x1) * max(0.0, y2 - y1)
        for j in order:
            if j == i or suppressed[j] or cids[j] != cids[i]:
                continue
            bx1, by1 = max(x1, boxes[j][0]), max(y1, boxes[j][1])
            bx2, by2 = min(x2, boxes[j][2]), min(y2, boxes[j][3])
            iw, ih = max(0.0, bx2 - bx1), max(0.0, by2 - by1)
            inter = iw * ih
            area_j = max(0.0, boxes[j][2] - boxes[j][0]) * max(0.0, boxes[j][3] - boxes[j][1])
            union = area_i + area_j - inter
            if union > 0 and inter / union > iou_thr:
                suppressed[j] = True
    return keep


def run_onnx(model_path, img_paths):
    import onnxruntime as ort
    sess = ort.InferenceSession(model_path, providers=['CPUExecutionProvider'])
    names = [o.name for o in sess.get_outputs()]
    ap_tab, st_tab = make_anchor_points()
    preds = {}
    for k, p in enumerate(img_paths):
        im = Image.open(p).convert('RGB')
        canvas, s, (px, py) = letterbox(im)
        x = (np.asarray(canvas, dtype=np.float32) / 255.0).transpose(2, 0, 1)[None]
        outs = sess.run(None, {'images': x})
        raw = outs[names.index('/model.22/Concat_output_0')]
        cls = outs[names.index('/model.22/Sigmoid_output_0')]
        boxes = dfl_decode_numpy(raw, ap_tab, st_tab)[0]
        scores = cls[0].max(0)
        cids = cls[0].argmax(0)
        idx = np.where(scores >= CONF)[0]
        if len(idx) == 0:
            preds[k] = []
            continue
        b = boxes[idx]
        kept = nms_classwise(b, scores[idx], cids[idx], NMS_IOU)
        res = []
        for i in kept:
            # letterbox 逆变换 -> 原图坐标
            bx1 = (b[i][0] - px) / s
            by1 = (b[i][1] - py) / s
            bx2 = (b[i][2] - px) / s
            by2 = (b[i][3] - py) / s
            res.append((int(cids[idx][i]), float(scores[idx][i]), bx1, by1, bx2, by2))
        preds[k] = res
        if (k + 1) % 32 == 0:
            print(f'  ... {k + 1}/{len(img_paths)}')
    return preds


def load_gt(label_dir, img_paths):
    """YOLO txt -> 原图绝对 xyxy"""
    gts = {}
    for k, p in enumerate(img_paths):
        h, w = Image.open(p).size[1], Image.open(p).size[0]
        lp = os.path.join(label_dir, os.path.splitext(os.path.basename(p))[0] + '.txt')
        g = []
        if os.path.exists(lp):
            for line in open(lp):
                t = line.split()
                if len(t) < 5:
                    continue
                c, cx, cy, bw, bh = int(t[0]), *map(float, t[1:5])
                g.append((c, (cx - bw / 2) * w, (cy - bh / 2) * h,
                          (cx + bw / 2) * w, (cy + bh / 2) * h))
        gts[k] = g
    return gts


def iou_xyxy(a, b):
    x1, y1 = max(a[0], b[0]), max(a[1], b[1])
    x2, y2 = min(a[2], b[2]), min(a[3], b[3])
    iw, ih = max(0.0, x2 - x1), max(0.0, y2 - y1)
    inter = iw * ih
    ua = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / ua if ua > 0 else 0.0


def compute_map(preds, gts, num_classes=80, iou_thr=0.5):
    aps, n_valid = [], 0
    per_class = {}
    for c in range(num_classes):
        # 收集该类所有预测
        dets = []
        n_gt = 0
        for k, g in gts.items():
            for gg in g:
                if gg[0] == c:
                    n_gt += 1
            for pp in preds.get(k, []):
                if pp[0] == c:
                    dets.append((pp[1], k, pp[2:]))
        if n_gt == 0:
            continue
        n_valid += 1
        if not dets:
            aps.append(0.0)
            per_class[c] = 0.0
            continue
        dets.sort(key=lambda t: -t[0])
        matched = {k: np.zeros(len(gts[k]), dtype=bool) for k in gts}
        tp = np.zeros(len(dets))
        fp = np.zeros(len(dets))
        for i, (score, k, box) in enumerate(dets):
            g = gts[k]
            best_iou, best_j = 0.0, -1
            for j, gg in enumerate(g):
                if gg[0] != c or matched[k][j]:
                    continue
                v = iou_xyxy(box, gg[1:])
                if v > best_iou:
                    best_iou, best_j = v, j
            if best_iou >= iou_thr and best_j >= 0:
                tp[i] = 1
                matched[k][best_j] = True
            else:
                fp[i] = 1
        ctp, cfp = np.cumsum(tp), np.cumsum(fp)
        rec = ctp / max(n_gt, 1)
        prec = ctp / np.maximum(ctp + cfp, 1e-9)
        mrec = np.concatenate(([0.0], rec, [1.0]))
        mpre = np.concatenate(([0.0], prec, [0.0]))
        for i in range(len(mpre) - 2, -1, -1):
            mpre[i] = max(mpre[i], mpre[i + 1])
        ap = float(np.mean(np.interp(np.linspace(0, 1, 101), mrec, mpre)))
        aps.append(ap)
        per_class[c] = ap
    return (float(np.mean(aps)) if aps else 0.0), n_valid, per_class


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(PROJECT_ROOT, 'data', 'coco128'))
    ap.add_argument('--model', default=None, help='ONNX 路径（PC 端推理）')
    ap.add_argument('--preds', default=None, help='板端导出的预测 txt（跳过推理）')
    ap.add_argument('--out', default=None, help='把预测写成 txt，供另一侧复用')
    args = ap.parse_args()

    img_dir = os.path.join(args.data, 'images', 'train2017')
    lbl_dir = os.path.join(args.data, 'labels', 'train2017')
    img_paths = sorted(glob.glob(os.path.join(img_dir, '*.jpg')))
    print(f'[*] 图片 {len(img_paths)} 张，标注目录 {lbl_dir}')
    gts = load_gt(lbl_dir, img_paths)
    n_gt = sum(len(v) for v in gts.values())
    print(f'[*] GT 框 {n_gt} 个，涉及类别 {len({g[0] for v in gts.values() for g in v})} 类')

    if args.preds:
        preds = {}
        for line in open(args.preds, encoding='utf-8'):
            t = line.split()
            if len(t) < 7:
                continue
            k = int(t[0])
            preds.setdefault(k, []).append((int(t[1]), float(t[2]),
                                            *map(float, t[3:7])))
        print(f'[*] 载入预测 {sum(len(v) for v in preds.values())} 个')
    else:
        if not args.model:
            print('[-] 要么给 --model，要么给 --preds')
            return 1
        print(f'[*] 用 {args.model} 推理（letterbox 640, conf={CONF}, nms={NMS_IOU}）')
        preds = run_onnx(args.model, img_paths)

    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            for k, v in preds.items():
                for c, s, x1, y1, x2, y2 in v:
                    f.write(f'{k} {c} {s:.6f} {x1:.2f} {y1:.2f} {x2:.2f} {y2:.2f}\n')
        print(f'[+] 预测已写出 {args.out}')

    m, n_cls, per_class = compute_map(preds, gts)
    print()
    print('=' * 62)
    print(f'  mAP@0.5 = {m:.4f}   (在 {len(img_paths)} 张、{n_gt} 个 GT 框、{n_cls} 个出现类上统计)')
    print('=' * 62)
    if args.out:
        with open(args.out + '.map', 'w', encoding='utf-8') as f:
            f.write(f'{m:.6f}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
