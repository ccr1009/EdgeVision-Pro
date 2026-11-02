// ============================================================
//  d20_gateway_npu.cpp —— D20 真·边缘 AI 推流网关
//
//  和 w3_d20_edge_gateway.cpp 的区别(这是重点):
//    旧版: usleep(300000) 假装推理, 坐标写死成 50*f
//    新版: rknn_run() 真推理, 坐标来自模型输出
//
//  架构(和第 2 周 D13 的积木一样):
//    [3 个 NPU worker]  各自 init 一个 context, 各绑一颗核
//            │  推理结果
//            ▼
//    [有界队列 max 32]  ← 满了丢最旧的帧(实时系统就该这样)
//            │
//            ▼
//    [推流线程]  send() 定长包头 + BBox 结构体  ──► TCP:9000 ──► 电脑
//
//  编译:
//    B=/root/edgevision/deploy/cpp/3rdparty/rknn_api
//    g++ -O2 -std=c++17 d20_gateway_npu.cpp \
//        -I$B/include -L$B/lib -lrknnrt -pthread -o d20_gateway_npu
//  运行:
//    LD_LIBRARY_PATH=$B/lib ./d20_gateway_npu
// ============================================================
#include <rknn_api.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <pthread.h>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define MODEL "/root/edgevision/data/models/yolov8n_rk3588_i8.rknn"
#define PORT 9000
#define QUEUE_MAX 32          // 有界队列: 网络卡顿时防止内存无限增长
#define INFER_PER_WORKER 100  // 每个 worker 推多少帧后收工

// ---------------- 传输协议(与电脑端 Python 对齐) ----------------
#pragma pack(push, 1)
struct BBox {
    int32_t class_id;  // 0: 行人, 1: 汽车
    float   score;
    float   x, y, w, h;
};
struct DetectionHeader {
    uint32_t frame_id;
    uint32_t bbox_count;
};
struct FramePacket {              // 一帧完整的数据包
    DetectionHeader header;
    BBox boxes[8];                // 最多 8 个目标
};
#pragma pack(pop)

// ---------------- 全局状态 ----------------
static std::deque<FramePacket> g_queue;       // 有界队列
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;

static std::atomic<bool> g_running{true};
static std::atomic<uint32_t> g_frame_id{0};
static std::atomic<uint64_t> g_dropped{0};    // 因队列满被丢弃的帧数

static int   g_client_fd = -1;
static std::vector<char> g_blob;              // 模型权重

// ---------------- 工具 ----------------
static double now_ms() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

// 把 RKNN 输出的 float 张量(1x84x8400)解码成 BBox 列表
// 84 = 4(框) + 80(类别); 8400 = 80x80 + 40x40 + 20x20
static void decode_yolov8(const float* data, int num_anchors, int num_classes,
                          int img_w, int img_h, float conf_th,
                          std::vector<BBox>& out, uint32_t max_out) {
    for (int a = 0; a < num_anchors && out.size() < max_out; ++a) {
        const float* p = data + a;
        int best_cls = -1;
        float best_score = 0.0f;
        for (int c = 0; c < num_classes; ++c) {
            float s = p[(4 + c) * num_anchors];
            if (s > best_score) { best_score = s; best_cls = c; }
        }
        if (best_score < conf_th) continue;

        float cx = p[0 * num_anchors];
        float cy = p[1 * num_anchors];
        float w  = p[2 * num_anchors];
        float h  = p[3 * num_anchors];

        BBox b;
        b.class_id = best_cls;
        b.score    = best_score;
        b.w        = w;
        b.h        = h;
        b.x        = cx - w * 0.5f;
        b.y        = cy - h * 0.5f;
        (void)img_w; (void)img_h;
        out.push_back(b);
    }
}

// ---------------- NPU 工作线程 ----------------
struct WorkerArg {
    int     core_id;
    int64_t frames_done = 0;
    double  total_ms    = 0.0;
};

static void* npu_worker(void* p) {
    WorkerArg* a = static_cast<WorkerArg*>(p);

    // 每个线程一份独立 context —— 不能共用
    std::vector<char> blob = g_blob;
    rknn_context ctx = 0;
    if (rknn_init(&ctx, blob.data(), (uint32_t)blob.size(), 0, nullptr) != RKNN_SUCC) {
        printf("[Core%d] rknn_init 失败\n", a->core_id);
        return nullptr;
    }

    rknn_core_mask mask = (a->core_id == 0) ? RKNN_NPU_CORE_0
                        : (a->core_id == 1) ? RKNN_NPU_CORE_1
                                            : RKNN_NPU_CORE_2;
    if (rknn_set_core_mask(ctx, mask) != RKNN_SUCC) {
        printf("[Core%d] set_core_mask 失败\n", a->core_id);
        rknn_destroy(ctx);
        return nullptr;
    }

    rknn_tensor_attr in_attr{};
    in_attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    size_t nbytes = 1;
    for (uint32_t i = 0; i < in_attr.n_dims; ++i) nbytes *= in_attr.dims[i];

    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));

    std::vector<uint8_t> img(nbytes, 114);
    rknn_input in{};
    in.index = 0;
    in.buf = img.data();
    in.size = (uint32_t)img.size();
    in.type = RKNN_TENSOR_UINT8;
    in.fmt  = RKNN_TENSOR_NHWC;
    in.pass_through = 0;

    std::vector<rknn_output> outs(io.n_output);
    std::vector<float> flat;

    // 预热
    for (int i = 0; i < 10; ++i) {
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, nullptr);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (auto& o : outs) o.want_float = 1;
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
        rknn_outputs_release(ctx, io.n_output, outs.data());
    }

    printf("[Core%d] 就绪, 开始推理\n", a->core_id);

    for (int f = 0; f < INFER_PER_WORKER && g_running; ++f) {
        double t0 = now_ms();

        // ---- 真推理 ----
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, nullptr);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (auto& o : outs) o.want_float = 1;
        if (rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr) != RKNN_SUCC) continue;

        // ---- 解码成 BBox ----
        std::vector<BBox> boxes;
        flat.assign((float*)outs[0].buf, (float*)outs[0].buf + 84 * 8400);
        decode_yolov8(flat.data(), 8400, 80, 640, 640, 0.25f, boxes, 8);
        rknn_outputs_release(ctx, io.n_output, outs.data());

        double cost = now_ms() - t0;
        a->frames_done++;
        a->total_ms += cost;

        // ---- 打包 ----
        FramePacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.frame_id   = ++g_frame_id;
        pkt.header.bbox_count = (uint32_t)boxes.size();
        for (size_t i = 0; i < boxes.size() && i < 8; ++i) pkt.boxes[i] = boxes[i];

        // ---- 入队(有界, 满了丢最旧的) ----
        pthread_mutex_lock(&g_lock);
        if (g_queue.size() >= QUEUE_MAX) {
            g_queue.pop_front();
            g_dropped++;
        }
        g_queue.push_back(pkt);
        pthread_cond_signal(&g_cond);
        pthread_mutex_unlock(&g_lock);
    }

    printf("[Core%d] 收工: %lld 帧, 平均 %.2f ms/帧\n",
           a->core_id, (long long)a->frames_done,
           a->frames_done ? a->total_ms / a->frames_done : 0.0);

    rknn_destroy(ctx);
    return nullptr;
}

// ---------------- 推流线程 ----------------
static void* network_streamer(void* p) {
    while (true) {
        FramePacket pkt;
        pthread_mutex_lock(&g_lock);
        while (g_queue.empty() && g_running) {
            pthread_cond_wait(&g_cond, &g_lock);
        }
        if (g_queue.empty() && !g_running) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        pkt = g_queue.front();
        g_queue.pop_front();
        pthread_mutex_unlock(&g_lock);

        if (g_client_fd < 0) continue;

        // 定长包头 + 包体, 两次 send 都检查返回值
        const char* hp = (const char*)&pkt.header;
        size_t hlen = sizeof(pkt.header);
        while (hlen > 0) {
            ssize_t n = send(g_client_fd, hp, hlen, MSG_NOSIGNAL);
            if (n <= 0) { printf("[推流] send 失败, 断开客户端\n"); close(g_client_fd); g_client_fd = -1; break; }
            hp += n; hlen -= (size_t)n;
        }
        if (g_client_fd < 0) continue;

        const char* bp = (const char*)pkt.boxes;
        size_t blen = sizeof(BBox) * pkt.header.bbox_count;
        while (blen > 0) {
            ssize_t n = send(g_client_fd, bp, blen, MSG_NOSIGNAL);
            if (n <= 0) { printf("[推流] send 失败, 断开客户端\n"); close(g_client_fd); g_client_fd = -1; break; }
            bp += n; blen -= (size_t)n;
        }
    }
    return nullptr;
}

int main() {
    FILE* fp = fopen(MODEL, "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    g_blob.resize((size_t)sz);
    if (fread(g_blob.data(), 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); return 1; }
    fclose(fp);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    listen(server_fd, 5);

    printf("==================================================\n");
    printf("  RK3588 EdgeVision 真·NPU 推流网关\n");
    printf("  模型: %s (%.2f MB)\n", MODEL, g_blob.size() / 1024.0 / 1024.0);
    printf("  队列: 有界 %d, 满了丢最旧\n", QUEUE_MAX);
    printf("==================================================\n");
    printf("等待电脑连接 %d 端口...\n", PORT);
    fflush(stdout);

    g_client_fd = accept(server_fd, nullptr, nullptr);
    if (g_client_fd < 0) { perror("accept"); return 1; }
    printf("★ 电脑已连入, 启动 3 核 NPU 推理 + 推流\n\n");

    pthread_t stream_tid;
    pthread_create(&stream_tid, nullptr, network_streamer, nullptr);

    WorkerArg args[3] = {{0}, {1}, {2}};
    pthread_t tids[3];
    double wall0 = now_ms();
    for (int i = 0; i < 3; ++i) pthread_create(&tids[i], nullptr, npu_worker, &args[i]);
    for (int i = 0; i < 3; ++i) pthread_join(tids[i], nullptr);
    double wall_ms = now_ms() - wall0;

    // 等队列排空
    while (true) {
        pthread_mutex_lock(&g_lock);
        bool empty = g_queue.empty();
        pthread_mutex_unlock(&g_lock);
        if (empty) break;
        usleep(10000);
    }

    g_running = false;
    pthread_cond_broadcast(&g_cond);
    pthread_join(stream_tid, nullptr);

    int total = 0;
    for (auto& a : args) total += (int)a.frames_done;
    printf("\n==================================================\n");
    for (auto& a : args)
        printf("  Core%d: %lld 帧, %.2f ms/帧\n", a.core_id,
               (long long)a.frames_done, a.frames_done ? a.total_ms / a.frames_done : 0.0);
    printf("  总帧数:   %d\n", total);
    printf("  墙钟:     %.0f ms\n", wall_ms);
    printf("  聚合吞吐: %.1f FPS\n", total / (wall_ms / 1000.0));
    printf("  队列丢弃: %llu 帧\n", (unsigned long long)g_dropped.load());
    printf("==================================================\n");

    if (g_client_fd >= 0) close(g_client_fd);
    close(server_fd);
    return 0;
}
