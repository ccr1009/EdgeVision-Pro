#include <iostream>
#include <dirent.h>     // opendir, readdir, closedir, struct dirent
#include <fcntl.h>      // open, O_RDONLY
#include <unistd.h>     // read, close
#include <cctype>       // isdigit
#include <cstring>      // strlen
#include <cstdio>       // perror

// 判断字符串是不是纯数字 (PID)
bool is_pid_folder(const char* name) {
    if (!name || name[0] == '\0') return false;
    for (int i = 0; name[i] != '\0'; ++i) {
        if (!isdigit(name[i])) return false;
    }
    return true;
}

int main() {
    const char* proc_dir = "/proc";
    
    // 1. 打开 /proc 目录
    DIR* dir = opendir(proc_dir);
    if (!dir) {
        perror("opendir /proc 失败");
        return -1;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  Mini-ps: 当前板子上运行的所有进程扫描  " << std::endl;
    std::cout << "========================================" << std::endl;

    struct dirent* entry = nullptr;
    int process_count = 0;

    // 2. 循环翻页遍历目录中的每一个条目
    while ((entry = readdir(dir)) != nullptr) {
        // 如果文件夹名字是纯数字，说明找到了一个正在运行的进程！
        if (is_pid_folder(entry->d_name)) {
            char comm_path[256];
            // 拼出路径：例如 /proc/1/comm
            snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", entry->d_name);

            // 3. 用 Day 1 学的 open + read 读出名字
            int fd = open(comm_path, O_RDONLY);
            if (fd >= 0) {
                char comm_name[128] = {0};
                ssize_t n = read(fd, comm_name, sizeof(comm_name) - 1);
                close(fd);

                if (n > 0) {
                    // 去掉末尾的换行符
                    if (comm_name[n - 1] == '\n') comm_name[n - 1] = '\0';

                    std::cout << "PID: " << entry->d_name 
                              << "\t-> 进程名: " << comm_name << std::endl;
                    process_count++;
                }
            }
        }
    }

    // 3. 关闭目录
    closedir(dir);

    std::cout << "========================================" << std::endl;
    std::cout << ">>> 扫描完毕！当前板上活跃进程总数: " << process_count << " 个 <<<" << std::endl;

    return 0;
}
