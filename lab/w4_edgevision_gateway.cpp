#include <iostream>
#include <vector>
#include <queue>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <opencv2/opencv.hpp>
#include <rknn_api.h>
#include "rknn_engine_dfl_free.h"
#include "postprocess.h"

#define PORT 9000
#define MODEL_PATH "/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn"
#define MAX_BBOXES 10

// -------------------------------------------------------------
// 1. 网络推流协议结构体 (与上位机 Python 严格对齐)
// -------------------------------------------------------------
#pragma pack(push, 1)
struct BBox {
    int32_t class_id;    // 0~79: COCO 80 类
    float   score;       // 置信度
    float   x, y, w, h;  // 还原到摄像头原始画面的像素坐标 (如 1280x720)
};

struct DetectionHeader {
    uint32_t frame_id;
    uint32_t bbox_count; // 实际检出目标数 (0 ~ MAX_BBOXES)
};

struct FrameResult {
    DetectionHeader header;         // 8 字节
    BBox boxes[MAX_BBOXES];         // 10 * 24 = 240 字节，总包长 248 字节
};
#pragma pack(pop)

// -------------------------------------------------------------
// 2. 图像帧缓冲区结构 (携带 letterbox 几何逆变换元数据)
// -------------------------------------------------------------
struct RawFrame {
    uint32_t frame_id;
    int orig_w;
    int orig_h;
    float scale;
    int pad_x;
    int pad_y;
    std::vector<uint8_t> rgb_data; // 640x640x3
};

// -------------------------------------------------------------
// 3. 全局共享资源与线程安全队列
// -------------------------------------------------------------
static volatile sig_atomic_t g_running = 1;

// 待推理图像队列（削峰填谷）
static std::queue<RawFrame> g_raw_queue;
static pthread_mutex_t g_raw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_raw_cond = PTHREAD_COND_INITIALIZER;

// 推流结果队列
static std::queue<FrameResult> g_stream_queue;
static pthread_mutex_t g_stream_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_stream_cond = PTHREAD_COND_INITIALIZER;

// 共享的 RKNN 模型二进制数据
static std::vector<char> g_model_data;

// 客户端 Socket FD (-1 表示无客户端连入)
static int g_client_fd = -1;
static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;

// -------------------------------------------------------------
// 4. 辅助函数：信号处理、模型加载与 letterbox 预处理
// -------------------------------------------------------------
void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
        pthread_cond_broadcast(&g_raw_cond);
        pthread_cond_broadcast(&g_stream_cond);
    }
}

bool load_model_file(const char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_model_data.resize(size);
    size_t read_bytes = fread(g_model_data.data(), 1, size, fp);
    fclose(fp);
    return read_bytes == size;
}

// letterbox：等比缩放 + 灰边(114)填充，返回缩放比与坐标偏移
static void letterbox(const cv::Mat& src, cv::Mat& dst, int target_size,
                      float& scale, int& pad_x, int& pad_y) 
{
    const int w = src.cols;
    const int h = src.rows;
    scale = std::min((float)target_size / w, (float)target_size / h);
    const int nw = (int)(w * scale);
    const int nh = (int)(h * scale);

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);

    dst = cv::Mat(target_size, target_size, src.type(), cv::Scalar(114, 114, 114));
    pad_x = (target_size - nw) / 2;
    pad_y = (target_size - nh) / 2;
    resized.copyTo(dst(cv::Rect(pad_x, pad_y, nw, nh)));
}

// -------------------------------------------------------------
// 5. 摄像头生产者线程（OpenCV V4L2 取流 + letterbox 预处理）
// -------------------------------------------------------------
void* camera_producer_thread(void* arg) {
    const char* dev_path = (const char*)arg;
    std::cout << "[CamThread] 正在打开摄像头: " << dev_path << " ...\n";

    cv::VideoCapture cap(dev_path, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::cerr << "[-] [CamThread] 无法打开摄像头 " << dev_path << "，请检查设备节点！\n";
        return nullptr;
    }

    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
    cap.set(cv::CAP_PROP_FPS, 30); // 显式请求 30 FPS，打破 V4L2 默认 20 FPS 锁帧限制

    // 硬件级优化：关闭自动曝光优先降帧，锁定 30 FPS 恒定输出
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "v4l2-ctl -d %s --set-ctrl=exposure_auto_priority=0 >/dev/null 2>&1", dev_path);
    system(cmd);

    // 硬件预热：丢弃前 15 帧，确保自动曝光 (AE) 充分收敛
    std::cout << "[CamThread] 摄像头已开启，正在预热 15 帧自动曝光...\n";
    cv::Mat warm_mat;
    for (int i = 0; i < 15; ++i) {
        cap >> warm_mat;
    }
    std::cout << "[CamThread] 摄像头预热完成，开始实时抓拍推流！\n";

    uint32_t frame_id = 0;
    cv::Mat bgr_mat, lb_mat, rgb_mat;

    while (g_running) {
        cap >> bgr_mat;
        if (bgr_mat.empty()) {
            usleep(5000);
            continue;
        }

        frame_id++;

        // 标准 letterbox 预处理并转换为 RGB
        float scale = 1.0f;
        int pad_x = 0, pad_y = 0;
        letterbox(bgr_mat, lb_mat, 640, scale, pad_x, pad_y);
        cv::cvtColor(lb_mat, rgb_mat, cv::COLOR_BGR2RGB);

        RawFrame raw_frame;
        raw_frame.frame_id = frame_id;
        raw_frame.orig_w   = bgr_mat.cols;
        raw_frame.orig_h   = bgr_mat.rows;
        raw_frame.scale    = scale;
        raw_frame.pad_x    = pad_x;
        raw_frame.pad_y    = pad_y;
        raw_frame.rgb_data.assign(rgb_mat.data, rgb_mat.data + (640 * 640 * 3));

        // 压入推理队列（防积压：若队列已有 2 帧积压，丢弃旧帧保持极低延迟）
        pthread_mutex_lock(&g_raw_lock);
        while (g_raw_queue.size() >= 2) {
            g_raw_queue.pop();
        }
        g_raw_queue.push(std::move(raw_frame));
        pthread_cond_signal(&g_raw_cond);
        pthread_mutex_unlock(&g_raw_lock);

        // 注意：cap >> 本身已阻塞在硬件 V4L2 帧中断上（33.3ms 天然节拍器），切勿额外 usleep 拖慢帧率
        // 原 usleep(15000) 已移除
    }

    cap.release();
    std::cout << "[CamThread] 摄像头资源已优雅释放。\n";
    return nullptr;
}

// -------------------------------------------------------------
// 6. NPU 3核并发工作线程
// -------------------------------------------------------------
void* npu_worker_thread(void* arg) {
    int core_id = *(int*)arg;
    const NpuCore core = (core_id == 0) ? NpuCore::Core0
                       : (core_id == 1) ? NpuCore::Core1 : NpuCore::Core2;

    RKNNEngineDflFree engine;
    if (!engine.init_from_memory(g_model_data.data(), g_model_data.size(), core)) {
        std::cerr << "[-] [NPU Core #" << core_id << "] 引擎初始化失败!\n";
        return nullptr;
    }
    std::cout << "[NPU Core #" << core_id << "] 引擎就绪(物理核硬绑定)\n";

    while (g_running) {
        RawFrame frame;
        pthread_mutex_lock(&g_raw_lock);
        while (g_raw_queue.empty() && g_running)
            pthread_cond_wait(&g_raw_cond, &g_raw_lock);
        if (!g_running && g_raw_queue.empty()) { pthread_mutex_unlock(&g_raw_lock); break; }
        frame = std::move(g_raw_queue.front());
        g_raw_queue.pop();
        pthread_mutex_unlock(&g_raw_lock);

        std::vector<Detection> dets;
        auto t0 = std::chrono::steady_clock::now();
        engine.run_inference(frame.rgb_data.data(), 640, 640, dets);
        auto t1 = std::chrono::steady_clock::now();
        double latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // 构造检测结果帧并做逆变换还原原图真实像素坐标
        FrameResult res{};
        res.header.frame_id   = frame.frame_id;
        uint32_t valid_count  = std::min((uint32_t)dets.size(), (uint32_t)MAX_BBOXES);
        res.header.bbox_count = valid_count;

        for (uint32_t i = 0; i < valid_count; ++i) {
            res.boxes[i].class_id = dets[i].class_id;
            res.boxes[i].score    = dets[i].score;

            // 几何逆变换：(640坐标 - pad) / scale
            float rx1 = (dets[i].x1 - frame.pad_x) / frame.scale;
            float ry1 = (dets[i].y1 - frame.pad_y) / frame.scale;
            float rx2 = (dets[i].x2 - frame.pad_x) / frame.scale;
            float ry2 = (dets[i].y2 - frame.pad_y) / frame.scale;

            // 边缘合法性保护，防止超出 1280x720 原始画面边界
            rx1 = std::max(0.0f, std::min(rx1, (float)frame.orig_w - 1.0f));
            rx2 = std::max(0.0f, std::min(rx2, (float)frame.orig_w - 1.0f));
            ry1 = std::max(0.0f, std::min(ry1, (float)frame.orig_h - 1.0f));
            ry2 = std::max(0.0f, std::min(ry2, (float)frame.orig_h - 1.0f));

            res.boxes[i].x = rx1;
            res.boxes[i].y = ry1;
            res.boxes[i].w = rx2 - rx1;
            res.boxes[i].h = ry2 - ry1;
        }

        pthread_mutex_lock(&g_stream_lock);
        // 防积压：结果队列最多保留 2 帧，若上位机消费慢或断开，果断丢弃旧帧保实时性
        while (g_stream_queue.size() >= 2) {
            g_stream_queue.pop();
        }
        g_stream_queue.push(res);
        pthread_cond_signal(&g_stream_cond);
        pthread_mutex_unlock(&g_stream_lock);

        if (frame.frame_id % 15 == 0)
            std::cout << "  [NPU Core #" << core_id << "] " << latency_ms
                      << " ms | 帧 #" << frame.frame_id << " | 检出 " << dets.size() << " 目标\n";
    }
    std::cout << "[NPU Core #" << core_id << "] 引擎已释放\n";
    return nullptr;
}

// -------------------------------------------------------------
// 7. TCP 网络推流服务线程
// -------------------------------------------------------------
void* tcp_streamer_thread(void* arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    std::cout << "[TCP Server] 推流网关已在端口 " << PORT << " 监听就绪！\n";

    // 迎宾子循环（支持重连）
    while (g_running) {
        timeval tv{1, 0}; // 1秒超时，便于检测 g_running
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        int sel = select(server_fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int c_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (c_fd < 0) continue;

        std::cout << "\n[+] [上位机连入] 电脑客户端已成功建立 TCP 推流通道！\n";

        // 连入瞬间清空历史积压帧，保证上位机收到的第一帧就是最新鲜的实时帧
        pthread_mutex_lock(&g_stream_lock);
        while (!g_stream_queue.empty()) {
            g_stream_queue.pop();
        }
        pthread_mutex_unlock(&g_stream_lock);

        pthread_mutex_lock(&g_client_lock);
        g_client_fd = c_fd;
        pthread_mutex_unlock(&g_client_lock);

        // 数据发送子循环
        while (g_running) {
            FrameResult result;

            pthread_mutex_lock(&g_stream_lock);
            while (g_stream_queue.empty() && g_running && g_client_fd != -1) {
                pthread_cond_wait(&g_stream_cond, &g_stream_lock);
            }

            if (!g_running || g_client_fd == -1) {
                pthread_mutex_unlock(&g_stream_lock);
                break;
            }

            result = g_stream_queue.front();
            g_stream_queue.pop();
            pthread_mutex_unlock(&g_stream_lock);

            // 定长直推定长结构体 (248 字节)
            ssize_t bytes_sent = send(c_fd, &result, sizeof(result), MSG_NOSIGNAL);
            if (bytes_sent <= 0) {
                std::cerr << "[-] [TCP Server] 上位机断开连接。\n";
                break;
            }
        }

        close(c_fd);
        pthread_mutex_lock(&g_client_lock);
        g_client_fd = -1;
        pthread_mutex_unlock(&g_client_lock);
    }

    close(server_fd);
    std::cout << "[TCP Server] 网络推流服务已关闭。\n";
    return nullptr;
}

// -------------------------------------------------------------
// 8. 主程序：资源编排与生命周期管理
// -------------------------------------------------------------
int main(int argc, char* argv[]) {
    const char* dev_path = (argc > 1) ? argv[1] : "/dev/video81";

    // 屏蔽 SIGPIPE，防止客户端断开引发主程序异常闪退
    signal(SIGPIPE, SIG_IGN);

    // 注册系统退出信号，实现平滑优雅退出
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::cout << "=========================================================\n";
    std::cout << " 🚀 EdgeVision 工业级边缘推理网关 V2.0 (DFL-Free + 3核硬绑定)\n";
    std::cout << " 硬件平台: Rockchip RK3588 | 模型: YOLOv8n INT8\n";
    std::cout << " 协议规格: 8字节包头 + 10x24字节BBox = 248字节定长帧\n";
    std::cout << "=========================================================\n";

    // 1. 预加载 RKNN 二进制模型到共享内存
    std::cout << "[System] 正在加载 NPU 模型: " << MODEL_PATH << " ...\n";
    if (!load_model_file(MODEL_PATH)) {
        std::cerr << "[-] 无法加载模型文件，请检查路径！\n";
        return -1;
    }
    std::cout << "[+] 模型加载成功，大小: " << g_model_data.size() / 1024 / 1024 << " MB\n";

    // 2. 启动 TCP 推流服务端线程
    pthread_t streamer_tid;
    pthread_create(&streamer_tid, nullptr, tcp_streamer_thread, nullptr);

    // 3. 启动 NPU 3核并发工作线程
    pthread_t worker_tids[3];
    int core_ids[3] = {0, 1, 2};
    for (int i = 0; i < 3; ++i) {
        pthread_create(&worker_tids[i], nullptr, npu_worker_thread, &core_ids[i]);
    }

    // 4. 启动摄像头生产者线程
    pthread_t cam_tid;
    pthread_create(&cam_tid, nullptr, camera_producer_thread, (void*)dev_path);

    // 等待所有线程有序退出
    pthread_join(cam_tid, nullptr);
    for (int i = 0; i < 3; ++i) {
        pthread_join(worker_tids[i], nullptr);
    }
    pthread_join(streamer_tid, nullptr);

    std::cout << "=========================================================\n";
    std::cout << "[System] 全链路网关安全关闭，所有硬件/显存/网络已优雅归位！\n";
    std::cout << "=========================================================\n";

    return 0;
}
