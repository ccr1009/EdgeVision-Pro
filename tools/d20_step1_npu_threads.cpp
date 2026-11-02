// ============================================================
//  d20_step1_npu_threads.cpp
//
//  D20 第 1 步: 3 个线程, 各绑一颗 NPU 核, 真并行推理
//
//  和第 0 步的区别:
//    第 0 步  usleep() 假装推理      —— 假活
//    第 1 步  rknn_run() 真推理      —— 真活
//
//  编译:
//    B=/root/edgevision/deploy/cpp/3rdparty/rknn_api
//    g++ -O2 -std=c++17 d20_step1_npu_threads.cpp \
//        -I$B/include -L$B/lib -lrknnrt -pthread -o d20_step1
//  运行:
//    LD_LIBRARY_PATH=$B/lib ./d20_step1
// ============================================================
#include <rknn_api.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <vector>

#define MODEL "/root/edgevision/data/models/yolov8n_rk3588_i8.rknn"
#define ITERS 50

// 模型权重整个读进内存, 每个线程各自拷贝一份给 rknn_init
static std::vector<char> g_blob;

static double now_ms() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

// ------------------------------------------------------------
//  每个线程的入参: 自己负责哪颗核
// ------------------------------------------------------------
struct WorkerArg {
    int    core_id;      // 0 / 1 / 2
    double mean_ms;      // 本线程平均单帧耗时
    bool   ok;           // 是否成功
};

// ------------------------------------------------------------
//  ★ 核心: 每个线程自己 init 一个 context, 自己绑核
//     一个 rknn_context 不能被多个线程共用, 所以必须各建各的
// ------------------------------------------------------------
static void* npu_worker(void* p) {
    WorkerArg* a = static_cast<WorkerArg*>(p);

    // 1) 拷一份模型数据(每个 context 独立持有)
    std::vector<char> blob = g_blob;

    // 2) 建上下文
    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, blob.data(), (uint32_t)blob.size(), 0, nullptr);
    if (ret != RKNN_SUCC) {
        printf("  [Core%d] rknn_init 失败: %d\n", a->core_id, ret);
        return nullptr;
    }

    // 3) 绑核 —— 必须在 init 之后
    rknn_core_mask mask;
    switch (a->core_id) {
        case 0:  mask = RKNN_NPU_CORE_0; break;
        case 1:  mask = RKNN_NPU_CORE_1; break;
        default: mask = RKNN_NPU_CORE_2; break;
    }
    if (rknn_set_core_mask(ctx, mask) != RKNN_SUCC) {
        printf("  [Core%d] set_core_mask 失败\n", a->core_id);
        rknn_destroy(ctx);
        return nullptr;
    }

    // 4) 问模型: 输入多大、几个输出
    rknn_tensor_attr in_attr{};
    in_attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));

    size_t nbytes = 1;
    for (uint32_t i = 0; i < in_attr.n_dims; ++i) nbytes *= in_attr.dims[i];

    rknn_input_output_num io_num{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));

    // 5) 准备一帧假图(114 = YOLO 常用灰底)
    std::vector<uint8_t> img(nbytes, 114);

    rknn_input in{};
    in.index        = 0;
    in.buf          = img.data();
    in.size         = (uint32_t)img.size();
    in.type         = RKNN_TENSOR_UINT8;
    in.fmt          = RKNN_TENSOR_NHWC;
    in.pass_through = 0;

    std::vector<rknn_output> outs(io_num.n_output);

    // 6) 一次性推理的封装
    auto infer_once = [&](double* cost_ms) {
        double t0 = now_ms();
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, nullptr);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (auto& o : outs) o.want_float = 0;
        rknn_outputs_get(ctx, io_num.n_output, outs.data(), nullptr);
        rknn_outputs_release(ctx, io_num.n_output, outs.data());
        if (cost_ms) *cost_ms = now_ms() - t0;
    };

    // 7) 预热 20 次(不计时) —— 第一次会建内部内存池, 算进去会虚高
    for (int i = 0; i < 20; ++i) infer_once(nullptr);

    // 8) 正式计时
    double sum = 0.0, one = 0.0;
    for (int i = 0; i < ITERS; ++i) {
        infer_once(&one);
        sum += one;
    }

    a->mean_ms = sum / ITERS;
    a->ok      = true;

    rknn_destroy(ctx);
    return nullptr;
}

int main() {
    // 读模型
    FILE* fp = fopen(MODEL, "rb");
    if (!fp) { perror("fopen 模型"); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_blob.resize((size_t)sz);
    if (fread(g_blob.data(), 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); return 1; }
    fclose(fp);

    printf("模型: %s (%.2f MB)\n", MODEL, g_blob.size() / 1024.0 / 1024.0);
    printf("每线程推理 %d 次, 共 3 线程\n\n", ITERS);

    // ★ 注意这里: core_ids 是独立数组, 不能把循环变量 i 的地址传进去
    int core_ids[3] = {0, 1, 2};
    WorkerArg args[3] = {};
    pthread_t tids[3];

    double wall0 = now_ms();
    for (int i = 0; i < 3; ++i) {
        args[i].core_id = core_ids[i];
        pthread_create(&tids[i], nullptr, npu_worker, &args[i]);
    }
    for (int i = 0; i < 3; ++i) pthread_join(tids[i], nullptr);
    double wall_ms = now_ms() - wall0;

    // 汇总
    int total = 0;
    printf("--------------------------------------------------\n");
    for (int i = 0; i < 3; ++i) {
        if (args[i].ok) {
            printf("  Core%d: %6.2f ms/帧\n", args[i].core_id, args[i].mean_ms);
            total += ITERS;
        } else {
            printf("  Core%d: 失败\n", args[i].core_id);
        }
    }
    printf("--------------------------------------------------\n");
    printf("  墙钟总耗时:   %.0f ms   (串行应为 %.0f ms)\n",
           wall_ms, total ? args[0].mean_ms * ITERS * 3 : 0.0);
    printf("  完成帧数:     %d\n", total);
    printf("  聚合吞吐:     %.1f FPS\n", total / (wall_ms / 1000.0));
    printf("  并行加速比:   %.2fx\n", (args[0].mean_ms * total) / wall_ms);
    printf("==================================================\n");
    return 0;
}
