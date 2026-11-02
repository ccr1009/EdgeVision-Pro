#include <iostream>
#include <dirent.h>     // Step 2
#include <cstring>      // Step 2
#include <fcntl.h>      // Step 1
#include <unistd.h>     // Step 1
#include <cstdlib>      // Step 1

int main() {
    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return -1;

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            
            // ★ 用胶水拼出完整路径：/sys/class/thermal/thermal_zoneX/temp
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", entry->d_name);

            // ★ 下面这几行，是不是完全就是 Step 1 的代码？
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                char buf[32] = {0};
                read(fd, buf, sizeof(buf) - 1);
                close(fd);

                double temp = std::atof(buf) / 1000.0;
                std::cout << entry->d_name << " 温度: " << temp << " °C" << std::endl;
            }
        }
    }

    closedir(dir);
    return 0;
}
