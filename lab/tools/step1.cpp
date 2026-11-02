#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>

int main() {
    // 1. 打开温度文件 (只读模式)
    int fd = open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY);
    if (fd < 0) {
        std::cerr << "打开失败" << std::endl;
        return -1;
    }

    // 2. 读出字符串
    char buf[32] = {0};
    read(fd, buf, sizeof(buf) - 1);
    close(fd);

    // 3. 转成摄氏度并打印
    double temp = std::atof(buf) / 1000.0;
    std::cout << "thermal_zone0 温度: " << temp << " °C" << std::endl;

    return 0;
}
