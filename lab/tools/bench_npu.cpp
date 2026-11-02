// ============================================================================
//  bench_npu.cpp —— RK3588 板端 NPU 推理性能探针 (v2)
//
//  作用: 在真机上一次加载模型, 交叉对比三件事对单帧延迟的影响:
//        1. 核心绑定: 单核 Core0 / 三核 0+1+2 / AUTO
//        2. 喂数据方式: 原生 int8 直喂(pass_through=1) vs uint8 转换(pass_through=0)
//        3. 测量范围: 全周期(拷输入+推理+取输出) vs 纯推理(只测 rknn_run)
//
//  为什么要分这么细: v1 实测单帧 37ms, 比官方理论 5.5~7ms 差 5 倍, 且 NPU 占用仅 48%
//        —— 说明瓶颈大概率不在 NPU 算力, 而在每帧的数据搬运。能量化这件事,
//        才谈得上后面用 RGA/DMA-BUF 做零拷贝优化。
//
//  板端编译(板子上原生编, 不要交叉编译):
//    g++ -O2 -std=c++17 bench_npu.cpp \
//        -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include \
//        -L/root/edgevision/deploy/cpp/3rdparty/rknn_api/lib \
//        -lrknnrt -o bench_npu
//
//  运行:
//    LD_LIBRARY_PATH=/root/edgevision/deploy/cpp/3rdparty/rknn_api/lib \
//    ./bench_npu /root/edgevision/data/models/yolov8n_rk3588_i8.rknn 300
// ============================================================================
#include <rknn_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double g_now_ms() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)std::ceil(p / 100.0 * v.size()) - 1;
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

static bool read_file(const char* path, std::vector<char>& out) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); return false; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    out.resize((size_t)sz);
    size_t got = fread(out.data(), 1, (size_t)sz, fp);
    fclose(fp);
    return got == (size_t)sz;
}

struct Stats {
    double mean = 0, p50 = 0, p95 = 0, p99 = 0, vmin = 0, vmax = 0, fps = 0;
    bool ok = false;
};

// 测量一轮。pure_run=true 时只在循环外设置一次输入, 循环里只计 rknn_run。
static Stats measure(rknn_context ctx, rknn_input* in, uint32_t n_out, int iters,
                     bool pure_run) {
    Stats s;
    std::vector<rknn_output> outs(n_out);
    std::vector<double> lat;
    lat.reserve((size_t)iters);

    auto one_output_cycle = [&]() {
        memset(outs.data(), 0, sizeof(rknn_output) * n_out);
        for (auto& o : outs) o.want_float = 0;
        rknn_outputs_get(ctx, n_out, outs.data(), nullptr);
        rknn_outputs_release(ctx, n_out, outs.data());
    };

    if (pure_run) {
        if (rknn_inputs_set(ctx, 1, in) != RKNN_SUCC) return s;
    }

    // 预热
    for (int i = 0; i < 20; ++i) {
        if (!pure_run) rknn_inputs_set(ctx, 1, in);
        if (rknn_run(ctx, nullptr) != RKNN_SUCC) return s;
        one_output_cycle();
    }

    for (int i = 0; i < iters; ++i) {
        double t0 = g_now_ms();
        if (!pure_run) rknn_inputs_set(ctx, 1, in);
        int ret = rknn_run(ctx, nullptr);
        if (ret != RKNN_SUCC) { printf("[!] rknn_run 失败: %d\n", ret); return s; }
        one_output_cycle();
        lat.push_back(g_now_ms() - t0);
    }

    double sum = 0.0;
    for (double v : lat) sum += v;
    s.mean = sum / lat.size();
    s.p50 = pct(lat, 50);
    s.p95 = pct(lat, 95);
    s.p99 = pct(lat, 99);
    s.vmin = *std::min_element(lat.begin(), lat.end());
    s.vmax = *std::max_element(lat.begin(), lat.end());
    s.fps = 1000.0 / s.mean;
    s.ok = true;
    return s;
}

int main(int argc, char** argv) {
    const char* model = argc > 1 ? argv[1] : "/root/edgevision/data/models/yolov8n_rk3588_i8.rknn";
    int iters = argc > 2 ? atoi(argv[2]) : 300;

    // --perf: 逐层耗时剖析模式(需在 rknn_init 时打开采集标记)
    bool perf_mode = (argc > 3 && strcmp(argv[3], "--perf") == 0);

    std::vector<char> blob;
    if (!read_file(model, blob)) return 1;

    rknn_context ctx = 0;
    uint32_t init_flag = perf_mode ? RKNN_FLAG_COLLECT_PERF_MASK : 0;
    int ret = rknn_init(&ctx, blob.data(), (uint32_t)blob.size(), init_flag, nullptr);
    if (ret != RKNN_SUCC) { printf("rknn_init 失败: %d\n", ret); return 1; }

    rknn_sdk_version ver{};
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC) {
        printf("运行时 API: %s | 驱动: %s\n", ver.api_version, ver.drv_version);
    }
    printf("模型: %s (%.2f MB)\n", model, blob.size() / 1024.0 / 1024.0);

    rknn_input_output_num io_num{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));

    rknn_tensor_attr attr{};
    attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr));
    printf("输入: n_dims=%u dims=[%u,%u,%u,%u] fmt=%d(0=NCHW,1=NHWC) type=%d(2=i8,3=u8) "
           "zp=%d scale=%f\n",
           attr.n_dims, attr.dims[0], attr.dims[1], attr.dims[2], attr.dims[3],
           attr.fmt, attr.type, attr.zp, attr.scale);
    // 反量化到 0~255 的等价灰度, 换算成模型要求的 INT8
    int8_t fill_i8 = (int8_t)(114 - attr.zp);

    size_t in_bytes = 1;
    for (uint32_t i = 0; i < attr.n_dims; ++i) in_bytes *= attr.dims[i];

    std::vector<uint8_t> buf_u8(in_bytes, 114);
    std::vector<int8_t> buf_i8(in_bytes, fill_i8);

    // 两种喂法: uint8 转换 / 原生 int8 直喂
    rknn_input feed_conv{};
    feed_conv.index = 0;
    feed_conv.buf = buf_u8.data();
    feed_conv.size = (uint32_t)buf_u8.size();
    feed_conv.type = RKNN_TENSOR_UINT8;
    feed_conv.fmt = RKNN_TENSOR_NHWC;
    feed_conv.pass_through = 0;

    rknn_input feed_native{};
    feed_native.index = 0;
    feed_native.buf = buf_i8.data();
    feed_native.size = (uint32_t)buf_i8.size();
    feed_native.type = (rknn_tensor_type)attr.type;
    feed_native.fmt = (rknn_tensor_format)attr.fmt;
    feed_native.pass_through = 1;

    printf("每轮 %d 次, 输入 %.2f MB\n\n", iters, in_bytes / 1024.0 / 1024.0);

    if (perf_mode) {
        rknn_set_core_mask(ctx, RKNN_NPU_CORE_0);
        for (int i = 0; i < 5; ++i) {  // 预热, 必须真跑过才有层耗时
            rknn_inputs_set(ctx, 1, &feed_conv);
            rknn_run(ctx, nullptr);
        }
        rknn_perf_run pr{};
        if (rknn_query(ctx, RKNN_QUERY_PERF_RUN, &pr, sizeof(pr)) == RKNN_SUCC) {
            printf("[单次 rknn_run] run_duration = %.2f ms\n", pr.run_duration / 1000.0);
        }
        rknn_perf_detail detail{};
        if (rknn_query(ctx, RKNN_QUERY_PERF_DETAIL, &detail, sizeof(detail)) == RKNN_SUCC &&
            detail.perf_data) {
            printf("\n========== 逐层耗时明细 ==========\n");
            fwrite(detail.perf_data, 1, (size_t)detail.data_len, stdout);
            printf("\n==================================\n");
        }
        rknn_destroy(ctx);
        return 0;
    }

    struct CoreCase { const char* name; rknn_core_mask mask; };
    CoreCase cores[] = {
        {"单核 Core0", RKNN_NPU_CORE_0},
        {"三核 0+1+2", RKNN_NPU_CORE_0_1_2},
        {"AUTO",       RKNN_NPU_CORE_AUTO},
    };

    printf("%-12s %-18s %-10s %8s %8s %8s %8s %8s\n",
           "核心绑定", "喂数据方式", "测量范围", "mean", "p50", "p95", "p99", "FPS");
    printf("%s\n", std::string(96, '-').c_str());

    for (auto& c : cores) {
        if (rknn_set_core_mask(ctx, c.mask) != RKNN_SUCC) {
            printf("[!] set_core_mask 失败: %s\n", c.name);
            continue;
        }
        struct FeedCase { const char* name; rknn_input* in; };
        FeedCase feeds[] = {{"uint8 转换", &feed_conv}, {"int8 原生直喂", &feed_native}};

        for (auto& f : feeds) {
            Stats full = measure(ctx, f.in, io_num.n_output, iters, false);
            Stats pure = measure(ctx, f.in, io_num.n_output, iters, true);
            if (full.ok) {
                printf("%-12s %-18s %-10s %8.2f %8.2f %8.2f %8.2f %8.1f\n",
                       c.name, f.name, "全周期", full.mean, full.p50, full.p95, full.p99, full.fps);
            } else {
                printf("%-12s %-18s %-10s %s\n", c.name, f.name, "全周期", "失败");
            }
            if (pure.ok) {
                printf("%-12s %-18s %-10s %8.2f %8.2f %8.2f %8.2f %8.1f\n",
                       c.name, f.name, "纯run", pure.mean, pure.p50, pure.p95, pure.p99, pure.fps);
            } else {
                printf("%-12s %-18s %-10s %s\n", c.name, f.name, "纯run", "失败");
            }
            fflush(stdout);
        }
    }

    rknn_destroy(ctx);
    return 0;
}
