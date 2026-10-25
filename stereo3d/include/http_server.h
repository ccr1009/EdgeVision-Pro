// ============================================================================
//  http_server.h —— MJPEG 网页预览（/ 、/stream 、/snapshot 、/stats、/depth）
//  实现文件：src/http_server.cpp  （owner: face3d-view）
//  ★ 直接复刻 edgevision-rk3588/lab/w4_edgevision_preview_real.cpp 的成熟做法：
//    原生 POSIX socket + pthread，不引第三方库；单生产者多次发布，多消费者订阅。
// ============================================================================
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <pthread.h>

namespace s3d {

class HttpPreview {
public:
    // port: 监听端口；title 显示在网页标题上
    HttpPreview(int port, std::string title);
    ~HttpPreview();

    // 启动监听线程（非阻塞），返回 false 表示 bind 失败
    bool start();
    // 停止：置停止标志 + 关闭 listen fd + join
    void stop();
    bool running() const { return running_; }

    // 生产者（主循环）调用：发布最新一帧 MJPEG + 一份统计文本
    void publish(const std::vector<uint8_t>& jpeg,
                 const std::string& stats_json);

    // 发布「单张视差图/深度图」的 JPEG（GET /depth 用），可选
    void publishDepth(const std::vector<uint8_t>& jpeg);

private:
    static void* threadEntry(void* self);
    void loop();

    int         port_;
    std::string title_;
    int         listen_fd_ = -1;
    bool        running_ = false;
    pthread_t   tid_{};
    bool        started_ = false;

    pthread_mutex_t         lock_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t          cond_ = PTHREAD_COND_INITIALIZER;
    std::vector<uint8_t>    jpeg_;
    std::vector<uint8_t>    depth_jpeg_;
    std::string             stats_;
    uint64_t                seq_ = 0;
};

}  // namespace s3d
