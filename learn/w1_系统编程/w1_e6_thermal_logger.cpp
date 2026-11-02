#include <iostream>
#include <dirent.h>     // opendir, readdir, closedir (Day 5 学的)
#include <fcntl.h>      // open, O_RDONLY, O_WRONLY, O_CREAT, O_APPEND (Day 1, Day 3)
#include <unistd.h>     // read, write, close
#include <cstring>      // strncmp, snprintf
#include <ctime>        // time, strftime 获取系统时间

// 专门用来读取小文件单行内容的辅助函数 (复用 Day 1)
bool read_sys_file(const char* filepath, char* out_buf, size_t max_len) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return false;
    
    ssize_t n = read(fd, out_buf, max_len - 1);
    close(fd);
    if (n <= 0) return false;
    
    out_buf[n] = '\0';
    if (out_buf[n - 1] == '\n') out_buf[n - 1] = '\0'; // 去掉末尾换行
    return true;
}

int main() {
    const char* thermal_dir_path = "/sys/class/thermal";
    DIR* dir = opendir(thermal_dir_path);
    if (!dir) {
        std::cerr << "打开 thermal 目录失败" << std::endl;
        return -1;
    }

    // 打开(或新建)日志文件 thermal.log，设置权限为可读写，模式为追加写入 (O_APPEND)
    int log_fd = open("thermal.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

    // 获取当前时间字符串：比如 2026-10-12 14:20:00
    time_t now = time(nullptr);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    std::cout << "==================================================" << std::endl;
    std::cout << "  RK3588 全温区健康巡检 (" << time_str << ") " << std::endl;
    std::cout << "==================================================" << std::endl;

    struct dirent* entry = nullptr;
    int sensor_count = 0;

    // 遍历 /sys/class/thermal/ 下的所有条目
    while ((entry = readdir(dir)) != nullptr) {
        // 只筛选出以 thermal_zone 开头的文件夹 (比如 thermal_zone0, thermal_zone1)
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            char type_path[256], temp_path[256];
            snprintf(type_path, sizeof(type_path), "%s/%s/type", thermal_dir_path, entry->d_name);
            snprintf(temp_path, sizeof(temp_path), "%s/%s/temp", thermal_dir_path, entry->d_name);

            char sensor_type[64] = "未知类型";
            char temp_raw[32] = "0";

            read_sys_file(type_path, sensor_type, sizeof(sensor_type));
            if (read_sys_file(temp_path, temp_raw, sizeof(temp_raw))) {
                double temp_c = std::atof(temp_raw) / 1000.0; // 毫摄氏度转为摄氏度

                // 1. 打印到控制台屏幕
                std::cout << "传感器 [" << sensor_type << "]\t当前温度: " 
                          << temp_c << " °C" << std::endl;

                // 2. 格式化写入日志文件 (如果日志文件打开成功)
                if (log_fd >= 0) {
                    char log_line[256];
                    int len = snprintf(log_line, sizeof(log_line), 
                                       "[%s] %s (%s): %.2f C\n", 
                                       time_str, sensor_type, entry->d_name, temp_c);
                    write(log_fd, log_line, len);
                }
                sensor_count++;
            }
        }
    }

    closedir(dir);
    if (log_fd >= 0) close(log_fd);

    std::cout << "==================================================" << std::endl;
    std::cout << ">>> 巡检完成！共记录 " << sensor_count 
              << " 个温区，数据已成功保存至 thermal.log <<<" << std::endl;

    return 0;
}
