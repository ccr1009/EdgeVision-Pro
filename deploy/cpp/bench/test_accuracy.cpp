// ============================================================================
//  test_accuracy.cpp —— 同一张真实图，对比「原模型(84ch)」与「DFL-Free 模型」
//  的检测输出，用来验证砍头 + 新校准集 + 新量化后精度没有劣化。
//
//  板端编译:
//    g++ -O2 -std=c++17 test_accuracy.cpp postprocess.cpp postprocess_dfl_free.cpp \
//        -I. -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include \
//        -lrknnrt `pkg-config --cflags --libs opencv4` -o test_accuracy
//  运行:
//    ./test_accuracy <model.rknn> <image.jpg> <old|dflfree>
// ============================================================================
#include <rknn_api.h>
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "postprocess.h"
#include "postprocess_dfl_free.h"

static bool load_model(const char* path, std::vector<char>& buf) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); return false; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf.resize((size_t)sz);
    bool ok = fread(buf.data(), 1, (size_t)sz, fp) == (size_t)sz;
    fclose(fp);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("用法: %s <model.rknn> <image.jpg> <old|dflfree>\n", argv[0]);
        return 1;
    }
    const char* model_path = argv[1];
    const char* img_path = argv[2];
    const bool is_dflfree = std::string(argv[3]) == "dflfree";
    const float conf_thr = argc > 4 ? (float)atof(argv[4]) : 0.25f;

    std::vector<char> mbuf;
    if (!load_model(model_path, mbuf)) return 1;

    rknn_context ctx = 0;
    if (rknn_init(&ctx, mbuf.data(), (uint32_t)mbuf.size(), 0, nullptr) < 0) {
        printf("[-] rknn_init 失败\n");
        return 1;
    }
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_0);

    cv::Mat img = cv::imread(img_path);
    if (img.empty()) { printf("[-] 读图失败: %s\n", img_path); return 1; }
    cv::Mat resized, rgb;
    cv::resize(img, resized, cv::Size(640, 640));
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    rknn_input in{};
    in.index = 0;
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    in.size = 640 * 640 * 3;
    in.buf = rgb.data;
    rknn_inputs_set(ctx, 1, &in);
    rknn_run(ctx, nullptr);

    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    std::vector<rknn_output> outs(io.n_output);
    for (auto& o : outs) { memset(&o, 0, sizeof(o)); o.want_float = 1; }
    rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);

    std::vector<Detection> dets;
    if (is_dflfree) {
        // 识别 64ch / 80ch 输出
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
        if (bi < 0 || ci < 0) { printf("[-] 输出识别失败\n"); return 1; }
        dflfree::decode((const float*)outs[bi].buf, (const float*)outs[ci].buf,
                        bnhwc, cnhwc, 640, conf_thr, 0.45f, dets);
    } else {
        decode_yolov8_outputs((const float*)outs[0].buf, 84, 8400, 640, 640,
                              conf_thr, 0.45f, dets);
    }

    printf("[%s] 检出 %zu 个目标\n", is_dflfree ? "dflfree" : "old", dets.size());
    for (size_t i = 0; i < dets.size(); ++i) {
        printf("  #%zu cls=%-3d score=%.4f  box=(%.2f, %.2f, %.2f, %.2f)\n",
               i, dets[i].class_id, dets[i].score,
               dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
    }

    rknn_outputs_release(ctx, io.n_output, outs.data());
    rknn_destroy(ctx);
    return 0;
}
