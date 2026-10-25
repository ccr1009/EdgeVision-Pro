// ============================================================================
//  preview_detect.cpp —— 用 DFL-Free 新模型跑真实图，把**真实检测框**画出来
//  （替代 w4_edgevision_preview.cpp 里那两个写死的假框 "Person 96%"）
//
//  板端编译:
//    g++ -O2 -std=c++17 preview_detect.cpp postprocess_dfl_free.cpp \
//        -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include -lrknnrt \
//        `pkg-config --cflags --libs opencv4` -o preview_detect
//  运行:
//    ./preview_detect <model.rknn> <in.jpg> <out.jpg> [conf]
// ============================================================================
#include <rknn_api.h>
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "postprocess_dfl_free.h"

static const char* COCO80[] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
    "fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
    "elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
    "skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard",
    "tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
    "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch",
    "potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone",
    "microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear",
    "hair drier","toothbrush"
};

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("用法: %s <model.rknn> <in.jpg> <out.jpg> [conf]\n", argv[0]);
        return 1;
    }
    const float conf = argc > 4 ? (float)atof(argv[4]) : 0.25f;

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
        printf("[-] rknn_init 失败\n");
        return 1;
    }
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_0);

    cv::Mat src = cv::imread(argv[2]);
    if (src.empty()) { printf("[-] 读图失败\n"); return 1; }
    cv::Mat resized, rgb;
    cv::resize(src, resized, cv::Size(640, 640));
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

    std::vector<Detection> dets;
    dflfree::decode((const float*)outs[bi].buf, (const float*)outs[ci].buf,
                    bnhwc, cnhwc, 640, conf, 0.45f, dets);

    // 画框：坐标是 640x640 输入坐标系，按原图尺寸等比缩放回去
    const double sx = (double)src.cols / 640.0;
    const double sy = (double)src.rows / 640.0;
    for (const auto& d : dets) {
        int x1 = (int)(d.x1 * sx), y1 = (int)(d.y1 * sy);
        int x2 = (int)(d.x2 * sx), y2 = (int)(d.y2 * sy);
        x1 = std::max(0, std::min(x1, src.cols - 1));
        x2 = std::max(0, std::min(x2, src.cols - 1));
        y1 = std::max(0, std::min(y1, src.rows - 1));
        y2 = std::max(0, std::min(y2, src.rows - 1));

        cv::rectangle(src, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(0, 255, 0), 2);
        char label[96];
        const char* name = (d.class_id >= 0 && d.class_id < 80) ? COCO80[d.class_id] : "?";
        snprintf(label, sizeof(label), "%s %.0f%%", name, d.score * 100);
        const int tw = 10 * (int)strlen(label);
        const int ty = y1 > 22 ? y1 - 6 : y1 + 20;
        cv::rectangle(src, cv::Point(x1, ty - 18), cv::Point(x1 + tw + 10, ty + 4),
                      cv::Scalar(0, 255, 0), -1);
        cv::putText(src, label, cv::Point(x1 + 5, ty - 2),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
    }

    // 左上角 HUD
    char hud[160];
    snprintf(hud, sizeof(hud), "DFL-Free INT8 | %zu obj | conf>=%.2f", dets.size(), conf);
    cv::rectangle(src, cv::Point(0, 0), cv::Point(src.cols, 34), cv::Scalar(30, 30, 30), -1);
    cv::putText(src, hud, cv::Point(10, 24), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(0, 255, 0), 1, cv::LINE_AA);

    cv::imwrite(argv[3], src);
    printf("[+] 检出 %zu 个目标 -> %s\n", dets.size(), argv[3]);
    for (size_t i = 0; i < dets.size() && i < 10; ++i) {
        printf("    #%zu %-12s %.3f  (%.0f,%.0f,%.0f,%.0f)\n", i,
               COCO80[dets[i].class_id], dets[i].score,
               dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
    }

    rknn_outputs_release(ctx, io.n_output, outs.data());
    rknn_destroy(ctx);
    return 0;
}
