// ============================================================================
//  edgevision_dashboard_main.cpp —— 车载道路实景 + 3D 感知 + BEV 雷达 实时看板
//
//  2026-10-26 新增。与 edgevision_pro_main.cpp（单张 jpg 离线出图）互补：
//  本程序是**连续视频流**版本，把板端 C++ 全链路直接变成 PC 浏览器可看的看板。
//
//  真实链路（每一帧都是真的，无任何仿真/补框/硬编码偏移）：
//    1. cv::VideoCapture 循环读取 mp4（H.264 软解，实测 5.71 ms/帧 @1080p）
//    2. cv::resize 到推流分辨率（默认 1280x720）
//    3. letterbox 640x640（灰边 114，与 eval_dump.cpp / edgevision_pro 口径一致）
//    4. RKNN NPU 推理（YOLOv8n DFL-Free INT8，真实模型）
//    5. 检测框还原 → 跨类别 IoU 去重 → BEV 地面投影 → 3D 线框 + BEV 雷达
//    6. cv::imencode JPEG → MJPEG over HTTP 推流 + /api/stats 结构化遥测
//
//  明确的**未实现**（不许对外宣传，见根 AGENTS.md）：
//    - H.264/H.265 硬件解码：本程序走 OpenCV 软解，**没有**用 MPP 硬解 AVC
//    - RGA 硬件 CSC：本程序用 OpenCV resize，**没有**用 RGA
//    - DMA-BUF 零拷贝：每帧都有 CPU 内存拷贝
//    - 目标跟踪：没有 ByteTrack/Kalman，每帧独立检测（跨帧不保 ID）
//    - RTSP：只支持本地文件循环
//
//  用法:
//    edgevision_dashboard                          # 默认 8090 端口 + 默认视频
//    edgevision_dashboard --video x.mp4 --port 8090
//    edgevision_dashboard --width 960 --height 540 --quality 75
//  环境变量: EV_MODEL / EV_VIDEO / EV_WEB
// ============================================================================

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "bev_projector.hpp"
#include "bev_render.hpp"
#include "rknn_engine_dfl_free.h"

using namespace edgevision;

// ===========================================================================
//  全局状态
// ===========================================================================
static std::atomic<bool> g_running{true};
static int g_listen_fd = -1;

// 一帧的"成品"：给所有 HTTP 客户端共享的最新帧
struct FrameSlot {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> jpeg;   // 已编码好的整帧图（含 3D 框 + BEV 雷达 + HUD）
    std::string stats_json;      // /api/stats 的完整 JSON 文本
    uint64_t seq = 0;            // 每产出一帧 +1，客户端靠它判断"有没有新帧"
    bool stopped = false;
};
static FrameSlot g_slot;

static std::string g_index_html;                              // 网页（启动时读一次）
static std::string g_index_mime = "text/html; charset=utf-8";

// ===========================================================================
//  小工具
// ===========================================================================
static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if ((unsigned char)c < 0x20) o.push_back(' ');
        else o.push_back(c);
    }
    return o;
}

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    if (sz <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    f.read(&out[0], sz);
    return f.good();
}

static bool send_all(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return false;   // 对端断开 -> 结束该连接
        }
        off += (size_t)n;
    }
    return true;
}

// ===========================================================================
//  HTTP 响应
// ===========================================================================
static std::string http_head(const char* status, const char* ctype, size_t len,
                             const char* extra = "") {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "HTTP/1.1 %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Cache-Control: no-store, no-cache, must-revalidate\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "%s"
             "Connection: close\r\n"
             "\r\n",
             status, ctype, len, extra);
    return std::string(buf);
}

static void send_plain(int fd, const char* status, const char* ctype, const std::string& body) {
    std::string h = http_head(status, ctype, body.size());
    send_all(fd, h.data(), h.size());
    send_all(fd, body.data(), body.size());
}

static void serve_index(int fd) {
    if (g_index_html.empty()) {
        std::string body =
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>EdgeVision</title></head>"
            "<body style=\"background:#0b1120;color:#e2e8f0;font-family:sans-serif;padding:40px\">"
            "<h2>dashboard.html 未找到</h2>"
            "<p>推流服务本身正常，可直接看原始画面："
            "<a style=\"color:#22d3ee\" href=\"/stream\">/stream</a> ，遥测："
            "<a style=\"color:#22d3ee\" href=\"/api/stats\">/api/stats</a></p>"
            "</body></html>";
        send_plain(fd, "200 OK", "text/html; charset=utf-8", body);
        return;
    }
    std::string h = http_head("200 OK", g_index_mime.c_str(), g_index_html.size());
    send_all(fd, h.data(), h.size());
    send_all(fd, g_index_html.data(), g_index_html.size());
}

static void serve_stats(int fd) {
    std::string body;
    {
        std::lock_guard<std::mutex> lk(g_slot.mtx);
        body = g_slot.stats_json;
    }
    if (body.empty()) {
        send_plain(fd, "503 Service Unavailable", "application/json",
                   "{\"error\":\"no frame yet\"}");
        return;
    }
    std::string h = http_head("200 OK", "application/json; charset=utf-8", body.size());
    send_all(fd, h.data(), h.size());
    send_all(fd, body.data(), body.size());
}

static void serve_snapshot(int fd) {
    std::vector<uint8_t> jpeg;
    {
        std::lock_guard<std::mutex> lk(g_slot.mtx);
        jpeg = g_slot.jpeg;
    }
    if (jpeg.empty()) {
        send_plain(fd, "503 Service Unavailable", "text/plain", "no frame yet");
        return;
    }
    std::string h = http_head("200 OK", "image/jpeg", jpeg.size());
    send_all(fd, h.data(), h.size());
    send_all(fd, jpeg.data(), jpeg.size());
}

// MJPEG 推流：只在有新帧（seq 变化）时发一帧
static void serve_stream(int fd) {
    static const char* kHdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (!send_all(fd, kHdr, strlen(kHdr))) return;

    uint64_t last_seq = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> jpeg;
        uint64_t seq = 0;
        {
            std::unique_lock<std::mutex> lk(g_slot.mtx);
            g_slot.cv.wait_for(lk, std::chrono::milliseconds(500), [&] {
                return g_slot.seq != last_seq || g_slot.stopped;
            });
            if (g_slot.stopped) return;
            if (g_slot.seq == last_seq) continue;   // 500ms 无新帧，回头再等（顺便检查 g_running）
            jpeg = g_slot.jpeg;                     // 拷贝最新帧：客户端慢时自动丢中间帧
            seq = g_slot.seq;
        }
        last_seq = seq;
        if (jpeg.empty()) continue;

        char part[192];
        int n = snprintf(part, sizeof(part),
                         "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                         jpeg.size());
        if (!send_all(fd, part, (size_t)n)) return;
        if (!send_all(fd, jpeg.data(), jpeg.size())) return;
        if (!send_all(fd, "\r\n", 2)) return;
    }
}

// ===========================================================================
//  单连接处理（独立线程，避免 /stream 长连接堵住 /api/stats）
// ===========================================================================
struct ConnArg { int fd; };

static void* conn_thread(void* p) {
    ConnArg* a = (ConnArg*)p;
    int fd = a->fd;
    delete a;

    char req[2048];
    ssize_t n = ::recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) { ::close(fd); return nullptr; }
    req[n] = '\0';

    char method[8] = {0}, path[1024] = {0};
    if (sscanf(req, "%7s %1023s", method, path) != 2) {
        ::close(fd);
        return nullptr;
    }
    char* q = strchr(path, '?');
    if (q) *q = '\0';   // 去掉 query string

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_plain(fd, "405 Method Not Allowed", "text/plain", "method not allowed");
    } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        serve_index(fd);
    } else if (strcmp(path, "/api/stats") == 0) {
        serve_stats(fd);
    } else if (strcmp(path, "/snapshot") == 0) {
        serve_snapshot(fd);
    } else if (strcmp(path, "/stream") == 0) {
        serve_stream(fd);   // 长连接
    } else if (strcmp(path, "/favicon.ico") == 0) {
        const char* h = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
        send_all(fd, h, strlen(h));
    } else {
        send_plain(fd, "404 Not Found", "text/plain", "not found");
    }
    ::close(fd);
    return nullptr;
}

static void http_server_thread() {
    while (g_running.load()) {
        sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int fd = ::accept(g_listen_fd, (sockaddr*)&cli, &clen);
        if (fd < 0) {
            if (!g_running.load()) break;
            if (errno == EINTR) continue;
            break;
        }
        // 客户端长期收不动就断开，避免线程永久卡在 send 上
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        ConnArg* a = new ConnArg{fd};
        pthread_t th;
        if (pthread_create(&th, nullptr, conn_thread, a) == 0) {
            pthread_detach(th);
        } else {
            delete a;
            ::close(fd);
        }
    }
}

// ===========================================================================
//  信号：优雅退出
// ===========================================================================
static void on_signal(int) {
    g_running.store(false);
    {
        std::lock_guard<std::mutex> lk(g_slot.mtx);
        g_slot.stopped = true;
    }
    g_slot.cv.notify_all();
    if (g_listen_fd >= 0) ::shutdown(g_listen_fd, SHUT_RDWR);
}

// ===========================================================================
//  检测后处理
// ===========================================================================
static float bbox_iou(const Detection& a, const Detection& b) {
    float x1 = std::max(a.x1, b.x1), y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2), y2 = std::min(a.y2, b.y2);
    float w = std::max(0.0f, x2 - x1), h = std::max(0.0f, y2 - y1);
    float inter = w * h;
    float ua = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return (ua > 1e-6f) ? (inter / ua) : 0.0f;
}

// 跨类别去重：模型自带 NMS 是 class-aware（不同类别互不去重），实测同一辆车会被
// 同时判成 truck / bus / car 三个框（bbox 几乎重合），画出来是"一辆车三种颜色"。
// 这里补一遍跨类别贪心去重，保留分数最高的，并如实统计丢弃数（不隐藏）。
static std::vector<Detection> dedup_cross_class(const std::vector<Detection>& in,
                                                float iou_thr, int* dropped) {
    std::vector<Detection> sorted = in;
    std::sort(sorted.begin(), sorted.end(),
              [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<Detection> keep;
    keep.reserve(sorted.size());
    for (const auto& d : sorted) {
        bool suppressed = false;
        for (const auto& k : keep) {
            if (bbox_iou(d, k) > iou_thr) { suppressed = true; break; }
        }
        if (suppressed) ++(*dropped);
        else keep.push_back(d);
    }
    return keep;
}

// COCO 前 9 类（BEV 尺寸先验只认这几类的名字）
static std::string cls_name(int id) {
    static const char* names[] = {"person", "bicycle", "car", "motorcycle", "airplane",
                                  "bus", "train", "truck", "boat"};
    if (id >= 0 && id <= 8) return names[id];
    return "cls" + std::to_string(id);
}

static bool is_traffic_class(int id) {
    return (id == 0 || id == 1 || id == 2 || id == 3 || id == 5 || id == 7);
}

// ===========================================================================
//  参数
// ===========================================================================
struct Args {
    int port = 8090;
    int out_w = 1280;
    int out_h = 720;
    int quality = 80;
    bool pace = true;
    std::string video;
    std::string model;
    std::string web;
};

static void usage(const char* prog) {
    printf("用法: %s [选项]\n", prog);
    printf("  --port N         HTTP 端口（默认 8090）\n");
    printf("  --video PATH     视频文件或摄像头设备路径 (如 /dev/video83 或 data/videos/highway_traffic.mp4)\n");
    printf("  --cam DEV        摄像头设备节点别名 (等价于 --video /dev/videoXX)\n");
    printf("  --model PATH     RKNN 模型（默认 data/models/yolov8n_dflfree_rk3588_i8.rknn）\n");
    printf("  --web PATH       网页文件（默认 deploy/cpp/web/dashboard.html）\n");
    printf("  --width N        推流宽度（默认 1280）\n");
    printf("  --height N       推流高度（默认 720）\n");
    printf("  --quality N      JPEG 质量 1-100（默认 80）\n");
    printf("  --no-pace        不按源视频帧率节流，能跑多快跑多快\n");
}

// ===========================================================================
//  一帧的绘制结果（bbox 与 3D 投影结果绑在一起，方便按距离排序后仍能画）
// ===========================================================================
struct DrawItem {
    std::string cls;
    float score = 0;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;      // 推流分辨率坐标
    bool has3d = false;
    Box3D box3d;
    float dist_m = 0;
};

// ===========================================================================
//  主程序
// ===========================================================================
int main(int argc, char** argv) {
    Args args;
    const char* env_model = std::getenv("EV_MODEL");
    const char* env_video = std::getenv("EV_VIDEO");
    const char* env_web = std::getenv("EV_WEB");
    args.model = env_model ? env_model : "data/models/yolov8n_dflfree_rk3588_i8.rknn";
    args.video = env_video ? env_video : "data/videos/highway_traffic.mp4";
    args.web = env_web ? env_web : "deploy/cpp/web/dashboard.html";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "[FAIL] %s 需要参数\n", name);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "--port") args.port = std::atoi(next("--port").c_str());
        else if (a == "--video") args.video = next("--video");
        else if (a == "--cam") args.video = next("--cam");
        else if (a == "--model") args.model = next("--model");
        else if (a == "--web") args.web = next("--web");
        else if (a == "--width") args.out_w = std::atoi(next("--width").c_str());
        else if (a == "--height") args.out_h = std::atoi(next("--height").c_str());
        else if (a == "--quality") args.quality = std::atoi(next("--quality").c_str());
        else if (a == "--no-pace") args.pace = false;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { fprintf(stderr, "[FAIL] 未知参数: %s\n", a.c_str()); usage(argv[0]); return 1; }
    }
    if (args.out_w < 320 || args.out_h < 240 || args.out_w > 1920 || args.out_h > 1080) {
        fprintf(stderr, "[FAIL] 分辨率越界: %dx%d\n", args.out_w, args.out_h);
        return 1;
    }
    if (args.quality < 1 || args.quality > 100) {
        fprintf(stderr, "[FAIL] quality 越界: %d\n", args.quality);
        return 1;
    }
    if (args.port < 1024 || args.port > 65535) {
        fprintf(stderr, "[FAIL] 端口越界: %d\n", args.port);
        return 1;
    }

    printf("==================================================================\n");
    printf("EdgeVision Dashboard — 车载道路实景 + 3D 线框 + BEV 雷达 实时看板\n");
    printf("  RK3588 板端原生 C++ | YOLOv8n DFL-Free INT8 | 真实链路，无仿真数据\n");
    printf("==================================================================\n");

    // ---------------- 1. 视频源 / 摄像头 ----------------
    bool is_cam = (args.video.rfind("/dev/video", 0) == 0);
    cv::VideoCapture cap;
    int src_w = 1280, src_h = 720;
    double src_fps = 30.0;
    int src_frames = 0;

    if (is_cam) {
        cap.open(args.video, cv::CAP_V4L2);
        if (!cap.isOpened()) {
            fprintf(stderr, "[FAIL] 无法打开摄像头设备: %s\n", args.video.c_str());
            return 1;
        }
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
        cap.set(cv::CAP_PROP_FPS, 30);

        char cmd[128];
        snprintf(cmd, sizeof(cmd), "v4l2-ctl -d %s --set-ctrl=exposure_auto_priority=0 >/dev/null 2>&1", args.video.c_str());
        system(cmd);

        // 硬件预热 15 帧确保自动曝光收敛
        cv::Mat warm;
        for (int i = 0; i < 15; ++i) {
            cap.read(warm);
        }
        src_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
        src_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        printf("[+] 摄像头就绪: %s\n", args.video.c_str());
        printf("    硬件采集 %dx%d @ 30.0 fps (MJPG 硬件流已锁定)\n", src_w, src_h);
    } else {
        cap.open(args.video);
        if (!cap.isOpened()) {
            fprintf(stderr, "[FAIL] 打不开视频: %s\n", args.video.c_str());
            return 1;
        }
        src_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
        src_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        src_fps = cap.get(cv::CAP_PROP_FPS);
        src_frames = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
        printf("[+] 视频文件: %s\n", args.video.c_str());
        printf("    源 %dx%d @ %.1f fps, %d 帧, 循环播放\n", src_w, src_h, src_fps, src_frames);
    }

    // ---------------- 2. 加载模型 ----------------
    RKNNEngineDflFree npu;
    if (!npu.init(args.model)) {
        fprintf(stderr, "[FAIL] RKNN 模型加载失败: %s\n", args.model.c_str());
        return 1;
    }
    printf("[+] 模型: %s\n", args.model.c_str());

    // ---------------- 3. 网页 ----------------
    if (read_file(args.web, g_index_html)) {
        printf("[+] 网页: %s (%zu bytes)\n", args.web.c_str(), g_index_html.size());
    } else {
        printf("[WARN] 网页文件不存在/为空: %s -> 使用内置兜底页\n", args.web.c_str());
    }

    // ---------------- 4. 起 HTTP 服务 ----------------
    g_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("[FAIL] socket"); return 1; }
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)args.port);
    if (::bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[FAIL] bind %d 失败 (端口被占?): %s\n", args.port, strerror(errno));
        return 1;
    }
    if (::listen(g_listen_fd, 16) < 0) { perror("[FAIL] listen"); return 1; }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::thread http_thread(http_server_thread);
    printf("[+] HTTP 服务已启动: 0.0.0.0:%d\n", args.port);
    printf("    浏览器打开: http://<板子IP>:%d\n", args.port);
    printf("[+] 推流分辨率 %dx%d, JPEG 质量 %d\n", args.out_w, args.out_h, args.quality);
    printf("[NOTICE] 未实现: H.264 硬解 / RGA CSC / DMA-BUF 零拷贝 / 目标跟踪 / RTSP\n");
    printf("------------------------------------------------------------------\n");
    fflush(stdout);

    // ---------------- 5. BEV 几何参数（按推流分辨率自洽标定）----------------
    //  hfov / mount_h / pitch 是同一条标定链路的口径，换分辨率只改画布像素数，
    //  角度与安装高度不变 —— 所以 720p 与 1080p 用同一组角度参数。
    const float hfov = 72.0f;
    const float mount_h = 1.50f;
    const float pitch_deg = 3.5f;
    const float range_fwd = 60.0f;
    const float range_lat = 16.0f;
    const int radar_size = std::max(180, (int)(args.out_h * 0.34f));   // 720p -> 244
    BEVProjector bev(args.out_w, args.out_h, hfov, mount_h, pitch_deg);

    printf("[+] BEVProjector: %dx%d, hfov=%.1f° 安装高=%.2fm 俯仰=%.1f° 前向量程=%.0fm\n",
           args.out_w, args.out_h, hfov, mount_h, pitch_deg, range_fwd);
    printf("[+] BEV 雷达尺寸: %d px\n", radar_size);
    printf("------------------------------------------------------------------\n");
    fflush(stdout);

    // ---------------- 6. 主循环 ----------------
    const double frame_period_ms = (src_fps > 1.0) ? (1000.0 / src_fps) : 33.3;
    double next_deadline = now_ms();

    // 复用的缓冲区（避免每帧 malloc）
    cv::Mat resized, lb_bgr, lb_rgb;
    std::vector<uint8_t> jpeg_buf;
    const std::vector<int> enc_params{cv::IMWRITE_JPEG_QUALITY, args.quality};

    uint64_t frame_id = 0;
    int loop_index = 0;
    int dropped_total = 0;
    const double t_start = now_ms();

    // 上一帧各段耗时（/api/stats 展示，单位 ms）
    double capture_ms = 0, prep_ms = 0, npu_ms = 0, dfl_ms = 0, draw_ms = 0, enc_ms = 0,
           total_ms = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        const double t_frame0 = now_ms();

        // ---- 6.1 读帧（含视频循环）----
        double t0 = now_ms();
        cv::Mat raw;
        if (!cap.read(raw) || raw.empty()) {
            if (is_cam) {
                usleep(5000);
                continue;
            }
            // 读完一圈 -> 回到第 0 帧
            cap.set(cv::CAP_PROP_POS_FRAMES, 0);
            ++loop_index;
            if (!cap.read(raw) || raw.empty()) {
                fprintf(stderr, "[FAIL] 读取视频帧失败，重新回到开头依然失败，退出\n");
                break;
            }
        }
        capture_ms = now_ms() - t0;

        // ---- 6.2 预处理：缩放到推流分辨率 + letterbox 640 ----
        t0 = now_ms();
        if (raw.cols != args.out_w || raw.rows != args.out_h) {
            cv::resize(raw, resized, cv::Size(args.out_w, args.out_h), 0, 0, cv::INTER_LINEAR);
        } else {
            resized = raw;
        }
        const int TARGET = 640;
        const float lb_scale = std::min((float)TARGET / resized.cols, (float)TARGET / resized.rows);
        const int nw = (int)(resized.cols * lb_scale);
        const int nh = (int)(resized.rows * lb_scale);
        const int pad_x = (TARGET - nw) / 2;
        const int pad_y = (TARGET - nh) / 2;
        {
            cv::Mat tmp;
            cv::resize(resized, tmp, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
            if (lb_bgr.rows != TARGET || lb_bgr.cols != TARGET) {
                lb_bgr = cv::Mat(TARGET, TARGET, CV_8UC3, cv::Scalar(114, 114, 114));
            } else {
                lb_bgr.setTo(cv::Scalar(114, 114, 114));
            }
            tmp.copyTo(lb_bgr(cv::Rect(pad_x, pad_y, nw, nh)));
        }
        // RKNN 要 RGB，OpenCV 给 BGR —— 在 640x640 上换通道，比在 720p 上换便宜
        cv::cvtColor(lb_bgr, lb_rgb, cv::COLOR_BGR2RGB);
        prep_ms = now_ms() - t0;

        // ---- 6.3 NPU 推理 ----
        t0 = now_ms();
        std::vector<Detection> raw_dets;
        if (!npu.run_inference(lb_rgb.data, TARGET, TARGET, raw_dets)) {
            fprintf(stderr, "[FAIL] 推理失败, 退出\n");
            break;
        }
        npu_ms = now_ms() - t0;
        dfl_ms = npu.last_decode_ms();

        // ---- 6.4 跨类别去重 -> 还原坐标 -> BEV 投影 ----
        t0 = now_ms();
        int dropped = 0;
        const std::vector<Detection> dets = dedup_cross_class(raw_dets, 0.65f, &dropped);
        dropped_total += dropped;

        std::vector<DrawItem> items;
        items.reserve(dets.size());
        for (const auto& d : dets) {
            if (!is_traffic_class(d.class_id)) continue;
            // letterbox 坐标 -> 推流分辨率坐标
            float x1 = (d.x1 - pad_x) / lb_scale;
            float y1 = (d.y1 - pad_y) / lb_scale;
            float x2 = (d.x2 - pad_x) / lb_scale;
            float y2 = (d.y2 - pad_y) / lb_scale;
            x1 = std::max(0.0f, std::min(x1, (float)resized.cols - 1));
            x2 = std::max(0.0f, std::min(x2, (float)resized.cols - 1));
            y1 = std::max(0.0f, std::min(y1, (float)resized.rows - 1));
            y2 = std::max(0.0f, std::min(y2, (float)resized.rows - 1));
            if (x2 <= x1 + 2 || y2 <= y1 + 2) continue;

            DrawItem it;
            it.cls = cls_name(d.class_id);
            it.score = d.score;
            it.x1 = x1;
            it.y1 = y1;
            it.x2 = x2;
            it.y2 = y2;
            it.has3d = bev.estimate_3d_box(x1, y1, x2, y2, it.cls, it.box3d);
            if (it.has3d) it.dist_m = it.box3d.distance_m;
            items.push_back(it);
        }
        // 按距离升序排（无 3D 的排最后），近距离优先绘制
        std::sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
            if (a.has3d != b.has3d) return a.has3d;
            return a.dist_m < b.dist_m;
        });
        prep_ms += now_ms() - t0;   // 后处理算进 prep，口径在 HUD 上标注清楚

        // ---- 6.5 绘制：3D 框 / 2D 兜底 / BEV 雷达 / HUD ----
        t0 = now_ms();
        cv::Mat canvas = resized.clone();

        std::vector<std::pair<std::string, Box3D>> bev_objects;
        int n3d = 0;
        for (const auto& it : items) {
            const cv::Scalar col = ev_render::class_color(it.cls);
            if (it.has3d) {
                ev_render::draw_3d_box(canvas, it.box3d, col, 2);
                bev_objects.push_back({it.cls, it.box3d});
                ++n3d;
            } else {
                ev_render::draw_2d_fallback(canvas, it.x1, it.y1, it.x2, it.y2, it.cls, it.score, col);
            }
        }

        // BEV 雷达贴右上角（HUD 条下面）
        cv::Mat radar = ev_render::render_bev_radar(radar_size, range_fwd, range_lat, bev_objects);
        const int hud_h = 34;
        ev_render::paste_radar(canvas, radar, 16, 16, hud_h + 6);

        // 顶部 HUD（全部真实测量值）
        {
            cv::rectangle(canvas, cv::Rect(10, 8, canvas.cols - 20, hud_h),
                          cv::Scalar(18, 22, 26), -1);
            cv::rectangle(canvas, cv::Rect(10, 8, canvas.cols - 20, hud_h),
                          cv::Scalar(45, 65, 80), 1);
            char hud[320];
            snprintf(hud, sizeof(hud),
                     "EdgeVision Dashboard | %dx%d | NPU %.1fms + DFL %.2fms | 3D %d/%zu | frame %llu | loop %d",
                     args.out_w, args.out_h, npu_ms, dfl_ms, n3d, items.size(),
                     (unsigned long long)frame_id, loop_index);
            cv::putText(canvas, hud, cv::Point(20, 30), cv::FONT_HERSHEY_SIMPLEX, 0.46,
                        cv::Scalar(0, 255, 200), 1, cv::LINE_AA);
        }
        draw_ms = now_ms() - t0;

        // ---- 6.6 JPEG 编码 ----
        t0 = now_ms();
        jpeg_buf.clear();
        cv::imencode(".jpg", canvas, jpeg_buf, enc_params);
        enc_ms = now_ms() - t0;

        total_ms = now_ms() - t_frame0;
        ++frame_id;

        // ---- 6.7 组 /api/stats JSON（字段与前端契约严格一致）----
        const double uptime_s = (now_ms() - t_start) / 1000.0;
        const double pipeline_fps = (total_ms > 0.01) ? (1000.0 / total_ms) : 0.0;
        const double video_progress_s = (src_fps > 0) ? (frame_id / src_fps) : 0.0;
        (void)video_progress_s;

        std::string js;
        js.reserve(768 + items.size() * 96);
        {
            char head[768];
            snprintf(head, sizeof(head),
                     "{\"frame_id\":%llu,\"loop_index\":%d,\"source_fps\":%.2f,"
                     "\"pipeline_fps\":%.2f,\"capture_ms\":%.2f,\"prep_ms\":%.2f,"
                     "\"npu_ms\":%.2f,\"decode_ms\":%.2f,\"draw_ms\":%.2f,"
                     "\"encode_ms\":%.2f,\"total_ms\":%.2f,"
                     "\"objects_total\":%zu,\"objects_3d\":%d,"
                     "\"dropped_cross_class\":%d,"
                     "\"resolution\":\"%dx%d\",\"video\":\"%s\","
                     "\"uptime_s\":%.1f,\"jpeg_bytes\":%zu,\"dets\":[",
                     (unsigned long long)frame_id, loop_index, src_fps, pipeline_fps,
                     capture_ms, prep_ms, npu_ms, dfl_ms, draw_ms, enc_ms, total_ms,
                     items.size(), n3d, dropped_total, args.out_w, args.out_h,
                     json_escape(args.video).c_str(), uptime_s, jpeg_buf.size());
            js += head;

            int emitted = 0;
            for (const auto& it : items) {
                if (emitted >= 12) break;          // 契约：最多 12 条
                if (!it.has3d) continue;           // 只列有 3D 定位的目标
                char d[192];
                snprintf(d, sizeof(d),
                         "%s{\"cls\":\"%s\",\"score\":%.3f,\"dist_m\":%.2f,"
                         "\"x_m\":%.2f,\"y_m\":%.2f}",
                         emitted ? "," : "", it.cls.c_str(), it.score, it.dist_m,
                         it.box3d.longitudinal_m, it.box3d.lateral_m);
                js += d;
                ++emitted;
            }
            js += "]}";
        }

        // ---- 6.8 发布到 HTTP（同一把锁保证 jpeg 与 stats 是同帧的）----
        {
            std::lock_guard<std::mutex> lk(g_slot.mtx);
            g_slot.jpeg = jpeg_buf;
            g_slot.stats_json = js;
            ++g_slot.seq;
        }
        g_slot.cv.notify_all();

        // ---- 6.9 节流到源视频帧率（跟不上就全速跑，不 sleep）----
        if (!is_cam && args.pace) {
            next_deadline += frame_period_ms;
            const double now = now_ms();
            if (next_deadline > now + 1.0) {
                struct timespec ts;
                const double sleep_ms = next_deadline - now;
                ts.tv_sec = (time_t)(sleep_ms / 1000.0);
                ts.tv_nsec = (long)((sleep_ms - ts.tv_sec * 1000.0) * 1e6);
                nanosleep(&ts, nullptr);
            } else if (now - next_deadline > 500.0) {
                next_deadline = now;   // 落后太多（刚启动/被抢占），重新对齐
            }
        }

        // 每 30 帧打印一行真实分段耗时
        if (frame_id % 30 == 0) {
            printf("[frame %llu] cap %.2f + prep %.2f + npu %.2f (dfl %.2f) + draw %.2f + enc %.2f"
                   " = %.2f ms (%.1f fps) | objs %zu (3d %d) | jpeg %zu B | loop %d\n",
                   (unsigned long long)frame_id, capture_ms, prep_ms, npu_ms, dfl_ms, draw_ms,
                   enc_ms, total_ms, pipeline_fps, items.size(), n3d, jpeg_buf.size(), loop_index);
            fflush(stdout);
        }
    }

    // ---------------- 7. 收尾 ----------------
    {
        std::lock_guard<std::mutex> lk(g_slot.mtx);
        g_slot.stopped = true;
    }
    g_slot.cv.notify_all();
    g_running.store(false);
    if (g_listen_fd >= 0) {
        ::shutdown(g_listen_fd, SHUT_RDWR);
        ::close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (http_thread.joinable()) http_thread.join();
    cap.release();
    npu.release();
    printf("[DONE] 共处理 %llu 帧, 循环 %d 圈, 跨类别去重累计丢弃 %d 框\n",
           (unsigned long long)frame_id, loop_index, dropped_total);
    return 0;
}
