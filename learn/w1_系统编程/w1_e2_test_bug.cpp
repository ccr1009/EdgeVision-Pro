#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>       // 包含 perror

int main() {
    // 故意写一个不存在的假文件路径
    const char* fake_path = "/root/edgevision/data/models/no_such_file.rknn";

    int fd = open(fake_path, O_RDONLY);
    if (fd < 0) {
        // perror 会自动查出系统错误原因并打印出来！
        perror("[致命错误] open 打开模型失败");
        return -1;
    }

    close(fd);
    return 0;
}
