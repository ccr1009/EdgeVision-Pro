#include <iostream>
#include <csignal>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    }
}

int main() {
    std::cout << "[Init] 模拟初始化边缘服务（开启摄像头 /dev/video81，加载 NPU）...\n";
    std::cout << "[Init] 当前进程 PID = " << getpid() << "\n";

    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::cout << "[Run] 主循环运行中... 请按 Ctrl+C 或在另一终端执行 kill -15 " << getpid() << "\n";

    int frame_id = 0;
    while (g_running) {
        frame_id++;
        std::cout << "  -> [FPS 30] 正在处理第 " << frame_id << " 帧画面，NPU 推理中...\n";
        sleep(1);
    }

    std::cout << "\n=========================================\n";
    std::cout << "[Shutdown] 捕获到终止信号，开始优雅收尾：\n";
    std::cout << "  1. 停止 V4L2 摄像头硬件推流 ioctl(STREAMOFF) ... [OK]\n";
    std::cout << "  2. 释放 NPU 驱动上下文 rknn_destroy() ... [OK]\n";
    std::cout << "  3. 关闭 TCP 推流 Socket 句柄 close(fd) ... [OK]\n";
    std::cout << "[Shutdown] 所有硬件资源已安全释放，进程完美退出。\n";
    std::cout << "=========================================\n";

    return 0;
}
