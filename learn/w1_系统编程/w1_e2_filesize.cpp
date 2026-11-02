#include <iostream>
#include <sys/stat.h>   // stat 结构体
#include <fcntl.h>      // open 函数和 O_RDONLY 宏
#include <unistd.h>     // lseek 和 close 函数

int main() {
    const char* path = "/root/edgevision/data/models/yolov8n_rk3588_i8.rknn";

    // ---------------- 方法 1：stat ----------------
    struct stat st;
    if (stat(path, &st) == 0) {
        std::cout << "[方法 1: stat]  大小: " << st.st_size << " bytes" << std::endl;
    } else {
        std::cerr << "stat 失败！" << std::endl;
        return -1;
    }

    // ---------------- 方法 2：open + lseek ----------------
    int fd = open(path, O_RDONLY);  // 以只读方式打开
    if (fd < 0) {
        std::cerr << "open 失败！" << std::endl;
        return -1;
    }

    // 把读写指针拉到文件末尾 (SEEK_END)，偏移 0 字节
    off_t size_by_lseek = lseek(fd, 0, SEEK_END);
    if (size_by_lseek < 0) {
        std::cerr << "lseek 失败！" << std::endl;
        close(fd);
        return -1;
    }

    std::cout << "[方法 2: lseek] 大小: " << size_by_lseek << " bytes" << std::endl;

    // 用完一定记得关闭
    close(fd);

    // ---------------- 校验对比 ----------------
    if (st.st_size == size_by_lseek) {
        std::cout << ">>> 完美！两种方式测出的大小完全一致！<<<" << std::endl;
    } else {
        std::cerr << ">>> 警告：两种方式大小不一致！<<<" << std::endl;
    }

    return 0;
}
