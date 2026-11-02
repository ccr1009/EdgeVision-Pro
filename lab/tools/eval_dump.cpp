// ============================================================================
//  eval_dump.cpp —— 板端批量推理，导出预测供 PC 端算 mAP@0.5
//
//  与 convert/eval_map.py 的口径必须完全一致：
//    1. letterbox(640, 灰边 114) 预处理
//    2. conf=0.01（mAP 要保留低分预测）、NMS IoU=0.45、class-aware
//    3. 坐标逆变换回原图绝对像素
//    4. 图片按文件名排序后编号（与 PC 端 sorted(glob) 一致）
//
//  输出格式: <idx> <cls> <score> <x1> <y1> <x2> <y2>
//
//  板端编译:
//    g++ -O2 -std=c++17 eval_dump.cpp postprocess_dfl_free.cpp \
//        -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include -lrknnrt \
//        `pkg-config --cflags --libs opencv4` -o eval_dump
//  运行:
//    ./eval_dump <model.rknn> <img_dir> > preds.txt
// ============================================================================
#include <rknn_api.h>
#include <opencv2/opencv.hpp>
#include <dirent.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "postprocess_dfl_free.h"

static void letterbox(const cv::Mat& src, cv::Mat& dst, int size, float& scale, int& pad_x, int& pad_y) {
    const int w = src.cols, h = src.rows;
    scale = std::min((float)size / w, (float)size / h);
    const int nw = (int)(w * scale), nh = (int)(h * scale);
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
    dst = cv::Mat(size, size, src.type(), cv::Scalar(114, 114, 114));
    pad_x = (size - nw) / 2;
    pad_y = (size - nh) / 2;
    resized.copyTo(dst(cv::Rect(pad_x, pad_y, nw, nh)));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <model.rknn> <img_dir> > preds.txt\n", argv[0]);
        return 1;
    }
    const float CONF = 0.01f, NMS_IOU = 0.45f;

    FILE* fp = fopen(argv[1], "rb");
    if (!fp) { perror("model"); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<char> mbuf((size_t)sz);
    if (fread(mbuf.data(), 1, (size_t)sz, fp) != (size_t)sz) return 1;
    fclose(fp);

    rknn_context ctx = 0;
    if (rknn_init(&ctx, mbuf.data(), (uint32_t)sz, 0, nullptr) < 0) {
        fprintf(stderr, "[-] rknn_init 失败\n");
        return 1;
    }
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_0_1_2);

    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    int bi = -1, ci = -1;
    bool bnhwc = false, cnhwc = false;
    for (uint32_t i = 0; i < io.n_output; ++i) {
        rknn_tensor_attr a{};
        a.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        for (int d = 0; d < a.n_dims; ++d) {
            if (a.dims[d] == 8400) {
                bool nhwc = (d != a.n_dims - 1);
                int ch = (int)(nhwc ? a.dims[a.n_dims - 1] : a.dims[a.n_dims - 2]);
                if (ch == 64) { bi = (int)i; bnhwc = nhwc; }
                else if (ch == 80) { ci = (int)i; cnhwc = nhwc; }
                break;
            }
        }
    }
    if (bi < 0 || ci < 0) { fprintf(stderr, "[-] 输出识别失败\n"); return 1; }

    // 收集 jpg 并按文件名排序（与 python sorted(glob) 一致）
    std::vector<std::string> files;
    DIR* d = opendir(argv[2]);
    if (!d) { perror("opendir"); return 1; }
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n.size() > 4 && (n.substr(n.size() - 4) == ".jpg" || n.substr(n.size() - 4) == ".JPG"))
            files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    fprintf(stderr, "[*] 共 %zu 张图\n", files.size());

    std::vector<rknn_output> outputs(io.n_output);
    std::vector<Detection> dets;
    for (size_t k = 0; k < files.size(); ++k) {
        cv::Mat bgr = cv::imread(std::string(argv[2]) + "/" + files[k]);
        if (bgr.empty()) { fprintf(stderr, "[-] 读图失败 %s\n", files[k].c_str()); continue; }

        float scale = 1.f;
        int px = 0, py = 0;
        cv::Mat lb, rgb;
        letterbox(bgr, lb, 640, scale, px, py);
        cv::cvtColor(lb, rgb, cv::COLOR_BGR2RGB);

        rknn_input in{};
        in.index = 0;
        in.type = RKNN_TENSOR_UINT8;
        in.fmt = RKNN_TENSOR_NHWC;
        in.size = 640 * 640 * 3;
        in.buf = rgb.data;
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, nullptr);
        for (auto& o : outputs) { memset(&o, 0, sizeof(o)); o.want_float = 1; }
        rknn_outputs_get(ctx, io.n_output, outputs.data(), nullptr);

        dflfree::decode((const float*)outputs[bi].buf, (const float*)outputs[ci].buf,
                        bnhwc, cnhwc, 640, CONF, NMS_IOU, dets);
        rknn_outputs_release(ctx, io.n_output, outputs.data());

        for (const auto& t : dets) {
            double x1 = (t.x1 - px) / scale;
            double y1 = (t.y1 - py) / scale;
            double x2 = (t.x2 - px) / scale;
            double y2 = (t.y2 - py) / scale;
            printf("%zu %d %.6f %.2f %.2f %.2f %.2f\n", k, t.class_id, t.score, x1, y1, x2, y2);
        }
        if ((k + 1) % 32 == 0) fprintf(stderr, "  ... %zu/%zu\n", k + 1, files.size());
    }

    rknn_destroy(ctx);
    return 0;
}
