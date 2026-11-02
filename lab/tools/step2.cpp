#include <iostream>
#include <dirent.h>     // opendir, readdir, closedir
#include <cstring>      // strncmp

int main() {
    // 1. 打开目录
    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) {
        std::cerr << "打开目录失败" << std::endl;
        return -1;
    }

    struct dirent* entry = nullptr;
    // 2. 像翻书一样，一页页读目录里的项
    while ((entry = readdir(dir)) != nullptr) {
        // 只挑出以 "thermal_zone" 开头的名字
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            std::cout << "找到温区: " << entry->d_name << std::endl;
        }
    }

    // 3. 关门
    closedir(dir);
    return 0;
}
