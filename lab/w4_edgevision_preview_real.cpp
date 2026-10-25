// ============================================================================
//  w4_edgevision_preview_real.cpp —— 实时预览（真实检测框版）
//
//  与 w4_edgevision_preview.cpp 的区别：
//    1. 用 DFL-Free 新模型（yolov8n_dflfree_rk3588_i8.rknn），真跑 decode
//    2. 画的是**真实检测框**，不再是跟着 frame_id 移动的假框
//    3. 预处理改成 letterbox（等比缩放 + 灰边填充），与训练/导出保持一致，
//       框位置不会因为拉伸变形而偏
//
//  浏览器打开: http://<板子IP>:8080
//  板端编译:
//    g++ -O2 -std=c++17 w4_edgevision_preview_real.cpp postprocess_dfl_free.cpp \
//        -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include -lrknnrt \
//        `pkg-config --cflags --libs opencv4` -o w4_edgevision_preview_real
// ============================================================================
#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <pthread.h>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <opencv2/opencv.hpp>
#include <rknn_api.h>

#include "postprocess_dfl_free.h"

#define HTTP_PORT 8080
#define MODEL_PATH "/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn"

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

static volatile sig_atomic_t g_running = 1;
static std::vector<uint8_t> g_latest_jpeg;
static pthread_mutex_t g_jpeg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_jpeg_cond = PTHREAD_COND_INITIALIZER;
static std::vector<char> g_model_data;

static void handle_signal(int) { g_running = 0; }

static bool load_model_file(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { perror("fopen model"); return false; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_model_data.resize((size_t)sz);
    bool ok = fread(g_model_data.data(), 1, (size_t)sz, fp) == (size_t)sz;
    fclose(fp);
    printf("[+] 模型 %s (%.2f MB)\n", path, sz / 1e6);
    return ok;
}

// letterbox：等比缩放 + 灰边(114)，返回缩放比与左上角偏移
static void letterbox(const cv::Mat& src, cv::Mat& dst, int size, float& scale, int& pad_x, int& pad_y) {
    const int w = src.cols, h = src.rows;
    scale = std::min((float)size / w, (float)size / h);
    const int nw = (int)(w * scale), nh = (int)(h * scale);
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
    dst = cv::Mat(size, size, src.type(), cv::Scalar(114, 114, 114));
    pad_x = (size - nw) / 2;
    pad_y = (size - nh) / 2;
    resized.copyTo(dst(cv::Rect(pad_x, pad_y, nw, nh)));
}

static void send_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

// 网页外壳：/ 返回页面，页面里用 <img src="/stream"> 拉 MJPEG 流
static const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EdgeVision RK3588 · 实时预览</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;background:#0b1120;color:#e2e8f0;font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
header{padding:16px 24px;border-bottom:1px solid #1e293b;display:flex;align-items:center;gap:10px}
h1{font-size:15px;margin:0;font-weight:600;letter-spacing:.2px}
.dot{width:8px;height:8px;border-radius:50%;background:#64748b}
.dot.on{background:#22c55e;box-shadow:0 0 8px #22c55e}
.sub{font-size:12px;color:#94a3b8;margin-left:auto;font-variant-numeric:tabular-nums}
main{padding:20px 24px;display:flex;justify-content:center}
.frame{max-width:100%;border:1px solid #1e293b;border-radius:10px;overflow:hidden;background:#000;line-height:0}
img{display:block;max-width:100%;height:auto}
footer{padding:0 24px 28px;font-size:12px;color:#64748b;line-height:1.9}
a{color:#7dd3fc;text-decoration:none}
a:hover{text-decoration:underline}
</style></head>
<body>
<header><span class="dot" id="dot"></span><h1>EdgeVision RK3588 · DFL-Free INT8 实时预览</h1><span class="sub" id="st">连接中…</span></header>
<main><div class="frame"><img id="v" src="/stream" alt="live"></div></main>
<footer>
每帧链路：UVC 采集 → letterbox 640×640 → NPU 三核推理 → DFL 解码 + NMS → 画框 → JPEG<br>
单帧快照：<a href="/snapshot">/snapshot</a> ｜ 原始流：<a href="/stream">/stream</a><br>
看不到框？把目标放到画面中间——占比太小时置信度会掉到阈值 0.25 以下。
</footer>
<script>
var img=document.getElementById('v'),st=document.getElementById('st'),dot=document.getElementById('dot');
img.onload=function(){st.textContent='LIVE';dot.className='dot on';};
img.onerror=function(){st.textContent='断流，重连中…';dot.className='dot';
  setTimeout(function(){img.src='/stream?t='+Date.now();},2000);};
</script>
</body></html>)HTML";

static void serve_index(int fd) {
    char hdr[256];
    const size_t len = strlen(kIndexHtml);
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", len);
    send_all(fd, hdr, n);
    send_all(fd, kIndexHtml, len);
}

static void serve_snapshot(int fd) {
    std::vector<uint8_t> jpg;
    pthread_mutex_lock(&g_jpeg_lock);
    jpg = g_latest_jpeg;
    pthread_mutex_unlock(&g_jpeg_lock);
    if (jpg.empty()) {
        const char* r = "HTTP/1.0 503 No Frame\r\nContent-Length: 0\r\n\r\n";
        send_all(fd, r, strlen(r));
        return;
    }
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", jpg.size());
    send_all(fd, hdr, n);
    send_all(fd, (const char*)jpg.data(), jpg.size());
}

static void* http_server_thread(void*) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_PORT);
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return nullptr; }
    listen(server_fd, 5);
    printf("[+] 网页预览: http://<board-ip>:%d   ( / 页面 | /stream 流 | /snapshot 快照 )\n", HTTP_PORT);

    while (g_running) {
        int c_fd = accept(server_fd, nullptr, nullptr);
        if (c_fd < 0) break;

        char req[1024] = {0};
        recv(c_fd, req, sizeof(req) - 1, 0);

        char path[128] = "/";
        if (sscanf(req, "GET %127s", path) != 1) { close(c_fd); continue; }

        if (strncmp(path, "/snapshot", 9) == 0) {
            serve_snapshot(c_fd);
            close(c_fd);
            continue;
        }
        if (strncmp(path, "/stream", 7) != 0) {
            serve_index(c_fd);
            close(c_fd);
            continue;
        }

        const char* stream_header =
            "HTTP/1.0 200 OK\r\n"
            "Server: RK3588-EdgeVision\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n\r\n";
        send_all(c_fd, stream_header, strlen(stream_header));

        while (g_running) {
            std::vector<uint8_t> jpeg_copy;
            pthread_mutex_lock(&g_jpeg_lock);
            pthread_cond_wait(&g_jpeg_cond, &g_jpeg_lock);
            if (!g_running) { pthread_mutex_unlock(&g_jpeg_lock); break; }
            jpeg_copy = g_latest_jpeg;
            pthread_mutex_unlock(&g_jpeg_lock);
            if (jpeg_copy.empty()) continue;

            char part_hdr[128];
            int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", jpeg_copy.size());
            if (send(c_fd, part_hdr, hdr_len, MSG_NOSIGNAL) < 0) break;
            if (send(c_fd, jpeg_copy.data(), jpeg_copy.size(), MSG_NOSIGNAL) < 0) break;
            if (send(c_fd, "\r\n", 2, MSG_NOSIGNAL) < 0) break;
        }
        close(c_fd);
    }
    close(server_fd);
    return nullptr;
}

int main(int argc, char* argv[]) {
    const char* dev_path = (argc > 1) ? argv[1] : "/dev/video81";
    const float conf = (argc > 2) ? (float)atof(argv[2]) : 0.25f;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!load_model_file(MODEL_PATH)) return 1;

    rknn_context ctx = 0;
    if (rknn_init(&ctx, g_model_data.data(), (uint32_t)g_model_data.size(), 0, nullptr) < 0) {
        std::cerr << "[-] rknn_init 失败\n";
        return 1;
    }
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_0_1_2);

    // 识别两个输出（raw_box 64ch / cls_sig 80ch）
    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
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
    if (bi < 0 || ci < 0) { std::cerr << "[-] 输出识别失败\n"; return 1; }
    printf("[+] 输出: raw_box idx=%d(nhwc=%d)  cls_sig idx=%d(nhwc=%d)\n", bi, bnhwc, ci, cnhwc);

    cv::VideoCapture cap(dev_path, cv::CAP_V4L2);
    if (!cap.isOpened()) { std::cerr << "[-] 无法打开摄像头: " << dev_path << "\n"; return 1; }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);

    cv::Mat warm;
    for (int i = 0; i < 15; ++i) cap >> warm;   // AE 预热收敛

    pthread_t http_tid;
    pthread_create(&http_tid, nullptr, http_server_thread, nullptr);

    std::vector<rknn_output> outputs(io.n_output);
    cv::Mat bgr, lb, rgb;
    std::vector<Detection> dets;
    uint32_t frame_id = 0;
    auto t_start = std::chrono::steady_clock::now();

    while (g_running) {
        auto tA = std::chrono::steady_clock::now();
        cap >> bgr;                       // 含 UVC 取流 + MJPG 解码
        auto tB = std::chrono::steady_clock::now();
        if (bgr.empty()) { usleep(5000); continue; }
        frame_id++;

        float scale = 1.f;
        int pad_x = 0, pad_y = 0;
        letterbox(bgr, lb, 640, scale, pad_x, pad_y);
        cv::cvtColor(lb, rgb, cv::COLOR_BGR2RGB);
        auto tC = std::chrono::steady_clock::now();

        rknn_input in{};
        in.index = 0;
        in.buf = rgb.data;
        in.size = 640 * 640 * 3;
        in.type = RKNN_TENSOR_UINT8;
        in.fmt = RKNN_TENSOR_NHWC;

        auto t0 = std::chrono::steady_clock::now();
        rknn_inputs_set(ctx, 1, &in);
        rknn_run(ctx, nullptr);
        for (auto& o : outputs) { memset(&o, 0, sizeof(o)); o.want_float = 1; }
        rknn_outputs_get(ctx, io.n_output, outputs.data(), nullptr);
        auto t1 = std::chrono::steady_clock::now();

        dflfree::decode((const float*)outputs[bi].buf, (const float*)outputs[ci].buf,
                        bnhwc, cnhwc, 640, conf, 0.45f, dets);
        auto t2 = std::chrono::steady_clock::now();
        rknn_outputs_release(ctx, io.n_output, outputs.data());

        const double npu_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double dec_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

        // 把 640 letterbox 坐标还原到原始画面
        for (const auto& d : dets) {
            int x1 = (int)((d.x1 - pad_x) / scale);
            int y1 = (int)((d.y1 - pad_y) / scale);
            int x2 = (int)((d.x2 - pad_x) / scale);
            int y2 = (int)((d.y2 - pad_y) / scale);
            x1 = std::max(0, std::min(x1, bgr.cols - 1));
            x2 = std::max(0, std::min(x2, bgr.cols - 1));
            y1 = std::max(0, std::min(y1, bgr.rows - 1));
            y2 = std::max(0, std::min(y2, bgr.rows - 1));

            cv::rectangle(bgr, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(0, 255, 0), 2);
            char label[96];
            const char* name = (d.class_id >= 0 && d.class_id < 80) ? COCO80[d.class_id] : "?";
            snprintf(label, sizeof(label), "%s %.0f%%", name, d.score * 100);
            const int tw = 12 * (int)strlen(label);
            const int ty = y1 > 24 ? y1 : y1 + 26;
            cv::rectangle(bgr, cv::Point(x1, ty - 24), cv::Point(x1 + tw, ty),
                          cv::Scalar(0, 255, 0), -1);
            cv::putText(bgr, label, cv::Point(x1 + 5, ty - 7),
                        cv::FONT_HERSHEY_SIMPLEX, 0.65, cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
        }

        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        double fps = (double)frame_id / (elapsed + 1e-5);

        char hud[192];
        snprintf(hud, sizeof(hud),
                 "DFL-Free INT8 | NPU %.1f ms + Decode %.1f ms | %.1f FPS | %zu obj",
                 npu_ms, dec_ms, fps, dets.size());
        cv::rectangle(bgr, cv::Point(0, 0), cv::Point(bgr.cols, 40), cv::Scalar(15, 23, 42), -1);
        cv::putText(bgr, hud, cv::Point(16, 28), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(56, 189, 248), 2, cv::LINE_AA);

        auto tD = std::chrono::steady_clock::now();

        std::vector<uint8_t> jpg;
        cv::imencode(".jpg", bgr, jpg, {cv::IMWRITE_JPEG_QUALITY, 80});
        auto tE = std::chrono::steady_clock::now();

        if (frame_id % 30 == 0) {
            auto ms = [](std::chrono::steady_clock::time_point a,
                         std::chrono::steady_clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            printf("[profile] 采集+MJPG %5.1f | 预处理 %4.1f | NPU %5.1f | 解码 %4.1f "
                   "| 画框 %4.1f | JPEG编码 %5.1f || 合计 %5.1f ms\n",
                   ms(tA, tB), ms(tB, tC), ms(tC, t1), ms(t1, t2),
                   ms(t2, tD), ms(tD, tE), ms(tA, tE));
            fflush(stdout);
        }

        pthread_mutex_lock(&g_jpeg_lock);
        g_latest_jpeg = std::move(jpg);
        pthread_cond_broadcast(&g_jpeg_cond);
        pthread_mutex_unlock(&g_jpeg_lock);
    }

    cap.release();
    rknn_destroy(ctx);
    pthread_cond_broadcast(&g_jpeg_cond);
    pthread_join(http_tid, nullptr);
    return 0;
}
