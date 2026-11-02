#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>

// 1. 目标框与包头结构体 (第 3 周 D16/D17)
struct BBox {
    int   class_id;    // 0: 行人, 1: 汽车
    float score;       // 置信度
    float x, y, w, h;  // 坐标
};

struct DetectionHeader {
    uint32_t frame_id;
    uint32_t bbox_count;
};

// 完整的一帧检测产物
struct FrameResult {
    DetectionHeader header;
    BBox boxes[2];
};

// 2. 线程安全推流队列 (第 2 周 D13)
std::queue<FrameResult> stream_queue;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  queue_cond = PTHREAD_COND_INITIALIZER;

int g_client_fd = -1;  // 连接上来的电脑客户端 FD
bool g_running = true;

// ★ NPU 工作线程：模拟 3 核心并发推理，把结果送进推流队列
void* npu_worker(void* arg) {
    int core_id = *(int*)arg;
    for (int f = 1; f <= 6; f++) {
        usleep(300000 + core_id * 50000); // 模拟各核心独立推理耗时

        FrameResult res;
        res.header.frame_id = f;
        res.header.bbox_count = 2;
        // 目标 1: 行人
        res.boxes[0] = {0, 0.95f, 50.0f * f, 100.0f, 60.0f, 130.0f};
        // 目标 2: 汽车
        res.boxes[1] = {1, 0.89f, 200.0f + f * 10, 180.0f, 150.0f, 80.0f};

        // 抢锁塞入推流队列，并按门铃
        pthread_mutex_lock(&queue_lock);
        stream_queue.push(res);
        printf("  [RK3588 NPU Core #%d] 推理完成 Frame #%d -> 塞入推流队列 (待发: %lu)\n", 
               core_id, f, stream_queue.size());
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_lock);
    }
    return NULL;
}

// ★ 网络推流线程：专门负责从队列取数据，向电脑推送
void* network_streamer(void* arg) {
    while (g_running) {
        pthread_mutex_lock(&queue_lock);
        while (stream_queue.empty() && g_running) {
            pthread_cond_wait(&queue_cond, &queue_lock);
        }
        if (!g_running && stream_queue.empty()) {
            pthread_mutex_unlock(&queue_lock);
            break;
        }

        FrameResult res = stream_queue.front();
        stream_queue.pop();
        pthread_mutex_unlock(&queue_lock);

        // 如果电脑连进来了，立刻通过网络推流出去！
        if (g_client_fd >= 0) {
            send(g_client_fd, &res.header, sizeof(res.header), 0);
            send(g_client_fd, res.boxes, sizeof(res.boxes), 0);
            printf("[网络推流器] >>> 已向上位机电脑实时推送 Frame #%d 数据包！\n", res.header.frame_id);
        }
    }
    return NULL;
}

int main() {
    // 1. 初始化 TCP 服务端 (9000 端口)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(9000);
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("====================================================\n");
    printf("   RK3588 EdgeVision 边缘 AI 智能网关系统已就绪！    \n");
    printf("====================================================\n");
    printf("[网关系统] 正在 9000 端口等待上位机电脑接入...\n");

    // 2. 迎宾员等待电脑连接
    g_client_fd = accept(server_fd, NULL, NULL);
    printf("★ [网关系统] 上位机电脑成功连入！立刻启动 NPU 多核推理与推流引擎！\n\n");

    // 3. 启动网络推流专用线程
    pthread_t stream_tid;
    pthread_create(&stream_tid, NULL, network_streamer, NULL);

    // 4. 启动 3 个 NPU 推理 Worker
    pthread_t npu_tids[3];
    int core_ids[3] = {0, 1, 2};
    for (int i = 0; i < 3; i++) {
        pthread_create(&npu_tids[i], NULL, npu_worker, &core_ids[i]);
    }

    // 等待 NPU 全部算完
    for (int i = 0; i < 3; i++) {
        pthread_join(npu_tids[i], NULL);
    }
    sleep(1); // 等最后的数据发完

    g_running = false;
    pthread_cond_broadcast(&queue_cond);
    pthread_join(stream_tid, NULL);

    if (g_client_fd >= 0) close(g_client_fd);
    close(server_fd);
    printf("\n>>> EdgeVision 边缘网关推流大练兵圆满完成！<<<\n");
    return 0;
}
