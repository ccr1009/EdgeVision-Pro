#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <csignal>

#define MAX_EVENTS 64
#define PORT 9001

static volatile sig_atomic_t g_running = 1;

void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    }
}

// 辅助函数：将 socket 设置为非阻塞模式（epoll 的黄金搭档）
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    // 1. 信号处理与屏蔽 SIGPIPE
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    // 2. 创建迎宾 Socket (Listen Socket)
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(listen_fd, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(listen_fd, 10);
    set_nonblocking(listen_fd);

    std::cout << "[Init] 迎宾 Socket 创建成功，监听端口: " << PORT << "\n";

    // 3. 【核心 API 1】创建 epoll 呼叫中心
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        return 1;
    }

    // 4. 【核心 API 2】把迎宾 Socket 挂上内核红黑树，监听读事件（EPOLLIN）
    epoll_event ev{};
    ev.events = EPOLLIN; // 有新连接也是一种读事件！
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    std::cout << "[Init] epoll 反应堆就绪，开始单线程高并发监听...\n";

    epoll_event events[MAX_EVENTS];

    // 5. 事件主循环
    while (g_running) {
        // 【核心 API 3】睡觉等铃响，超时设为 1000 毫秒（避免死等方便检测 g_running）
        int n_ready = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);

        if (n_ready < 0) {
            if (!g_running) break; // 被信号唤醒
            continue;
        }

        // 遍历所有按了铃的就绪描述符（按铃几个就循环几次，绝对不遍历无辜的）
        for (int i = 0; i < n_ready; ++i) {
            int current_fd = events[i].data.fd;

            if (current_fd == listen_fd) {
                // 情况 A：按铃的是迎宾员 -> 说明有新客人进店！
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);
                if (client_fd >= 0) {
                    set_nonblocking(client_fd);
                    
                    // 给新客人发呼叫铃：挂入 epoll 监听他的说话（读）事件
                    epoll_event client_ev{};
                    client_ev.events = EPOLLIN | EPOLLET; // 监听可读事件
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);

                    std::cout << "  [+] [新连接] 客户端 FD=" << client_fd << " 连入，已自动挂载到 epoll 红黑树！\n";
                }
            } else {
                // 情况 B：按铃的是已连接的客人 -> 说明有数据发过来了！
                char buf[512] = {0};
                int bytes = recv(current_fd, buf, sizeof(buf) - 1, 0);

                if (bytes > 0) {
                    std::cout << "  [数据] 收到 FD=" << current_fd << " 发送: " << buf;
                    // Echo 回复
                    std::string reply = "[RK3588 epoll 应答] 收到: " + std::string(buf);
                    send(current_fd, reply.c_str(), reply.size(), MSG_NOSIGNAL);
                } else if (bytes == 0 || (bytes < 0 && errno != EAGAIN)) {
                    // 客户端主动断开或异常
                    std::cout << "  [-] [断开] 客户端 FD=" << current_fd << " 离线，从 epoll 红黑树移除！\n";
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_fd, nullptr); // 注销
                    close(current_fd); // 关掉描述符
                }
            }
        }
    }

    std::cout << "\n[Shutdown] 正在关闭 epoll 反应堆与迎宾 Socket...\n";
    close(listen_fd);
    close(epoll_fd);
    std::cout << "[Shutdown] epoll 优雅退出完毕。\n";

    return 0;
}
