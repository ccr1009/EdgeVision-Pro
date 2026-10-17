// ============================================================================
//  bench_multicore.cpp —— RK3588 三核 NPU 真并行吞吐测试
//
//  背景: rknn_set_core_mask(RKNN_NPU_CORE_0_1_2) 只是让「同一个 context」在
//        多核上跑, 对单次推理的加速很有限(实测只有 4%)。
//        想让三核真正满载, 必须开 3 个独立 context, 每个绑一颗核, 各跑各的。
//        这个程序就是量化「到底能快多少」。
//
//  板端编译:
//    g++ -O2 -std=c++17 bench_multicore.cpp -I$B/include -L$B/lib -lrknnrt -pthread -o bench_multicore
//  运行:
//    LD_LIBRARY_PATH=$B/lib ./bench_multicore /root/edgevision/data/models/yolov8n_rk3588_i8.rknn 100
// ============================================================================
#include <rknn_api.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <vector>

static std::vector<char> g_blob;
static std::string g_model;
static int g_iters = 100;

static double now_ms() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

struct ThreadArg {
    int core_id;
    rknn_core_mask mask;
    double mean_ms = 0;
    double total_ms = 0;
    bool ok = false;
};

static void* worker(void* p) {
    ThreadArg* a = (ThreadArg*)p;

    rknn_context ctx = 0;
    // 每个线程加载一份模型(独立 context), 才能真正各占一颗核
    std::vector<char> blob = g_blob;
    int ret = rknn_init(&ctx, blob.data(), (uint32_t)blob.size(), 0, nullptr);
    if (ret != RKNN_SUCC) { printf("[Core%d] rknn_init 失败: %d\n", a->core_id, ret); return nullptr; }

    if (rknn_set_core_mask(ctx, a->mask) != RKNN_SUCC) {
        printf("[Core%d] set_core_mask 失败\n", a->core_id);
        return nullptr;
    }

    rknn_tensor_attr attr{};
    attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr));
    size_t nbytes = 1;
    for (uint32_t i = 0; i < attr.n_dims; ++i) nbytes *= attr.dims[i];
    std::vector<uint8_t> buf(nbytes, 114);

    rknn_input in{};
    in.index = 0;
    in.buf = buf.data();
    in.size = (uint32_t)buf.size();
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    in.pass_through = 0;

    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    std::vector<rknn_output> outs(io.n_output);

    auto once = [&](double* t_out) {
        double t0 = now_ms();
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, nullptr);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (auto& o : outs) o.want_float = 0;
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
        rknn_outputs_release(ctx, io.n_output, outs.data());
        if (t_out) *t_out = now_ms() - t0;
    };

    for (int i = 0; i < 20; ++i) once(nullptr);  // 预热

    double t_start = now_ms();
    double sum = 0, one = 0;
    for (int i = 0; i < g_iters; ++i) {
        once(&one);
        sum += one;
    }
    a->total_ms = now_ms() - t_start;
    a->mean_ms = sum / g_iters;
    a->ok = true;

    rknn_destroy(ctx);
    return nullptr;
}

// 单线程基线: 用于计算并行加速比
static double single_baseline(int iters) {
    rknn_context ctx = 0;
    std::vector<char> blob = g_blob;
    if (rknn_init(&ctx, blob.data(), (uint32_t)blob.size(), 0, nullptr) != RKNN_SUCC) return 0;
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_0);

    rknn_tensor_attr attr{};
    attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr));
    size_t nbytes = 1;
    for (uint32_t i = 0; i < attr.n_dims; ++i) nbytes *= attr.dims[i];
    std::vector<uint8_t> buf(nbytes, 114);
    rknn_input in{};
    in.index = 0;
    in.buf = buf.data();
    in.size = (uint32_t)buf.size();
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;

    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    std::vector<rknn_output> outs(io.n_output);

    auto once = [&]() {
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, nullptr);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (auto& o : outs) o.want_float = 0;
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
        rknn_outputs_release(ctx, io.n_output, outs.data());
    };
    for (int i = 0; i < 20; ++i) once();
    double t0 = now_ms();
    for (int i = 0; i < iters; ++i) once();
    double ms = now_ms() - t0;
    rknn_destroy(ctx);
    return ms;
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

int main(int argc, char** argv) {
    g_model = argc > 1 ? argv[1] : "/root/edgevision/data/models/yolov8n_rk3588_i8.rknn";
    g_iters = argc > 2 ? atoi(argv[2]) : 100;

    if (!read_file(g_model.c_str(), g_blob)) return 1;
    printf("模型: %s (%.2f MB), 每线程 %d 次\n\n", g_model.c_str(),
           g_blob.size() / 1024.0 / 1024.0, g_iters);

    printf(">>> 单线程基线 (只绑 Core0) ...\n");
    fflush(stdout);
    double base_ms = single_baseline(g_iters);
    double base_fps = g_iters / (base_ms / 1000.0);
    printf("    单线程: %.2f ms/帧 -> %.1f FPS\n\n", base_ms / g_iters, base_fps);

    printf(">>> 3 线程 / 3 独立 context / 每线程绑一颗核 ...\n");
    fflush(stdout);

    ThreadArg args[3] = {{0, RKNN_NPU_CORE_0}, {1, RKNN_NPU_CORE_1}, {2, RKNN_NPU_CORE_2}};
    pthread_t tids[3];
    double wall0 = now_ms();
    for (int i = 0; i < 3; ++i) pthread_create(&tids[i], nullptr, worker, &args[i]);
    for (int i = 0; i < 3; ++i) pthread_join(tids[i], nullptr);
    double wall = now_ms() - wall0;

    int done = 0;
    for (int i = 0; i < 3; ++i) {
        if (args[i].ok) {
            printf("    Core%d: %.2f ms/帧, 本线程 %d 帧用时 %.0f ms\n",
                   args[i].core_id, args[i].mean_ms, g_iters, args[i].total_ms);
            done += g_iters;
        }
    }

    double agg_fps = done / (wall / 1000.0);
    printf("\n==================================================\n");
    printf("  墙钟总耗时:      %.0f ms\n", wall);
    printf("  完成推理总数:    %d 帧\n", done);
    printf("  三核聚合吞吐:    %.1f FPS\n", agg_fps);
    printf("  并行加速比:      %.2fx (相对单线程 %.1f FPS)\n", agg_fps / base_fps, base_fps);
    printf("  单帧等效延迟:    %.2f ms (3 路并行时每路)\n", 3000.0 / agg_fps);
    printf("==================================================\n");
    return 0;
}
