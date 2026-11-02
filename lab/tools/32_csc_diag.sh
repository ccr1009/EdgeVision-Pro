#!/bin/bash
# 查 1: build_native 的编译优化等级
grep -r "CMAKE_BUILD_TYPE" /root/edgevision/deploy/cpp/build_native/CMakeCache.txt
echo "---"
# 查 2: 编译 flags 里有没有 -O2
grep "CXX_FLAGS" /root/edgevision/deploy/cpp/build_native/CMakeFiles/edgevision_pro_rknn.dir/flags.make
echo "---"
# 查 3: 对照 mpp_jpeg_decoder 在 debug 下的 CSC 速度 (in-process, 跑一张图已有数字 118ms)
# 临时用 -O2 单独编译一个 to_rgb888 micro-bench 验证优化等级就是根因
cat > /tmp/csc_bench.cpp << 'EOF'
#include <cstdio>
#include <chrono>
#include <vector>
#include "mpp_jpeg_decoder.h"
using namespace edgevision;
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> jpg(sz); fread(jpg.data(), 1, sz, f); fclose(f);
    MppJpegDecoder dec; dec.init(1280, 720);
    DecodedFrame fr; dec.decode(jpg.data(), jpg.size(), &fr);
    std::vector<uint8_t> rgb(fr.width * fr.height * 3);
    for (int i = 0; i < 20; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        dec.to_rgb888(fr, rgb.data());
        auto t1 = std::chrono::high_resolution_clock::now();
        printf("csc %2d: %.3f ms\n", i,
               std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return 0;
}
EOF
g++ -O2 -o /tmp/csc_bench_o2 /tmp/csc_bench.cpp \
    -I/root/edgevision/deploy/cpp/include \
    -I/root/edgevision/third_party/mpp/include \
    /root/edgevision/deploy/cpp/src/mpp_jpeg_decoder.cpp \
    -L/root/edgevision/third_party/mpp/lib -Wl,-Bstatic -lrockchip_mpp -Wl,-Bdynamic -lpthread -lm -ldl -lrt 2>&1 | head -5
echo "=== O2 build run ==="
/tmp/csc_bench_o2 /tmp/mppo/a.jpg
echo "=== no-O build run ==="
g++ -o /tmp/csc_bench_o0 /tmp/csc_bench.cpp \
    -I/root/edgevision/deploy/cpp/include \
    -I/root/edgevision/third_party/mpp/include \
    /root/edgevision/deploy/cpp/src/mpp_jpeg_decoder.cpp \
    -L/root/edgevision/third_party/mpp/lib -Wl,-Bstatic -lrockchip_mpp -Wl,-Bdynamic -lpthread -lm -ldl -lrt
/tmp/csc_bench_o0 /tmp/mppo/a.jpg
