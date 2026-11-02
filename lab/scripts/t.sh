#!/bin/bash
set -e
cd /root/edgevision/deploy/cpp
echo "=== source check ==="
ls -la include/mpp_jpeg_decoder.h src/mpp_jpeg_decoder.cpp
echo "=== MPP third_party ==="
ls -la /root/edgevision/third_party/mpp/lib/ 2>/dev/null || echo "MISSING third_party/mpp/lib"
echo "=== test standalone compile ==="
cat > /tmp/tmppp.cpp <<'EOF'
#include "mpp_jpeg_decoder.h"
#include <cstdio>
int main(){ edgevision::MppJpegDecoder d; printf("init=%d ready=%d\n", d.init(1280,720), d.is_ready()); }
EOF
D=/root/edgevision/third_party/mpp
g++ -O2 -std=c++11 -o /tmp/tmppp /tmp/tmppp.cpp src/mpp_jpeg_decoder.cpp -Iinclude -I$D/include -L$D/lib -lrockchip_mpp -lpthread -ldl -lrt -Wl,-rpath,$D/lib 2>&1 | head -30
echo "=== run ==="
timeout 20 /tmp/tmppp && echo "RUN_OK" || echo "RUN_FAIL $?"
