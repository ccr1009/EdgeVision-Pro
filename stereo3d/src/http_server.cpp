// ============================================================================
//  http_server.cpp —— MJPEG 网页预览（/ 、/stream 、/snapshot 、/stats 、/depth）
//  目标板：RK3588 / Debian 11 / OpenCV 4.5.1 / g++ 10.2.1 / C++17
//  实现 include/http_server.h 的契约（头文件只读，签名一字未改）
//
//  ---------------------------------------------------------------------------
//  设计要点（校招面试素材，讲「为什么」）
//  ---------------------------------------------------------------------------
//  1) 为什么用【条件变量】而不是【轮询】—— 本题最核心的一个考点
//     /stream 每个订阅者的需求是「有新帧就发，没新帧就等着」。
//     朴素做法是 while(1){ if (seq != last) send(); usleep(5000); }：
//        - 空转唤醒：3 个观众 × 200 Hz = 每秒 600 次无意义唤醒，每次都要抢锁。
//          摄像头 + SGBM 已经把大核吃满，这些唤醒会抢走 SGBM 的调度时间片，
//          直接体现为端到端 FPS 掉。
//        - 延迟有下限：usleep(5ms) 意味着平均多等 2.5 ms，而且不可预测。
//     条件变量是**事件驱动**：publish() 里 pthread_cond_broadcast() 把阻塞在
//     cond_wait 上的线程从内核等待队列（futex）里摘出来放进就绪队列 ——
//     没有等待者时 broadcast 几乎零成本，有新帧时唤醒延迟是微秒级。
//     没新帧时线程**真的一点 CPU 都不占**，这就是「阻塞优于忙等」。
//     ★ 必须用 while 重判条件，不能写 if：
//       ① 虚假唤醒（spurious wakeup）—— 内核允许无信号唤醒，标准明确要求重判；
//       ② 广播后多个线程抢同一把锁，抢到的那个可能已经把 seq 又推进了。
//
//  2) 为什么新连接不回放历史帧（背压）
//     若新订阅者从「队头」开始补发，一连上就瞬间灌几十上百帧老数据：
//     浏览器 <img> 只显示最后一帧，前面全白传 —— 每帧 JPEG 约 100 KB，
//     100 帧就是 10 MB，百兆上行会被打满，反过来拖慢主循环的 publish()（抢同一把锁）。
//     所以连接建立时把 last_seq 直接对齐到当前 seq_：
//     只发「连上之后产生的新帧」，天然等价于「永远只发最新帧」。
//
//  3) 为什么所有 send 都带 MSG_NOSIGNAL
//     浏览器关标签页 = 对端发 FIN/RST，此时 send() 触发 SIGPIPE，
//     而 SIGPIPE 的默认动作是**终止进程** —— 一个人关网页就能把整个视觉服务干掉。
//     两种解法：全局 signal(SIGPIPE, SIG_IGN)（污染整个进程），
//     或每次 send 带 MSG_NOSIGNAL（只作用于本次调用，返回 EPIPE 自己处理）。
//     这里用后者：不靠「别人记得调 signal」这个隐含前提，本模块自己就安全。
//
//  4) 为什么一个连接一个线程（而不是单线程内联、也不是 epoll）
//     ★ 单线程内联（把 /stream 的 for 循环写在 accept 循环里）是错的：
//       一个观众把连接保持一辈子，accept 就永远回不来 —— 第二个观众连不上，
//       连页面里 fetch('/stats') 这种请求也会被堵死。所以必须每个连接独立线程。
//     观众数量是「人」的量级（1~5 个），每个线程阻塞在 cond_wait 上不占 CPU，
//     代码量小、正确性最容易验证；真上到几百并发才值得上 epoll。
//     这是「按规模选架构」，不是「不会 epoll」。
//
//  5) /depth 为什么只存不广播
//     视差/深度图是给调试面板低频拉的（人眼观察），没必要跟 30 fps 主画面同频，
//     更不该为它多维护一套订阅者列表 —— 存最新一张，客户端 GET 时现取。
// ============================================================================
#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace s3d {
namespace {

// ---------------------------------------------------------------------------
//  send_all：把 len 字节全部发出去
//  TCP 是字节流，send() 只保证「写进去了一部分」，在板子上单帧 JPEG 约 100 KB，
//  远超 socket 发送缓冲区，一次 send 发不完是常态 —— 必须循环。
//  ★ MSG_NOSIGNAL：见文件头第 3 点
//  返回 false 表示对端已断开/出错，调用方应立即结束该连接。
// ---------------------------------------------------------------------------
bool send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;   // 被信号打断：重试，不是错误
            return false;                   // EPIPE / ECONNRESET / EAGAIN(超时)
        }
        if (n == 0) return false;           // 对端关闭
        off += static_cast<size_t>(n);
    }
    return true;
}

bool send_str(int fd, const std::string& s) { return send_all(fd, s.data(), s.size()); }

// 极简 HTTP 响应（只支持 GET，够用且没有解析负担）
bool send_response(int fd, int code, const char* reason, const char* ctype,
                   const std::string& body) {
    char hdr[512];
    const int n = std::snprintf(hdr, sizeof(hdr),
                                "HTTP/1.0 %d %s\r\n"
                                "Server: stereo3d-rk3588\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %zu\r\n"
                                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                                "Pragma: no-cache\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                code, reason, ctype, body.size());
    if (n <= 0) return false;
    if (!send_all(fd, hdr, static_cast<size_t>(n))) return false;
    return body.empty() || send_all(fd, body.data(), body.size());
}

bool send_jpeg(int fd, const std::vector<uint8_t>& jpg) {
    char hdr[256];
    const int n = std::snprintf(hdr, sizeof(hdr),
                                "HTTP/1.0 200 OK\r\n"
                                "Server: stereo3d-rk3588\r\n"
                                "Content-Type: image/jpeg\r\n"
                                "Content-Length: %zu\r\n"
                                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                jpg.size());
    if (n <= 0) return false;
    if (!send_all(fd, hdr, static_cast<size_t>(n))) return false;
    return send_all(fd, jpg.data(), jpg.size());
}

// 从 "GET /stream?t=1698 HTTP/1.1" 抠出 path，并去掉 query（浏览器重连会带 ?t=时间戳）
std::string parse_path(const char* req) {
    const char* p = std::strstr(req, "GET ");   // 只认 GET：本项目没有写接口
    if (!p) return std::string();
    p += 4;
    const char* end = p;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n' && *end != '?') ++end;
    return std::string(p, end);
}

// ---------------------------------------------------------------------------
//  网页外壳：/ 返回这段 HTML，页面里用 <img src="/stream"> 拉 MJPEG 流
//  - 深色主题（与 edgevision-rk3588 的预览页保持同一套视觉语言）
//  - onerror 2 秒后重连：MJPEG 断流时浏览器只触发一次 onerror，不重连画面永久黑
//  - 底部把「这条流水线是干嘛的」写清楚：答辩/面试投屏就能直接讲
// ---------------------------------------------------------------------------
std::string index_html(const std::string& title) {
    return std::string(R"HTML(<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>)HTML") + title + R"HTML(</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;background:#0b1120;color:#e2e8f0;font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
header{padding:16px 24px;border-bottom:1px solid #1e293b;display:flex;align-items:center;gap:10px;flex-wrap:wrap}
h1{font-size:15px;margin:0;font-weight:600;letter-spacing:.2px}
.dot{width:8px;height:8px;border-radius:50%;background:#64748b;flex:0 0 auto}
.dot.on{background:#22c55e;box-shadow:0 0 8px #22c55e}
.sub{font-size:12px;color:#94a3b8;margin-left:auto;font-variant-numeric:tabular-nums}
main{padding:20px 24px;display:flex;justify-content:center}
.frame{max-width:100%;border:1px solid #1e293b;border-radius:10px;overflow:hidden;background:#000;line-height:0}
img{display:block;max-width:100%;height:auto}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px;padding:0 24px 8px}
.card{background:#111c33;border:1px solid #1e293b;border-radius:10px;padding:12px 14px;font-size:12px;color:#cbd5e1;line-height:1.8}
.card b{color:#7dd3fc;font-weight:600}
pre{margin:0;font-size:11.5px;color:#94a3b8;white-space:pre-wrap;word-break:break-all;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
footer{padding:8px 24px 28px;font-size:12px;color:#64748b;line-height:1.9}
a{color:#7dd3fc;text-decoration:none}
a:hover{text-decoration:underline}
.warn{color:#fbbf24}
code{color:#a5b4fc}
</style></head>
<body>
<header>
  <span class="dot" id="dot"></span>
  <h1 id="ttl">)HTML" + title + R"HTML(</h1>
  <span class="sub" id="st">连接中…</span>
</header>
<main><div class="frame"><img id="v" src="/stream" alt="live"></div></main>
<div class="grid">
  <div class="card"><b>实时参数 /stats</b><pre id="stats">加载中…</pre></div>
  <div class="card"><b>快捷入口</b>
    <a href="/snapshot">/snapshot 单帧快照</a><br>
    <a href="/depth">/depth 最新视差/深度图</a><br>
    <a href="/stats">/stats 统计 JSON</a><br>
    <a href="/stream">/stream 原始 MJPEG</a>
  </div>
</div>
<footer>
<b>这条流水线在干嘛：</b>两颗 USB 摄像头（左 <code>/dev/video81</code>／右 <code>/dev/video83</code>）
先做<b>双目立体矫正</b>（去畸变 + 极线对齐，让同名点落在同一行），
再用 <b>SGBM 半全局匹配</b>算稠密视差，按三角测量 <code>Z = fx·B / d</code> 换成深度图；
然后左目灰度图跑 <b>Haar 人脸检测</b>，把每个脸框<b>向内缩 10%</b> 后统计有效深度像素，
取<b>深度中位数</b>当作这张脸的距离（中位数能免疫背景混叠和 SGBM 离群点，均值不能），
最后由「框中心像素 + 中值视差」反投影出<b>相机系 3D 坐标 (X,Y,Z)</b>，单位米。<br>
<span class="warn">未标定</span>时 HUD 显示 <code>UNCALIBRATED</code>：此时只有相对远近可信，绝对距离不可信。<br>
页面断流 2 秒自动重连；连接建立后只推之后的<b>最新帧</b>（不回放历史，避免背压堆积）。
</footer>
<script>
var img=document.getElementById('v'),st=document.getElementById('st'),dot=document.getElementById('dot'),
    sbox=document.getElementById('stats');
img.onload=function(){st.textContent='LIVE';dot.className='dot on';};
img.onerror=function(){st.textContent='断流，重连中…';dot.className='dot';
  setTimeout(function(){img.src='/stream?t='+Date.now();},2000);};
function poll(){
  fetch('/stats',{cache:'no-store'}).then(function(r){return r.text();}).then(function(t){
    var o; try{o=JSON.parse(t);}catch(e){sbox.textContent=t;return;}
    sbox.textContent=Object.keys(o).map(function(k){return k+': '+o[k];}).join('\n');
  }).catch(function(){sbox.textContent='（后端未响应）';})
   .then(function(){setTimeout(poll,1000);});
}
poll();
</script>
</body></html>)HTML";
}

// ---------------------------------------------------------------------------
//  在册连接表（文件级静态）
//  为什么在这儿而不是成员：http_server.h 是只读契约，只给了 tid_ / listen_fd_，
//  没有容器放「一批连接线程」。职责上这个类就是单实例（一个进程一个预览服务），
//  所以用文件级静态就够了。★★ 如果将来要给头文件加成员，这里应该升级成
//  HttpPreview 的私有成员（同时也就支持多实例了）—— 如实说明，不藏着。
//  用途只有两个：① stop() 时 shutdown 所有在途连接，把阻塞的 send 踢醒；
//                  ② stop() 时等所有连接线程退出，避免析构后线程还在碰 this。
// ---------------------------------------------------------------------------
struct ConnRegistry {
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;
    std::vector<int> fds;
};

ConnRegistry& registry() {
    static ConnRegistry r;   // C++11 起函数内 static 局部变量的初始化是线程安全的
    return r;
}

void conn_add(int fd) {
    ConnRegistry& r = registry();
    pthread_mutex_lock(&r.mtx);
    r.fds.push_back(fd);
    pthread_mutex_unlock(&r.mtx);
}

void conn_done(int fd) {
    ConnRegistry& r = registry();
    pthread_mutex_lock(&r.mtx);
    for (size_t i = 0; i < r.fds.size(); ++i) {
        if (r.fds[i] == fd) {
            r.fds.erase(r.fds.begin() + static_cast<long>(i));
            break;
        }
    }
    pthread_cond_broadcast(&r.cv);
    pthread_mutex_unlock(&r.mtx);
}

// stop()：对每个在途连接 shutdown(SHUT_RDWR)。
// 这一步不能省：连接线程可能正阻塞在 send()（浏览器停在后台标签页、TCP 零窗口），
// 光把 running_ 置 false 是叫不醒它的，必须让内核把这个 socket 上的等待打断。
void conn_shutdown_all() {
    ConnRegistry& r = registry();
    pthread_mutex_lock(&r.mtx);
    for (int fd : r.fds) ::shutdown(fd, SHUT_RDWR);
    pthread_mutex_unlock(&r.mtx);
}

// stop()：等所有连接线程自行退出（它们会在 recv/send/cond_wait 上看到错误或标志）
void conn_wait_all() {
    ConnRegistry& r = registry();
    pthread_mutex_lock(&r.mtx);
    while (!r.fds.empty()) {
        // 带超时地等：极端情况下（比如内核卡在 send）也不至于永久 hang 住主线程
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        pthread_cond_timedwait(&r.cv, &r.mtx, &ts);
        for (int fd : r.fds) ::shutdown(fd, SHUT_RDWR);   // 再补一刀
    }
    pthread_mutex_unlock(&r.mtx);
}

// 线程入口的参数包：区分「accept 主线程」和「单个连接线程」
struct JobArg {
    HttpPreview* owner;
    int          fd;
    bool         is_main;
};

}  // namespace

// ===========================================================================
//  构造 / 析构
// ===========================================================================
HttpPreview::HttpPreview(int port, std::string title) : port_(port), title_(std::move(title)) {}

HttpPreview::~HttpPreview() {
    // 析构必须停线程：否则线程还在用 this-> 成员，对象没了就是 use-after-free
    stop();
}

// ===========================================================================
//  启动：非阻塞（socket/bind/listen 在调用线程里同步做完，出错能立刻返回 false；
//               只有 accept 循环扔到子线程）
// ===========================================================================
bool HttpPreview::start() {
    if (started_) return true;

    // 1) socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::perror("[http] socket");
        listen_fd_ = -1;
        return false;
    }

    // 2) SO_REUSEADDR：程序刚被 Ctrl-C 时 TCP 处于 TIME_WAIT，
    //    不设这个要等 60 秒才能重启同一个端口 —— 调试时极其难受
    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3) bind + listen。绑 INADDR_ANY 让同网段的电脑能直接访问板子 IP
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port_));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[http] bind");
        std::fprintf(stderr, "[http] 端口 %d 绑定失败（可能被占用）\n", port_);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 16) < 0) {
        std::perror("[http] listen");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    JobArg* arg = new JobArg{this, -1, true};
    if (pthread_create(&tid_, nullptr, &HttpPreview::threadEntry, arg) != 0) {
        std::perror("[http] pthread_create");
        delete arg;
        running_ = false;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    started_ = true;
    std::printf("[http] 预览已启动: http://<board-ip>:%d/  "
                "( / | /stream | /snapshot | /stats | /depth )\n", port_);
    std::fflush(stdout);
    return true;
}

// ===========================================================================
//  停止：置标志 + 关 listen fd 让 accept 立刻返回 + 唤醒/踢醒所有连接 + 等它们退出
// ===========================================================================
void HttpPreview::stop() {
    if (!started_) return;

    running_ = false;

    // ★ 关键：accept() 阻塞在 listen fd 上，半路不会自己醒。
    //   只有 close()/shutdown() 能让它返回（EBADF/EINVAL）。
    //   （「自己连自己一次骗 accept」也行，但要额外 socket 且有端口竞争，不划算。）
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    // 叫醒所有还在 cond_wait 里的 /stream 线程，让它们重判 running_ 后退出
    pthread_mutex_lock(&lock_);
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&lock_);

    // 把阻塞在 send/recv 上的连接线程踢醒（这时它们才会去看 running_）
    conn_shutdown_all();

    pthread_join(tid_, nullptr);   // 等 accept 线程退出（之后不会再有新连接进来）
    conn_wait_all();               // 等所有连接线程退出
    started_ = false;
}

// ===========================================================================
//  生产者：发布最新 JPEG + stats 文本，并唤醒所有订阅者
// ===========================================================================
void HttpPreview::publish(const std::vector<uint8_t>& jpeg, const std::string& stats_json) {
    pthread_mutex_lock(&lock_);
    jpeg_  = jpeg;
    stats_ = stats_json;
    ++seq_;
    // ★ 用 broadcast 不用 signal：signal 只唤醒一个，其它观众得等下一帧，
    //   多观众时表现为「有的浏览器画面滞后几帧」。broadcast 有惊群代价，
    //   但观众数是人级（<10），惊群成本远小于画面不同步的体验问题。
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&lock_);
}

// 只存不广播（见文件头第 5 点）
void HttpPreview::publishDepth(const std::vector<uint8_t>& jpeg) {
    pthread_mutex_lock(&lock_);
    depth_jpeg_ = jpeg;
    pthread_mutex_unlock(&lock_);
}

// ===========================================================================
//  线程入口：一个函数两种身份（accept 主线程 / 单个连接线程）
//  为什么挤在一个函数里：http_server.h 是我不能改的契约，里面只有
//  `static void* threadEntry(void*)` 这一个线程入口，没有 serveConn 之类的成员。
//  而读 jpeg_/seq_/stats_ 这些私有成员必须在成员函数（或成员函数内定义的 lambda）
//  作用域里 —— 所以连接处理逻辑写在这里，靠 JobArg::is_main 分流。
// ===========================================================================
void* HttpPreview::threadEntry(void* raw) {
    JobArg* job = static_cast<JobArg*>(raw);
    HttpPreview* self = job->owner;
    const int c_fd = job->fd;
    const bool is_main = job->is_main;
    delete job;

    if (is_main) {
        self->loop();
        return nullptr;
    }

    // -----------------------------------------------------------------------
    //  以下是「单个连接」的全部处理（路由 + 分流）
    // -----------------------------------------------------------------------
    conn_add(c_fd);   // 登记，stop() 才能踢醒/等待本线程

    // 不设 SO_RCVTIMEO 的话，一个只连不发的客户端能把 recv 永久卡住
    // （每连接一线程时它只会卡住自己，但仍然会泄漏一个线程）
    timeval rtv{};
    rtv.tv_sec  = 3;
    rtv.tv_usec = 0;
    ::setsockopt(c_fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    // 发送超时：浏览器切到后台标签页 / 网络卡住时 TCP 会零窗口，
    // send 可能长时间阻塞。给 5 秒上限，超时就当对端已死，回收线程。
    timeval stv{};
    stv.tv_sec  = 5;
    stv.tv_usec = 0;
    ::setsockopt(c_fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

    // TCP_NODELAY：MJPEG 一帧被拆成「分片头 + 图像 + CRLF」三次 send，
    // Nagle 会把最后那个 2 字节的 CRLF 压住等 ACK，白白拖慢下一帧的边界。
    int one = 1;
    ::setsockopt(c_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char req[2048] = {0};
    // 请求头通常一次 recv 就够（浏览器的 GET 只有几百字节）；
    // 只读一次也够用 —— 我们只关心第一行的方法和路径。
    const ssize_t rn = ::recv(c_fd, req, sizeof(req) - 1, 0);
    if (rn > 0) {
        const std::string path = parse_path(req);

        if (path == "/stats") {
            std::string body;
            pthread_mutex_lock(&self->lock_);
            body = self->stats_;
            pthread_mutex_unlock(&self->lock_);
            if (body.empty()) body = "{}";   // 还没出第一帧：给合法空 JSON，不编造字段
            send_response(c_fd, 200, "OK", "application/json; charset=utf-8", body);

        } else if (path == "/snapshot") {
            std::vector<uint8_t> jpg;
            pthread_mutex_lock(&self->lock_);
            jpg = self->jpeg_;
            pthread_mutex_unlock(&self->lock_);
            if (jpg.empty()) {
                send_response(c_fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                              "no frame yet\n");
            } else {
                send_jpeg(c_fd, jpg);
            }

        } else if (path == "/depth") {
            std::vector<uint8_t> jpg;
            pthread_mutex_lock(&self->lock_);
            jpg = self->depth_jpeg_;
            pthread_mutex_unlock(&self->lock_);
            if (jpg.empty()) {
                // 503 而不是 200+空图：让客户端能区分「这个能力当前没数据」
                // 和「深度图恰好全黑」，后者会被误判成算法坏了
                send_response(c_fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                              "depth image not published yet\n");
            } else {
                send_jpeg(c_fd, jpg);
            }

        } else if (path == "/stream") {
            const char* kBoundary = "frame";
            const std::string hdr =
                "HTTP/1.0 200 OK\r\n"
                "Server: stereo3d-rk3588\r\n"
                "Connection: close\r\n"
                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Pragma: no-cache\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
            if (send_str(c_fd, hdr)) {
                // ★ 背压核心：last_seq 对齐到「当前」seq_，本连接只收之后的新帧。
                //   既不回放历史帧，又天然保证「发的永远是最新帧」
                //   （同一时刻多个广播，醒来时直接用 seq_ 对应的那份 jpeg_）。
                uint64_t last_seq;
                pthread_mutex_lock(&self->lock_);
                last_seq = self->seq_;
                pthread_mutex_unlock(&self->lock_);

                while (self->running_) {
                    std::vector<uint8_t> jpg;
                    uint64_t cur_seq = 0;

                    pthread_mutex_lock(&self->lock_);
                    // ★ while 而不是 if（见文件头第 1 点）
                    while (self->running_ && self->seq_ == last_seq) {
                        pthread_cond_wait(&self->cond_, &self->lock_);
                    }
                    if (!self->running_) {
                        pthread_mutex_unlock(&self->lock_);
                        break;
                    }
                    jpg     = self->jpeg_;   // 只拷最新一帧
                    cur_seq = self->seq_;
                    pthread_mutex_unlock(&self->lock_);
                    last_seq = cur_seq;

                    if (jpg.empty()) continue;

                    char part[192];
                    const int hn = std::snprintf(part, sizeof(part),
                                                 "--%s\r\nContent-Type: image/jpeg\r\n"
                                                 "Content-Length: %zu\r\n\r\n",
                                                 kBoundary, jpg.size());
                    if (hn <= 0) break;
                    // 任意一处失败 = 对端断开 -> 立刻结束本连接。
                    // 不判断的话断开的客户端会让本线程每帧空 send 一次，白烧 CPU
                    if (!send_all(c_fd, part, static_cast<size_t>(hn)) ||
                        !send_all(c_fd, jpg.data(), jpg.size()) ||
                        !send_all(c_fd, "\r\n", 2)) {
                        break;
                    }
                }
            }

        } else {
            // "/" 以及所有未知路径都给主页（单页应用，没有 404 的必要）
            send_response(c_fd, 200, "OK", "text/html; charset=utf-8", index_html(self->title_));
        }
    }

    conn_done(c_fd);
    ::close(c_fd);
    return nullptr;
}

// ===========================================================================
//  accept 主循环：来一个连接就起一个线程（见文件头第 4 点）
// ===========================================================================
void HttpPreview::loop() {
    while (running_) {
        int c_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (c_fd < 0) {
            if (errno == EINTR) continue;
            // running_ == false（stop() 关掉了 listen fd）是正常退出路径
            if (!running_) break;
            continue;
        }
        if (!running_) {          // stop() 与 accept 返回之间的竞态：别再起线程
            ::close(c_fd);
            break;
        }

        JobArg* arg = new JobArg{this, c_fd, false};
        pthread_t tid;
        if (pthread_create(&tid, nullptr, &HttpPreview::threadEntry, arg) != 0) {
            std::perror("[http] pthread_create(conn)");
            delete arg;
            ::close(c_fd);
            continue;
        }
        // detach：连接线程自己管生命周期，join 由 conn_wait_all() 用「登记表清空」代替
        // （不用 join 是因为连接是动态的，攒 pthread_t 列表还要处理复用与回收，更复杂）
        pthread_detach(tid);
    }

    // 退出前再叫醒一次，防止还有线程睡死在 cond_wait
    pthread_mutex_lock(&lock_);
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&lock_);
}

}  // namespace s3d
