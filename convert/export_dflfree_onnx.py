# ==============================================================================
# EdgeVision-RK3588: YOLOv8 "DFL-Free" ONNX Exporter (Graph Surgery)
# ------------------------------------------------------------------------------
# 背景（实测依据，见 docs/BENCHMARKS.md 2.2.3）：
#   原 yolov8n.onnx 的 DFL 检测头在 RK3588 NPU 上无法融合，回退到 CPU：
#     Transpose 9.18ms + Softmax 4.47ms + Reshape 2.66/0.94/0.77ms = 18.44ms (49%)
#   把 DFL 从图里切掉，让 NPU 只跑卷积，解码放到 C++ (A76 上约 1~2ms)。
#
# 切点（基于 ultralytics 8.x 导出的 opset12 图）：
#   原图:  images -> ... -> /model.22/Concat_1 (1,64,8400)  ← 3尺度 raw box 分布
#                              /model.22/Mul_2  -> Sigmoid (1,80,8400) ← cls 概率
#                              dfl(Reshape/Transpose/Softmax/Conv) -> 解码 -> Concat_3
#   新图:  images -> ... -> raw_box (1,64,8400)  +  cls_sig (1,80,8400)
#
# 切掉后需要 C++ 侧补充（deploy/cpp/src/postprocess_dfl_free.cpp）：
#   1) DFL: raw_box 按 4x16 分组做 softmax，与 [0..15] 加权求和 -> 4 个距离
#   2) dist2bbox: 用 anchor center 转成 xyxy
#   3) NMS
# ==============================================================================
import os
import sys
import argparse
import numpy as np
import onnx
from onnx import helper, TensorProto

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
DEFAULT_SRC = os.path.join(PROJECT_ROOT, 'data', 'models', 'yolov8n.onnx')
DEFAULT_DST = os.path.join(PROJECT_ROOT, 'data', 'models', 'yolov8n_dflfree.onnx')

def _resolve(graph, tag=''):
    """按图拓扑定位切点，不依赖 value_info 的 shape（ultralytics 导出通常没有中间 shape）。

    raw_box : DFL 的第一个 Reshape 节点的输入 = 3 尺度 box 分布 concat 后的 (1,64,8400)
    cls_sig : Detect 头里最后的 Sigmoid 输出 = cls 概率 (1,80,8400)
    """
    # --- raw_box：DFL 分支的入口 ---
    dfl_reshape = None
    for n in graph.node:
        if n.op_type == 'Reshape' and '/dfl/' in n.name:
            dfl_reshape = n
            break
    if dfl_reshape is None:
        raise RuntimeError("找不到 DFL 的 Reshape 节点（name 含 '/dfl/'），请先跑 inspect_onnx.py 看结构")
    box_out = dfl_reshape.input[0]

    # --- cls_sig：Detect 头最后一个 Sigmoid ---
    sig = None
    for n in graph.node:
        if n.op_type == 'Sigmoid' and n.name.startswith('/model.22'):
            sig = n          # 取最后一个（三个尺度里只会有最终 concat 前那个）
    if sig is None:
        raise RuntimeError("找不到 model.22 下的 Sigmoid 节点")
    cls_out = sig.output[0]

    print(f"[+] raw_box 切点: {box_out}   (生产者=DflReshape『{dfl_reshape.name}』的输入)")
    print(f"[+] cls_sig 切点: {cls_out}   (生产者={sig.name})")
    return box_out, cls_out


def extract_subgraph(src_path, dst_path, box_out, cls_out):
    """从 src 图中裁剪出「输入 -> box_out / cls_out」的子图并另存。"""
    m = onnx.load(src_path)
    g = m.graph

    producers = {}
    for n in g.node:
        for o in n.output:
            producers[o] = n

    graph_inputs = {i.name for i in g.input}
    init_names = {t.name for t in g.initializer}

    # 反向 BFS 收集依赖
    needed = set()
    stack = [box_out, cls_out]
    while stack:
        t = stack.pop()
        if t in needed or t in graph_inputs or t in init_names:
            continue
        needed.add(t)
        p = producers.get(t)
        if p is None:
            continue
        for inp in p.input:
            if inp:
                stack.append(inp)

    # 按原顺序保留被依赖的节点（ONNX 节点必须拓扑有序）
    new_nodes = [n for n in g.node if any(o in needed for o in n.output)]

    # 仅保留用到的 initializer
    used_init = set()
    for n in new_nodes:
        for inp in n.input:
            if inp in init_names:
                used_init.add(inp)
    new_inits = [t for t in g.initializer if t.name in used_init]

    # 重建 value_info（只留还有意义的）
    kept_tensors = needed | graph_inputs
    new_vi = [v for v in g.value_info if v.name in kept_tensors]

    def mk_output(name, channels, anchors):
        return helper.make_tensor_value_info(name, TensorProto.FLOAT, [1, channels, anchors])

    new_graph = helper.make_graph(
        nodes=new_nodes,
        name=g.name + '_dflfree',
        inputs=list(g.input),
        outputs=[mk_output(box_out, 64, 8400), mk_output(cls_out, 80, 8400)],
        initializer=new_inits,
        value_info=new_vi,
    )
    new_model = helper.make_model(
        new_graph,
        producer_name='edgevision-rk3588-dflfree',
        opset_imports=m.opset_import,
        ir_version=m.ir_version,
    )
    onnx.checker.check_model(new_model)
    onnx.save(new_model, dst_path)

    removed = len(g.node) - len(new_nodes)
    print(f"[+] 裁剪完成: 节点 {len(g.node)} -> {len(new_nodes)} (去掉 {removed} 个，含整个 DFL 解码链)")
    print(f"[+] 输出: {dst_path} ({os.path.getsize(dst_path)} bytes)")
    return dst_path


# ------------------------------------------------------------------------------
# 参考解码（纯 numpy）：用来验证「砍头后的 ONNX + CPU 解码 == 原 ONNX 输出」
# C++ 侧实现必须与此严格一致
# ------------------------------------------------------------------------------
def make_anchor_points(strides=(8, 16, 32), imgsz=640):
    """生成 anchor centers（**格子单位**，即 (x+0.5, y+0.5)）与每个 anchor 的 stride。
    顺序必须与 concat 顺序一致: stride 8 (80x80) -> 16 (40x40) -> 32 (20x20)。

    注意单位：ultralytics 的 dist2bbox 是在格子单位下算的，最后整体 * stride 才变像素。
    这里保留格子单位，decode 时再乘 stride —— 这正是最初解码差了 8 倍的原因。
    """
    pts, sts = [], []
    for s in strides:
        n = imgsz // s
        y, x = np.meshgrid(np.arange(n), np.arange(n), indexing='ij')
        pts.append(np.stack([x.reshape(-1) + 0.5, y.reshape(-1) + 0.5], axis=1))
        sts.append(np.full(n * n, float(s), dtype=np.float32))
    return np.concatenate(pts, axis=0).astype(np.float32), np.concatenate(sts)


def dfl_decode_numpy(raw_box, anchors, strides, reg_max=16):
    """raw_box: (1,64,8400) -> boxes (1,8400,4) in xyxy pixel"""
    n, c, a = raw_box.shape
    assert c == 4 * reg_max, f"channel {c} != 4*{reg_max}"
    d = raw_box[0].reshape(4, reg_max, a)          # (4,16,8400)
    d = d - d.max(axis=1, keepdims=True)           # 数值稳定
    e = np.exp(d)
    prob = e / e.sum(axis=1, keepdims=True)        # softmax over 16 bins
    bins = np.arange(reg_max, dtype=np.float32).reshape(1, reg_max, 1)
    dist = (prob * bins).sum(axis=1)               # (4,8400) 格子单位 left,top,right,bottom
    s = strides[:, None]                           # (8400,1) 每 anchor 的 stride
    x1y1 = (anchors - dist[:2].T) * s
    x2y2 = (anchors + dist[2:].T) * s
    return np.concatenate([x1y1, x2y2], axis=1)[None, ...]


def verify(src_path, dst_path, box_out, cls_out):
    import onnxruntime as ort

    print("\n=== 数值等价性验证（ONNXRuntime CPU） ===")
    rng = np.random.default_rng(2026)
    # 两路输入：随机噪声（最坏情况） + 真实图（若存在）
    inputs = [('random', rng.uniform(0, 1, (1, 3, 640, 640)).astype(np.float32))]
    real = os.path.join(PROJECT_ROOT, 'data', 'calib', 'calib_000.jpg')
    if os.path.exists(real):
        try:
            from PIL import Image
            img = Image.open(real).convert('RGB').resize((640, 640))
            arr = np.asarray(img, dtype=np.float32).transpose(2, 0, 1)[None] / 255.0
            inputs.append(('real_jpg', arr))
        except Exception as e:
            print(f"[!] 跳过真实图验证: {e}")

    s_full = ort.InferenceSession(src_path, providers=['CPUExecutionProvider'])
    s_cut = ort.InferenceSession(dst_path, providers=['CPUExecutionProvider'])
    anchors, strides = make_anchor_points()

    ok_all = True
    for tag, x in inputs:
        full = s_full.run(None, {s_full.get_inputs()[0].name: x})[0]      # (1,84,8400)
        cut = s_cut.run(None, {s_cut.get_inputs()[0].name: x})
        names = [o.name for o in s_cut.get_outputs()]
        raw_box = cut[names.index(box_out)]
        cls_sig = cut[names.index(cls_out)]

        boxes = dfl_decode_numpy(raw_box, anchors, strides)                        # (1,8400,4) xyxy
        # 原模型输出是 xywh（ultralytics export 默认 dist2bbox(xywh=True)*strides）
        ref_wh = full[0, :4].T[None]                                      # (1,8400,4) cx,cy,w,h
        cx, cy, w, h = ref_wh[..., 0], ref_wh[..., 1], ref_wh[..., 2], ref_wh[..., 3]
        ref_boxes = np.stack([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2], axis=-1)
        ref_cls = full[:, 4:, :]                                          # (1,80,8400)

        e_box = float(np.abs(boxes - ref_boxes).max())
        e_cls = float(np.abs(cls_sig - ref_cls).max())
        # 相对误差更有意义：坐标量级是像素
        rel = e_box / max(1e-6, float(np.abs(ref_boxes).mean()))
        print(f"  [{tag}] box max|Δ| = {e_box:.6f} px (相对 {rel:.2e}) | cls max|Δ| = {e_cls:.6f}")
        if e_box > 1e-3 or e_cls > 1e-4:
            print(f"  [!] {tag} 误差偏大，需检查切点/解码逻辑")
            ok_all = False

    print("[SUCCESS] DFL-Free 导出与解码验证通过" if ok_all else "[FAILED] 验证未通过")
    return ok_all


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=DEFAULT_SRC)
    ap.add_argument('--dst', default=DEFAULT_DST)
    ap.add_argument('--skip-verify', action='store_true')
    args = ap.parse_args()

    m = onnx.load(args.src)
    # shape 推断仅用于日志确认，切点定位不依赖它
    try:
        m = onnx.shape_inference.infer_shapes(m)
    except Exception as e:
        print(f"[!] shape inference 跳过: {e}")
    box_out, cls_out = _resolve(m.graph, 'both')
    vi = {v.name: v for v in m.graph.value_info}
    for tag, name in (('raw_box', box_out), ('cls_sig', cls_out)):
        if name in vi:
            d = [x.dim_value for x in vi[name].type.tensor_type.shape.dim]
            print(f"    {tag} 推断 shape = {d}")

    extract_subgraph(args.src, args.dst, box_out, cls_out)
    if not args.skip_verify:
        verify(args.src, args.dst, box_out, cls_out)
