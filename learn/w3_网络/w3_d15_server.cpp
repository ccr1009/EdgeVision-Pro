#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    // 1. 买电话机 (IPv4 协议, TCP 字节流)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("创建 socket 失败");
        return -1;
    }

    // 允许地址快速复用，防止重启服务器时报 Address already in use
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. 绑定手机卡 (绑定本地所有网卡 0.0.0.0, 端口 9000)
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 接收任何网卡来的连接
    server_addr.sin_port = htons(9000);              // 监听 9000 端口

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind 端口绑定失败");
        close(server_fd);
        return -1;
    }

    // 3. 待机响铃 (最多允许 5 个连接在门外排队)
    listen(server_fd, 5);
    printf("[RK3588 网关] 服务器已就绪，正在 9000 端口等待电脑连接呼叫...\n");

    // 4. 铃响了，接通电话！
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept 接听失败");
        close(server_fd);
        return -1;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    printf("[RK3588 网关] 成功接通来自电脑的连接！IP: %s, 端口: %d\n", client_ip, ntohs(client_addr.sin_port));

    // 5. 接收电脑发来的消息
    char buffer[256] = {0};
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read > 0) {
        printf("[RK3588 网关] 收到电脑数据: \"%s\"\n", buffer);

        // 回复电脑一句话 (Echo 确认)
        const char* reply = "[RK3588] 消息已收到，连接正常！\n";
        send(client_fd, reply, strlen(reply), 0);
    }

    // 挂断电话
    close(client_fd);
    close(server_fd);
    printf("[RK3588 网关] 本次通信完成，正常挂机关门！\n");
    return 0;
}
