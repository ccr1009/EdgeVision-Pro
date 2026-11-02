#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(9000);
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("[RK3588 边缘主机] 【阻塞版反例】已启动！\n");
    printf("[RK3588 边缘主机] 注意看：因为没加非阻塞，主程序马上要被卡死了...\n\n");

    for (int i = 1; i <= 10; i++) {
        printf("--- 准备尝试接收网络连接 --- \n");

        // ★ 致命死等：因为是默认阻塞模式，如果没有电脑连进来，代码会永远死死卡在这里！
        int client_fd = accept(server_fd, NULL, NULL);

        // 下面的 AI 业务代码，永远没有机会被执行！
        printf("[NPU AI 引擎] 执行第 %d 帧 AI 目标检测...\n", i);
        usleep(400000);
    }

    close(server_fd);
    return 0;
}
