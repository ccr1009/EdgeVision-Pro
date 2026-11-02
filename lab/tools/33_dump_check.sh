#!/bin/bash
# 0 检出排查: 把 to_rgb888 输出 dump 成 PPM, scp 回 PC 人眼验证
# 同时 dump letterbox 后的 640x640
cat > /tmp/dump_pro.cpp << 'EOF'
// dump_pro.cpp: 跑完整链路但把中间图 dump 出来
#include "mpp_jpeg_decoder.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <cstdio>
#include <vector>
using namespace edgevision;
int main(int argc, char** argv) {
    const char* jpg_path = argv[1];
    FILE* f = fopen(jpg_path, "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> jpg(sz); fread(jpg.data(), 1, sz, f); fclose(f);

    MppJpegDecoder dec; dec.init(1280, 720);
    DecodedFrame fr; dec.decode(jpg.data(), jpg.size(), &fr);
    printf("decode ok: %dx%d stride=%d\n", fr.width, fr.height, fr.hor_stride);

    std::vector<uint8_t> rgb(fr.width * fr.height * 3);
    dec.to_rgb888(fr, rgb.data());
    // dump RGB (直接 PPM P6)
    FILE* o = fopen("/tmp/pro_rgb.ppm", "wb");
    fprintf(o, "P6\n%d %d\n255\n", fr.width, fr.height);
    fwrite(rgb.data(), 1, rgb.size(), o); fclose(o);
    printf("dumped /tmp/pro_rgb.ppm\n");

    // letterbox 到 640
    cv::Mat src(fr.height, fr.width, CV_8UC3, rgb.data());
    cv::Mat dst; float scale = 640.0f / (fr.width > fr.height ? fr.width : fr.height);
    int nw = (int)(fr.width * scale), nh = (int)(fr.height * scale);
    cv::resize(src, dst, cv::Size(nw, nh));
    cv::copyMakeBorder(dst, dst, (640-nh)/2, 640-nh-(640-nh)/2, 0, 640-nw,
                       cv::BORDER_CONSTANT, cv::Scalar(114,114,114));
    cv::imwrite("/tmp/pro_lb.jpg", dst);
    printf("dumped /tmp/pro_lb.jpg (scale=%.3f pad_y=%d)\n", scale, (640-nh)/2);
    return 0;
}
EOF
g++ -O2 -o /tmp/dump_pro /tmp/dump_pro.cpp \
    -I/root/edgevision/deploy/cpp/include \
    -I/root/edgevision/third_party/mpp/include \
    -I/usr/include/opencv4 \
    /root/edgevision/deploy/cpp/src/mpp_jpeg_decoder.cpp \
    -L/root/edgevision/third_party/mpp/lib -Wl,-Bstatic -lrockchip_mpp -Wl,-Bdynamic \
    -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lpthread -lm -ldl -lrt 2>&1 | head -5
echo "=== run dump ==="
/tmp/dump_pro /tmp/mppo/a.jpg
echo "=== 用 OpenCV 直接 decode 同一张图做对照 ==="
python3 -c "
import cv2
img = cv2.imread('/tmp/mppo/a.jpg')
print('cv2 decode:', img.shape)
cv2.imwrite('/tmp/pro_cv2.jpg', cv2.resize(img, (640, 360)))
"
echo "=== 顺带确认旧链路 (imdecode) 的模型输出 ==="
cd /root/edgevision
./deploy/cpp/build_native/edgevision_pro_rknn /tmp/mppo/a.jpg 2>/dev/null | grep detections
