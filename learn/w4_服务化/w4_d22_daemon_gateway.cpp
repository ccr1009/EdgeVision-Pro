#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main() {
    printf("[前台进程 PID=%d] 准备化身为后台守护进程 (Daemon)...\n", getpid());
    sleep(1);

    // ★★★ 神仙系统调用：一秒脱离终端，变身后台守护进程！★★★
    // 参数 1: 0 表示把工作目录切换到根目录 "/"，防止占用可卸载设备
    // 参数 2: 0 表示把输入输出重定向到 /dev/null 黑洞
    if (daemon(0, 0) < 0) {
        perror("daemon 失败");
        return -1;
    }

    // --- 下面的代码，已经在后台幽灵般常驻运行了！---

    // 记录它的新身份到日志文件 (因为此时没有屏幕可以 printf 了)
    FILE* log_fp = fopen("/root/lab/daemon_gateway.log", "a");
    if (log_fp) {
        fprintf(log_fp, "[守护进程启动] 新的独立 PID: %d, 已脱离终端在后台常驻！\n", getpid());
        fclose(log_fp);
    }

    // 启动 9000 端口 TCP 推流服务
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(9000);
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    // 守护进程在后台默默接客，哪怕 SSH 关了也持续提供服务！
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            const char* msg = "【RK3588 后台守护进程】恭喜！即使 SSH 断开，网关依然在线为你推流！\n";
            send(client_fd, msg, strlen(msg), 0);
            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}
