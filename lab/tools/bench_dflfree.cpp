// ============================================================================
//  bench_dflfree.cpp —— DFL-Free 模型实测（对比原模型的 37ms 基线）
//
//  测什么（三层口径，必须分开讲，否则数字没意义）：
//    1. 纯推理   : 只有 rknn_run
//    2. 全周期   : rknn_inputs_set + run + rknn_outputs_get (want_float=1)
//    3. 全链路   : 全周期 + DFL 解码 + NMS（这才是替代原模型的功能等价口径）
//
//  板端编译:
//    g++ -O2 -std=c++17 bench_dflfree.cpp ../src/postprocess_dfl_free.cpp \
//        -I../include -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include \
//        -lrknnrt -o bench_dflfree
//  运行:
//    ./bench_dflfree /root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn 200
// ============================================================================
#include <rknn_api.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "postprocess_dfl_free.h"

static double now_ms() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)std::ceil(p / 100.0 * v.size()) - 1;
    return v[std::min(i, v.size() - 1)];
}

static void print_stats(const char* tag, std::vector<double> v) {
    double sum = 0;
    for (double x : v) sum += x;
    printf("%-22s mean=%7.2f  p50=%7.2f  p95=%7.2f  p99=%7.2f  FPS=%5.1f\n",
           tag, sum / v.size(), pct(v, 50), pct(v, 95), pct(v, 99),
           1000.0 * v.size() / sum);
}

int main(int argc, char** argv) {
    const char* model = argc > 1 ? argv[1] : "/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn";
    int iter = argc > 2 ? atoi(argv[2]) : 200;

    FILE* fp = fopen(model, "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<char> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); return 1; }
    fclose(fp);
    printf("[*] 模型 %s (%.2f MB)\n", model, sz / 1e6);

    rknn_context ctx = 0;
    if (rknn_init(&ctx, buf.data(), (uint32_t)sz, 0, nullptr) < 0) {
        printf("[-] rknn_init 失败\n");
        return 1;
    }
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_AUTO);

    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    printf("[+] inputs=%u outputs=%u\n", io.n_input, io.n_output);

    // 识别输出布局：找一个 8400 维
    int box_idx = -1, cls_idx = -1;
    bool box_nhwc = false, cls_nhwc = false;
    for (uint32_t i = 0; i < io.n_output; ++i) {
        rknn_tensor_attr a{};
        a.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        printf("    out[%u] %-12s dims=[", i, a.name);
        for (int d = 0; d < a.n_dims; ++d) printf("%u%s", a.dims[d], d + 1 < a.n_dims ? "," : "");
        int ch = -1;
        bool nhwc = false;
        for (int d = 0; d < a.n_dims; ++d) {
            if (a.dims[d] == 8400) {
                nhwc = (d != a.n_dims - 1);
                ch = (int)(nhwc ? a.dims[a.n_dims - 1] : a.dims[a.n_dims - 2]);
                break;
            }
        }
        printf("] ch=%d nhwc=%d\n", ch, (int)nhwc);
        if (ch == 64) { box_idx = (int)i; box_nhwc = nhwc; }
        else if (ch == 80) { cls_idx = (int)i; cls_nhwc = nhwc; }
    }
    if (box_idx < 0 || cls_idx < 0) { printf("[-] 输出识别失败\n"); rknn_destroy(ctx); return 1; }

    // 合成输入（与历史基线同口径：整幅灰图）
    std::vector<unsigned char> input(640 * 640 * 3, 128);
    rknn_input in{};
    in.index = 0;
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    in.size = 640 * 640 * 3;
    in.buf = input.data();

    std::vector<double> t_run, t_io, t_decode, t_total;
    std::vector<rknn_output> outs(io.n_output);
    std::vector<Detection> dets;

    // 预热
    for (int i = 0; i < 5; ++i) {
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, nullptr);
        for (auto& o : outs) { memset(&o, 0, sizeof(o)); o.want_float = 1; }
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
        rknn_outputs_release(ctx, io.n_output, outs.data());
    }

    for (int i = 0; i < iter; ++i) {
        double t0 = now_ms();
        rknn_inputs_set(ctx, 1, &in);

        double t1 = now_ms();
        rknn_run(ctx, nullptr);
        double t2 = now_ms();

        for (auto& o : outs) { memset(&o, 0, sizeof(o)); o.want_float = 1; }
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
        double t3 = now_ms();

        dflfree::decode((const float*)outs[box_idx].buf, (const float*)outs[cls_idx].buf,
                        box_nhwc, cls_nhwc, 640, 0.25f, 0.45f, dets);
        double t4 = now_ms();

        rknn_outputs_release(ctx, io.n_output, outs.data());
        double t5 = now_ms();

        t_run.push_back(t2 - t1);      // 只有 rknn_run
        t_io.push_back(t3 - t0);       // inputs_set + run + outputs_get
        t_decode.push_back(t4 - t3);   // DFL 解码 + NMS
        t_total.push_back(t4 - t0);    // 全链路 = IO 周期 + 解码
    }

    printf("\n=== 延迟分解（%d 次，合成灰图输入） ===\n", iter);
    print_stats("1. 纯推理 rknn_run", t_run);
    print_stats("2. IO周期(set+run+get)", t_io);
    print_stats("3. DFL解码+NMS", t_decode);
    print_stats("4. 全链路(2+3)", t_total);
    printf("\n[+] 本轮检出框数（灰图，预期 0）: %zu\n", dets.size());

    // 看一下原始输出是否不是全零（排除量化把输出压成 0 的坑）
    rknn_inputs_set(ctx, 1, &in);
    rknn_run(ctx, nullptr);
    for (auto& o : outs) { memset(&o, 0, sizeof(o)); o.want_float = 1; }
    rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
    const float* b = (const float*)outs[box_idx].buf;
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < 64 * 8400; ++i) { mn = std::min(mn, b[i]); mx = std::max(mx, b[i]); }
    printf("[+] raw_box 数值范围: [%.4f, %.4f]（全 0 说明量化异常）\n", mn, mx);
    rknn_outputs_release(ctx, io.n_output, outs.data());

    rknn_destroy(ctx);
    return 0;
}
