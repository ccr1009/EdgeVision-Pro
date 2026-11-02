#include <fcntl.h>      // open
#include <unistd.h>     // read, close
#include <cstdio>       // printf, perror
#include <cstdlib>      // atoi

int main() {
    const char* path = "/sys/class/thermal/thermal_zone0/temp";
    int fd = open(path, O_RDONLY);          // 打开文件 -> 文件描述符
    if (fd < 0) { perror("open"); return 1; }

    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);   // 读内容
    if (n < 0) { perror("read"); close(fd); return 1; }
    close(fd);                              // 关闭

    int milli = atoi(buf);                  // "38846" -> 38846
    printf("温度: %.2f C\n", milli / 1000.0);
    return 0;
}
