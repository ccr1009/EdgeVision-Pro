#!/bin/bash
cd /root/lab
D=/root/edgevision/third_party/mpp
g++ -O2 -std=c++11 -o rga_dump rga_dump.cpp -I$D/include -L$D/lib -lrockchip_mpp \
  -I/usr/include/rga -lrga -lpthread -ldl -lrt -Wl,-rpath,$D/lib 2>&1 | grep -E "error" | head
[ -x ./rga_dump ] && timeout 60 ./rga_dump /tmp/mppo/a.jpg 1280 720 /tmp/rga_out.ppm 2>/dev/null
echo "=== compare with OpenCV ==="
python3 - <<'PY' 2>/dev/null || echo "no numpy/cv2"
import numpy as np
try:
    import cv2
    ref = cv2.imread('/tmp/mppo/a.jpg')  # BGR
    ref = cv2.cvtColor(ref, cv2.COLOR_BGR2RGB)
except Exception as e:
    print("cv2 fail", e); raise SystemExit(0)
import struct
with open('/tmp/rga_out.ppm','rb') as f:
    assert f.readline().strip()==b'P6'
    w,h = map(int, f.readline().split())
    f.readline()
    data = np.frombuffer(f.read(), dtype=np.uint8).reshape(h,w,3)
print("PPM", w,h, "ref", ref.shape)
if ref.shape[:2]==(h,w):
    for i,ch in enumerate("RGB"):
        d = np.abs(data[:,:,i].astype(int)-ref[:,:,i].astype(int))
        mse = (d**2).mean(); psnr = 10*np.log10(255**2/mse) if mse>0 else 99
        print(f"{ch}: PSNR={psnr:.2f} dB max|d|={d.max()} same%={(d==0).mean()*100:.1f}")
PY
