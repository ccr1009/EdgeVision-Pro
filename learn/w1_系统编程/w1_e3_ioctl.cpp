#include <iostream>
#include <unistd.h>     // STDOUT_FILENO
#include <sys/ioctl.h>  // ioctl 函数
#include <termios.h>    // winsize 结构体
#include <cstdio>       // perror
int main() {
    struct winsize size;
    // 向终端设备发出 TIOCGWINSZ 请求，获取当前窗口宽高
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
        perror("ioctl 获取窗口尺寸失败");
        return -1;
    }
    std::cout << ">>> ioctl 调用成功！<<<" << std::endl;
    std::cout << "当前终端宽度 (列数): " << size.ws_col << " 列" << std::endl;
    std::cout << "当前终端高度 (行数): " << size.ws_row << " 行" << std::endl;
    return 0;
}
