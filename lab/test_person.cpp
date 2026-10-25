#include <iostream>
#include <vector>
#include <opencv2/opencv.hpp>
#include <rknn_api.h>
#include "postprocess.h"

int main() {
    FILE* fp = fopen("/root/edgevision/data/models/yolov8n_rk3588_i8.rknn", "rb");
    fseek(fp, 0, SEEK_END); size_t s = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::vector<char> b(s); fread(b.data(), 1, s, fp); fclose(fp);

    rknn_context ctx = 0;
    rknn_init(&ctx, b.data(), s, 0, nullptr);
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_0);

    // 读取含有人像的 cam2_warm.jpg
    cv::Mat mat = cv::imread("/root/lab/cam2_warm.jpg");
    if (mat.empty()) {
        std::cerr << "[-] 无法读取 cam2_warm.jpg\n";
        return 1;
    }

    cv::Mat resized, rgb;
    cv::resize(mat, resized, cv::Size(640, 640));
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    rknn_input in{};
    in.index = 0;
    in.buf = rgb.data;
    in.size = 640 * 640 * 3;
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;

    rknn_inputs_set(ctx, 1, &in);
    rknn_run(ctx, nullptr);

    rknn_output out{};
    out.want_float = 1;
    out.index = 0;
    rknn_outputs_get(ctx, 1, &out, nullptr);

    float* data = (float*)out.buf;
    float max_class_score = -1e9;
    int best_c = -1;

    for (int c = 4; c < 84; ++c) {
        for (int i = 0; i < 8400; ++i) {
            float val = data[c * 8400 + i];
            if (val > max_class_score) {
                max_class_score = val;
                best_c = c - 4;
            }
        }
    }

    std::cout << "[+] cam2_warm.jpg 人像图推理完成！" << std::endl;
    std::cout << "    最高类别置信度: " << max_class_score << " (类别 ID: " << best_c << ")" << std::endl;

    for (float conf : {0.50f, 0.35f, 0.25f, 0.15f, 0.05f}) {
        std::vector<Detection> dets;
        decode_yolov8_outputs(data, 84, 8400, 640, 640, conf, 0.45f, dets);
        std::cout << "    conf=" << conf << " 检出目标数: " << dets.size() << std::endl;
        for (const auto& d : dets) {
            std::cout << "      -> 类别: " << d.class_id << ", 置信度: " << d.score 
                      << ", 坐标: [" << d.x1 << ", " << d.y1 << ", " << d.x2 << ", " << d.y2 << "]" << std::endl;
        }
    }

    rknn_outputs_release(ctx, 1, &out);
    rknn_destroy(ctx);
    return 0;
}
