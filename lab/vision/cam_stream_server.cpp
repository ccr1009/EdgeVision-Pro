#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <signal.h>

static volatile bool g_running = true;

void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

struct Buffer {
    void* start;
    size_t length;
};

int main(int argc, char* argv[]) {
    const char* dev_name = (argc > 1) ? argv[1] : "/dev/video81";
    int port = (argc > 2) ? std::atoi(argv[2]) : 8080;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN); // 防止浏览器断开连接导致进程崩溃

    // 1. 打开 V4L2 摄像头设备
    int cam_fd = open(dev_name, O_RDWR);
    if (cam_fd < 0) {
        std::cerr << "[-] 无法打开摄像头设备: " << dev_name << " (" << strerror(errno) << ")\n";
        return 1;
    }
    std::cout << "[+] 成功打开摄像头: " << dev_name << "\n";

    // 2. 设置格式为 MJPEG 1280x720
    v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 1280;
    fmt.fmt.pix.height = 720;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(cam_fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "[-] VIDIOC_S_FMT 失败: " << strerror(errno) << "\n";
        close(cam_fd);
        return 1;
    }
    std::cout << "[+] 设置视频格式成功: 1280x720 (MJPEG)\n";

    // 3. 申请 4 个 MMAP 内存缓冲区
    v4l2_requestbuffers req = {};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam_fd, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "[-] VIDIOC_REQBUFS 失败\n";
        close(cam_fd);
        return 1;
    }

    std::vector<Buffer> buffers(req.count);
    for (unsigned int i = 0; i < req.count; ++i) {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(cam_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "[-] VIDIOC_QUERYBUF 失败\n";
            close(cam_fd);
            return 1;
        }
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam_fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            std::cerr << "[-] mmap 失败\n";
            close(cam_fd);
            return 1;
        }
        if (ioctl(cam_fd, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "[-] VIDIOC_QBUF 失败\n";
            close(cam_fd);
            return 1;
        }
    }

    // 4. 开启视频流采集
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "[-] VIDIOC_STREAMON 失败: " << strerror(errno) << "\n";
        close(cam_fd);
        return 1;
    }
    std::cout << "[+] V4L2 硬件视频流已启动！\n";

    // 5. 预热丢弃前 15 帧（让自动曝光 AE 收敛）
    for (int i = 0; i < 15; ++i) {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        ioctl(cam_fd, VIDIOC_DQBUF, &buf);
        ioctl(cam_fd, VIDIOC_QBUF, &buf);
    }
    std::cout << "[+] 传感器 15 帧自动曝光（AE）预热完成！\n";

    // 6. 创建 HTTP TCP 服务端
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[-] 绑定端口 " << port << " 失败: " << strerror(errno) << "\n";
        close(cam_fd);
        return 1;
    }
    listen(server_fd, 5);

    std::cout << "\n=======================================================\n";
    std::cout << "  🚀 RK3588 实时对焦监视器已上线！\n";
    std::cout << "  请在电脑浏览器中直接打开:\n";
    std::cout << "  👉 http://<board-ip>:" << port << "/\n";
    std::cout << "=======================================================\n\n";

    // 7. 处理客户端请求循环
    while (g_running) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }

        char req_buf[1024] = {0};
        read(client_fd, req_buf, sizeof(req_buf) - 1);

        // 如果访问首页 /，返回带控制样式的 HTML 监视器页面
        if (std::strstr(req_buf, "GET / HTTP") != nullptr || std::strstr(req_buf, "GET /index") != nullptr) {
            std::string html = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Connection: close\r\n\r\n"
                "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<title>RK3588 实时对焦监视器</title>"
                "<style>"
                "body { background: #0f172a; color: #f8fafc; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; text-align: center; margin: 0; padding: 24px; }"
                "h1 { color: #38bdf8; margin-bottom: 8px; font-size: 26px; }"
                "p { color: #94a3b8; font-size: 15px; margin-top: 0; }"
                ".tag { display: inline-block; background: #0369a1; color: #e0f2fe; padding: 4px 12px; border-radius: 9999px; font-weight: bold; font-size: 13px; margin-bottom: 16px; }"
                ".card { display: inline-block; background: #1e293b; border-radius: 12px; padding: 12px; box-shadow: 0 10px 25px -5px rgba(0,0,0,0.5); border: 1px solid #334155; }"
                "img { max-width: 90vw; max-height: 75vh; border-radius: 8px; border: 2px solid #0284c7; display: block; margin: 0 auto; }"
                ".tips { margin-top: 16px; background: #334155; display: inline-block; padding: 10px 20px; border-radius: 8px; color: #f1f5f9; font-size: 14px; }"
                "</style></head><body>"
                "<h1>📷 RK3588 双目摄像头实时对焦监视器</h1>"
                "<div class='tag'>● LIVE 直播中 (1280x720 @ 30FPS)</div><br>"
                "<div class='card'>"
                "<img src='/stream' alt='Live Video Stream'>"
                "</div><br>"
                "<div class='tips'>💡 <b>调焦指引</b>：用手轻轻顺时针或逆时针旋转 <b>Cam 1 镜头最前端的外环</b>，实时观察画面边缘，直到文字与线条最锐利！</div>"
                "</body></html>";

            send(client_fd, html.c_str(), html.size(), MSG_NOSIGNAL);
            close(client_fd);
            continue;
        }

        // 如果请求 /stream，进入 MJPEG 连续推流
        const char* stream_header =
            "HTTP/1.0 200 OK\r\n"
            "Server: RK3588-MJPEG-Streamer\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n\r\n";

        send(client_fd, stream_header, strlen(stream_header), MSG_NOSIGNAL);

        // 连续推流循环
        while (g_running) {
            v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (ioctl(cam_fd, VIDIOC_DQBUF, &buf) < 0) {
                break;
            }

            char part_hdr[128];
            int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                buf.bytesused);

            // 发送帧头
            if (send(client_fd, part_hdr, hdr_len, MSG_NOSIGNAL) < 0) {
                ioctl(cam_fd, VIDIOC_QBUF, &buf);
                break;
            }

            // 发送零拷贝 JPEG 原始数据
            if (send(client_fd, buffers[buf.index].start, buf.bytesused, MSG_NOSIGNAL) < 0) {
                ioctl(cam_fd, VIDIOC_QBUF, &buf);
                break;
            }

            // 发送换行分隔符
            if (send(client_fd, "\r\n", 2, MSG_NOSIGNAL) < 0) {
                ioctl(cam_fd, VIDIOC_QBUF, &buf);
                break;
            }

            // 将缓冲区放回队列
            ioctl(cam_fd, VIDIOC_QBUF, &buf);
        }

        close(client_fd);
    }

    // 释放资源
    ioctl(cam_fd, VIDIOC_STREAMOFF, &type);
    for (auto& b : buffers) {
        munmap(b.start, b.length);
    }
    close(cam_fd);
    close(server_fd);
    std::cout << "[+] 实时监视器已退出\n";
    return 0;
}
