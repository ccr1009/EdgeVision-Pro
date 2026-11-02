#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    // 1. 创建 UDP 套接字 (注意第 2 个参数是 SOCK_DGRAM，不是 TCP 的 SOCK_STREAM)
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("创建 UDP socket 失败");
        return -1;
    }

    // 2. 开启广播权限 (大喇叭开关，必须显式开启)
    int broadcast_enable = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    // 3. 目标地址设为全局广播地址 255.255.255.255，端口 8888
    struct sockaddr_in broadcast_addr;
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(8888);
    broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");

    printf("[RK3588 广播服务] 启动成功，每隔 1 秒向全网大声广播心跳...\n");

    const char* heartbeat_msg = "DISCOVER_BEACON: RK3588_AI_GATEWAY, SERVICE_PORT: 9000";

    // 连续广播 5 次心跳
    for (int i = 1; i <= 5; i++) {
        // UDP 发送不需要 connect，直接 sendto 指明目标地址！
        sendto(sock_fd, heartbeat_msg, strlen(heartbeat_msg), 0,
               (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
        printf("[RK3588] 发送第 %d 次广播心跳: \"%s\"\n", i, heartbeat_msg);
        sleep(1);
    }

    close(sock_fd);
    printf("[RK3588 广播服务] 广播完毕，正常退出！\n");
    return 0;
}
