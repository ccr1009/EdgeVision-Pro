#!/bin/bash
# 数值对比: MPP CSC 输出 vs cv2.imdecode 参考 (同一张 a.jpg)
# 若结构相似度高 => to_rgb888 正确, 0 检出的原因在别处 (conf阈值/模型)
cat > /tmp/cmp_pro.py << 'EOF'
import cv2, numpy as np
# MPP 链路的 RGB dump (PPM P6)
with open('/tmp/pro_rgb.ppm','rb') as f:
    data = f.read()
# parse P6 header
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
rgb = np.frombuffer(parts[3], dtype=np.uint8).reshape(h, w, 3)
print('MPP PPM:', w, 'x', h)
# cv2 直接解同一张 jpg (BGR)
ref_bgr = cv2.imread('/tmp/mppo/a.jpg')
print('cv2 ref:', ref_bgr.shape)
ref_rgb = cv2.cvtColor(ref_bgr, cv2.COLOR_BGR2RGB)
if ref_rgb.shape[:2] != (h, w):
    ref_rgb = cv2.resize(ref_rgb, (w, h))
# 每通道线性回归 (RGA 案例用过, 判断是全局色彩偏移还是结构错乱)
for i, name in enumerate(['R','G','B']):
    x = ref_rgb[:,:,i].astype(np.float32).ravel()
    y = rgb[:,:,i].astype(np.float32).ravel()
    k, b = np.polyfit(x, y, 1)
    resid = np.abs(k*x+b - y)
    print(f'{name}: out = {k:.4f}*ref + {b:.2f}  resid mean={resid.mean():.2f} max={resid.max():.0f}')
# 结构相似度: 灰度相关系数
g1 = cv2.cvtColor(ref_rgb, cv2.COLOR_RGB2GRAY).astype(np.float32)
g2 = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY).astype(np.float32)
corr = np.corrcoef(g1.ravel(), g2.ravel())[0,1]
print(f'gray correlation: {corr:.4f}  (>0.9 = 结构正确)')
# 检查是否 R/B 通道互换 (常见错误)
rb_swap = np.corrcoef(rgb[:,:,2].ravel().astype(np.float32), ref_rgb[:,:,0].ravel().astype(np.float32))[0,1]
print(f'corr(out_B, ref_R) = {rb_swap:.4f}  (若 > corr(out_R, ref_R) 则 RGB 顺序反了)')
EOF
/root/rknn_env/bin/python3 /tmp/cmp_pro.py
