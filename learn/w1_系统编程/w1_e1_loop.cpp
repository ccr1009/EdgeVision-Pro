#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

int main() {
    const char* path = "/sys/class/thermal/thermal_zone0/temp";
    for (int i = 0; i < 10; i++) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) { perror("open"); return 1; }

        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n < 0) { perror("read"); return 1; }

        int milli = atoi(buf);
        printf("[%d] 温度: %.2f C\n", i + 1, milli / 1000.0);
        sleep(1);
    }
    return 0;
}
