#include <iostream>
#include <fcntl.h>      // open, O_WRONLY
#include <unistd.h>     // write, close, usleep, lseek
#include <cstdio>       // perror

int main() {
    // 1. 先确保 trigger 是 none（夺取控制权）
    int fd_trig = open("/sys/class/leds/work/trigger", O_WRONLY);
    if (fd_trig >= 0) {
        write(fd_trig, "none", 4);
        close(fd_trig);
    }

    // 2. 打开控制亮度的文件
    const char* path = "/sys/class/leds/work/brightness";
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("打开 brightness 失败");
        return -1;
    }

    std::cout << ">>> 开始控制板载 LED 快闪 5 次... 请盯紧板子右边的灯！<<<" << std::endl;

    for (int i = 1; i <= 5; ++i) {
        std::cout << "第 " << i << " 次: 亮 -> 灭" << std::endl;

        // 磁头归位，写入 "1" 点亮
        lseek(fd, 0, SEEK_SET);
        write(fd, "1", 1);
        usleep(300000); // 延时 300 毫秒 (0.3秒)

        // 磁头归位，写入 "0" 熄灭
        lseek(fd, 0, SEEK_SET);
        write(fd, "0", 1);
        usleep(300000); // 延时 300 毫秒
    }

    // 最后保持点亮状态
    lseek(fd, 0, SEEK_SET);
    write(fd, "1", 1);
    std::cout << ">>> 闪烁结束，当前保持常亮！<<<" << std::endl;

    close(fd);
    return 0;
}
