"""探查 yolov8n.onnx 的图结构，定位 model.22 (Detect head) 的切点。
模型说明：YOLOv8n, 1x3x640x640 -> 1x84x8400
Detect 头结构（ultralytics v8）：
  每个 stride 分支 -> cv2 (box, 64ch = 4*reg_max) / cv3 (cls, 80ch)
  DFL: reshape 64 -> (1,4,16,s,s) -> softmax -> conv [0..15] -> (1,4,s,s)
  最后 concat 三尺度 + transpose -> (1,84,8400)
"""
import sys
import onnx
from onnx import numpy_helper

MODEL = sys.argv[1] if len(sys.argv) > 1 else r"D:/a研究生/项目/rk3588项目/edgevision-rk3588/data/models/yolov8n.onnx"

m = onnx.load(MODEL)
g = m.graph
print(f"ir_version={m.ir_version}  opset={[(o.domain, o.version) for o in m.opset_import]}")
print(f"inputs : {[(i.name, [d.dim_value or d.dim_param for d in i.type.tensor_type.shape.dim]) for i in g.input]}")
print(f"outputs: {[(o.name, [d.dim_value or d.dim_param for d in o.type.tensor_type.shape.dim]) for o in g.output]}")
print(f"nodes={len(g.node)}  initializers={len(g.initializer)}")

# 打印 model.22 开头的所有节点
print("\n=== nodes with name starting 'model.22' ===")
producers = {out: n for n in g.node for out in n.output}
for n in g.node:
    if n.name.startswith("model.22"):
        print(f"[{n.op_type:12s}] {n.name:45s} in={list(n.input)} out={list(n.output)}")

print("\n=== 所有非 model.22 但输出被 model.22 消费的 tensor ===")
consumed = set()
for n in g.node:
    if n.name.startswith("model.22"):
        consumed.update(n.input)
for t in sorted(consumed):
    p = producers.get(t)
    if p is not None and not p.name.startswith("model.22"):
        print(f"  {t:30s} <- [{p.op_type}] {p.name}")
    elif t not in producers:
        print(f"  {t:30s} <- (graph input / initializer)")
