#include <iostream>
#include <vector>
#include <algorithm>
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

    // 读取此前调焦拍好的真实清晰房间照片
    cv::Mat mat = cv::imread("/root/lab/cam1_focused.jpg");
    if (mat.empty()) {
        std::cerr << "[-] 无法读取 cam1_focused.jpg\n";
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

    std::cout << "[+] 静态实拍图像推理完成！" << std::endl;
    std::cout << "    最高置信度: " << max_class_score << " (类别 ID: " << best_c << ")" << std::endl;

    std::vector<Detection> dets;
    decode_yolov8_outputs(data, 84, 8400, 640, 640, 0.15f, 0.45f, dets);
    std::cout << "    在阈值 0.15 下检出目标数: " << dets.size() << std::endl;
    for (const auto& d : dets) {
        std::cout << "      -> 目标: class=" << d.class_id << ", conf=" << d.score 
                  << ", box=[" << d.x1 << ", " << d.y1 << ", " << d.x2 << ", " << d.y2 << "]" << std::endl;
    }

    rknn_outputs_release(ctx, 1, &out);
    rknn_destroy(ctx);
    return 0;
}
