#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>      // fcntl, O_NONBLOCK (系统调用控制)
#include <string.h>
#include <errno.h>      // errno, EAGAIN, EWOULDBLOCK
#include <sys/socket.h>
#include <netinet/in.h>

int main() {
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

    // ★ 核心魔法：把 server_fd 设置为【非阻塞模式】！
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    printf("[RK3588 边缘主机] 非阻塞网络已启动 (9000 端口)！\n");
    printf("[RK3588 边缘主机] 即使没有上位机连接，AI 推理主循环也不会被卡死！\n\n");

    int ai_frame = 0;

    // 主业务大循环
    for (int i = 1; i <= 10; i++) {
        // 1. 迎宾员瞅一眼门口：非阻塞 accept
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd >= 0) {
            // 抓到真正的连接了！
            printf("  ★ [网络事件] 电脑连进来了！立刻推流并释放连接\n");
            const char* msg = "Hello from Non-Blocking RK3588!\n";
            send(client_fd, msg, strlen(msg), 0);
            close(client_fd);
        } else {
            // 没连进来是正常的，系统会置错误码为 EAGAIN / EWOULDBLOCK
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 证明此时没有网络连接，直接跳过，绝不卡死！
            }
        }

        // 2. 核心 AI 业务持续全速运行
        ai_frame++;
        printf("[NPU AI 引擎] 持续执行第 %d 帧目标检测推理 (未受网络阻塞影响)...\n", ai_frame);
        usleep(400000); // 模拟一帧推理耗时 0.4 秒
    }

    close(server_fd);
    printf("\n>>> 10 轮 AI 推理丝滑跑完，网络从未造成任何卡顿！<<<\n");
    return 0;
}
