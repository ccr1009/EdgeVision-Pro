#!/bin/bash
cd /root/edgevision/deploy/cpp
D=/root/edgevision/third_party/mpp
echo "=== compile test_dec ==="
g++ -O2 -std=c++11 -o /root/lab/test_dec /root/lab/test_dec.cpp src/mpp_jpeg_decoder.cpp \
  -Iinclude -I$D/include -L$D/lib -lrockchip_mpp -lpthread -ldl -lrt \
  `pkg-config --cflags --libs opencv4 2>/dev/null || echo ""` \
  -Wl,-rpath,$D/lib 2>&1 | head -30
echo "=== try opencv without pkgconfig ==="
if [ ! -x /root/lab/test_dec ]; then
g++ -O2 -std=c++11 -o /root/lab/test_dec /root/lab/test_dec.cpp src/mpp_jpeg_decoder.cpp \
  -Iinclude -I$D/include -L$D/lib -lrockchip_mpp -lpthread -ldl -lrt \
  -I/usr/include/opencv4 -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
  -Wl,-rpath,$D/lib 2>&1 | head -30
fi
ls -la /root/lab/test_dec 2>/dev/null && echo BUILD_OK || echo BUILD_FAIL
