#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// 1. 定长包头 (固定 8 字节)
struct DetectionHeader {
    uint32_t frame_id;
    uint32_t bbox_count;
};

// 2. 目标检测框结构体 (固定 24 字节)
struct BBox {
    int   class_id;    // 0: 行人, 1: 汽车
    float score;       // 置信度 (0.0 ~ 1.0)
    float x, y, w, h;  // 坐标
};

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(9000); // 监听 9000 端口

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);
    printf("[RK3588 AI 网关] 等待上位机电脑连接 9000 端口进行推流...\n");

    int client_fd = accept(server_fd, NULL, NULL);
    printf("[RK3588 AI 网关] 电脑已连接！开始连续推送 3 帧目标检测数据流...\n");

    // 模拟连续推理 3 帧画面
    for (uint32_t f = 1; f <= 3; f++) {
        sleep(1); // 模拟每秒出 1 帧检测结果

        // 构造包头：告诉电脑这一帧有 2 个检测目标
        DetectionHeader header;
        header.frame_id = f;
        header.bbox_count = 2;

        // 构造包体：2 个具体的检测目标
        BBox boxes[2];
        // 目标 1：行人，置信度 0.92
        boxes[0].class_id = 0;
        boxes[0].score = 0.92f;
        boxes[0].x = 100.0f; boxes[0].y = 150.0f; boxes[0].w = 50.0f; boxes[0].h = 120.0f;

        // 目标 2：汽车，置信度 0.88
        boxes[1].class_id = 1;
        boxes[1].score = 0.88f;
        boxes[1].x = 300.0f; boxes[1].y = 200.0f; boxes[1].w = 180.0f; boxes[1].h = 90.0f;

        // ★ 核心发送逻辑：先发定长包头，再发变长数据体！
        send(client_fd, &header, sizeof(header), 0);
        send(client_fd, boxes, sizeof(boxes), 0);

        printf("[RK3588 AI 网关] 已推送 Frame #%d: 包含 %d 个目标框 (行人+汽车)\n", 
               f, header.bbox_count);
    }

    close(client_fd);
    close(server_fd);
    printf("[RK3588 AI 网关] 推流演示结束，正常关闭！\n");
    return 0;
}
