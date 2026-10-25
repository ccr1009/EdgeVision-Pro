#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "rkllm.h"

// -------------------------------------------------------------
// 全局状态与输出缓冲
// -------------------------------------------------------------
static std::string g_step_output;
// --no-led 模式: 巡检守护拥有 LED 写权, Agent 降级为只读 (task-6)
static bool g_no_led_mode = false;

int callback(RKLLMResult *result, void *userdata, LLMCallState state) {
    if (state == RKLLM_RUN_NORMAL) {
        std::cout << result->text << std::flush;
        g_step_output += result->text;
    } else if (state == RKLLM_RUN_FINISH) {
        std::cout << std::endl;
    }
    return 0;
}

static std::string read_file_string(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream ss;
    ss << file.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

static bool write_file_string(const std::string& path, const std::string& val) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << val;
    // 显式检查流状态: sysfs 写入可能因权限/节点不存在静默失败
    file.flush();
    if (!file.good()) return false;
    return true;
}

// -------------------------------------------------------------
// LED 驱动辅助函数
// -------------------------------------------------------------
// 用户态爆闪是否正在运行(本进程的 control_led alarm 或巡检守护共用同一 pid 文件)。
// 为什么必须查这个: 本板内核没有 CONFIG_LEDS_TRIGGER_TIMER, alarm 只能靠用户态
// shell 循环翻转 brightness 实现, 此时 trigger 恒为 "none"。若只看 trigger+brightness,
// 恰好在灭相(0)采样就会把正在爆闪的报警灯误判成 "off" —— 实测踩过: Agent 因此
// 向用户汇报「LED 始终熄灭, 导致过热保护被抑制」的完全错误结论。
static bool led_user_blink_alive();

std::string get_led_current_mode() {
    // 最高优先级: 用户态爆闪在跑 → 就是报警态, 与 trigger 内容无关
    if (led_user_blink_alive()) return "alarm_blink";

    std::string trig = read_file_string("/sys/class/leds/work/trigger");
    size_t l = trig.find('[');
    size_t r = trig.find(']');
    if (l != std::string::npos && r != std::string::npos && r > l) {
        std::string mode = trig.substr(l + 1, r - l - 1);
        if (mode == "none") {
            // 兜底: pid 文件缺失/被清理时, 多点采样识别 100ms 翻转(单次读会踩灭相)
            int hi = 0, lo = 0;
            for (int i = 0; i < 12; ++i) { // 12 x 15ms = 180ms, 覆盖 1.8 个爆闪周期
                std::string s = read_file_string("/sys/class/leds/work/brightness");
                if (s == "0") lo++; else hi++;
                if (hi > 0 && lo > 0) return "alarm_blink";
                usleep(15000);
            }
            return (hi > 0) ? "on" : "off";
        }
        if (mode == "timer") return "alarm";
        if (mode == "default-on") return "on";
        return mode; // e.g. "heartbeat"
    }
    return "unknown";
}

// -------------------------------------------------------------
// 工具 1: 物理硬件状态与温区采集
// -------------------------------------------------------------
std::string tool_get_system_health() {
    double soc_temp = 0.0;
    double npu_temp = 0.0;
    std::string t0 = read_file_string("/sys/class/thermal/thermal_zone0/temp");
    std::string t1 = read_file_string("/sys/class/thermal/thermal_zone1/temp");
    std::string t2 = read_file_string("/sys/class/thermal/thermal_zone2/temp");
    if (!t0.empty()) soc_temp = std::stod(t0) / 1000.0;
    if (!t2.empty()) npu_temp = std::stod(t2) / 1000.0;
    else npu_temp = soc_temp;

    // NPU 负载
    std::string npu_raw = read_file_string("/sys/kernel/debug/rknpu/load");
    int c0 = 0, c1 = 0, c2 = 0;
    if (!npu_raw.empty()) {
        sscanf(npu_raw.c_str(), "NPU load: Core0: %d%%, Core1: %d%%, Core2: %d%%", &c0, &c1, &c2);
    }

    // 内存
    std::ifstream meminfo("/proc/meminfo");
    long total_kb = 0, avail_kb = 0;
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0) sscanf(line.c_str(), "MemTotal: %ld", &total_kb);
        if (line.rfind("MemAvailable:", 0) == 0) sscanf(line.c_str(), "MemAvailable: %ld", &avail_kb);
    }

    std::string led_mode = get_led_current_mode();

    std::stringstream ss;
    ss << "[{"
       << "\"soc_temp_c\": " << soc_temp << ", "
       << "\"npu_temp_c\": " << npu_temp << ", "
       << "\"npu_load\": \"core0:" << c0 << "%, core1:" << c1 << "%, core2:" << c2 << "%\","
       << "\"mem_avail_mb\": " << (avail_kb / 1024) << ", "
       << "\"current_led_mode\": \"" << led_mode << "\", "
       << "\"hardware_model\": \"Rockchip RK3588 (8GB)\", "
       << "\"status\": \"" << (soc_temp > 75.0 ? "high_temperature_warning" : "normal_optimal") << "\""
       << "}]";
    return ss.str();
}

// -------------------------------------------------------------
// 工具 2: 查询当前 LED 状态 (读操作)
// -------------------------------------------------------------
std::string tool_get_led_status() {
    std::string mode = get_led_current_mode();
    std::string br = read_file_string("/sys/class/leds/work/brightness");
    std::stringstream ss;
    ss << "[{"
       << "\"led_name\": \"work\", "
       << "\"current_mode\": \"" << mode << "\", "
       << "\"brightness\": " << (br.empty() ? "0" : br) << ", "
       << "\"description\": \"" << (mode == "heartbeat" ? "正常呼吸闪烁"
                                       : (mode == "alarm" ? "100ms急闪报警"
                                       : (mode == "alarm_blink" ? "100ms急闪报警(用户态爆闪运行中)"
                                       : (mode == "on" ? "常亮模式" : "熄灭关闭")))) << "\""
       << "}]";
    return ss.str();
}

// -------------------------------------------------------------
// 工具 3: 控制板载物理 LED 执行器 (写操作, P0 加固版)
//   - 模式白名单校验, 非法值不触碰任何 sysfs 节点
//   - 每一步写入都检查结果, 任一失败立即上报 failed_node
//   - 写后回读 actual_mode / brightness, 与请求不符则报 readback_mismatch
//
//   硬件事实(实测): 本板内核 5.10.110 出厂未编 CONFIG_LEDS_TRIGGER_TIMER,
//   trigger 列表里没有 timer, 写 "timer" 返回 EINVAL(旧的静默假成功即来源于此)。
//   因此 alarm 模式改为用户态爆闪: fork 一个 setsid 守护子进程,
//   以 100ms 周期翻转 brightness; 切换到其他模式时先 kill 该子进程。
// -------------------------------------------------------------
#define LED_ALARM_PID_FILE "/tmp/edgevision_led_alarm.pid"
#define LED_ALARM_PERIOD_US 100000 // 100ms

// 判定用户态爆闪子进程是否存活。pid 文件由本进程(control_led alarm)与
// 巡检守护(edgevision_patrol_daemon)共用, 因此这里读到 alive 也可能是守护在闪灯
// —— 对只读查询来说这正是我们想要的: 无论谁在闪, 物理上就是报警态。
static bool led_user_blink_alive() {
    std::ifstream pf(LED_ALARM_PID_FILE);
    if (!pf.is_open()) return false;
    pid_t pid = 0;
    pf >> pid;
    pf.close();
    if (pid <= 1) return false;
    // kill(pid,0) 成功仅代表"有权限发信号且进程存在", 还需排除僵尸与 pid 复用:
    // 读 /proc/<pid>/cmdline 确认仍是那个 while 循环爆闪命令。
    if (kill(pid, 0) != 0) {
        // 进程已死但 pid 文件残留(实测: 回归脚本收尾/异常退出会留下陈旧 pid 文件)。
        // 顺手清掉, 避免文件长期堆积误导人工排查。
        ::unlink(LED_ALARM_PID_FILE);
        return false;
    }
    std::string cmd = read_file_string("/proc/" + std::to_string((long)pid) + "/cmdline");
    if (cmd.find("brightness") == std::string::npos) {
        // pid 被内核复用给了别的进程 —— 同样视为爆闪已停, 并清掉陈旧 pid 文件
        ::unlink(LED_ALARM_PID_FILE);
        return false;
    }
    return true;
}

// 停掉 alarm 爆闪子进程(若在跑)。返回 true = 确认已停(或本就没在跑)
static bool led_alarm_stop() {
    std::ifstream pf(LED_ALARM_PID_FILE);
    if (!pf.is_open()) return true;
    pid_t pid = 0;
    pf >> pid;
    pf.close();
    if (pid <= 1) return true;
    if (kill(pid, SIGTERM) == 0) {
        // 给子进程一点时间退出, 避免竞态残留
        for (int i = 0; i < 20; ++i) {
            if (kill(pid, 0) != 0) break;
            usleep(10000);
        }
        if (kill(pid, 0) == 0) kill(pid, SIGKILL); // 兜底强杀
    }
    unlink(LED_ALARM_PID_FILE);

    // 兜底: 无论 pid 文件状态如何, 都把 LED 拉回已知态, 防止爆闪残留
    write_file_string("/sys/class/leds/work/trigger", "none");
    write_file_string("/sys/class/leds/work/brightness", "0");
    return true;
}

// alarm 爆闪子进程主体: 100ms 亮 / 100ms 灭循环。
// 必须用 exec (/bin/sh 循环) 而不是在本进程内直接循环:
// fork 会继承父进程(RKLLM Agent)的全部 NPU 内存映射与设备 fd,
// 子进程不死就会把 NPU 内存钉死, 导致后续 rkllm_init 申请 205MB NPU 内存失败
// (实测 errno 14 Bad address)。exec 重建地址空间, 彻底释放继承的映射。
static const char* LED_ALARM_SH_CMD =
    "while true; do"
    " echo 255 > /sys/class/leds/work/brightness; sleep 0.1;"
    " echo 0 > /sys/class/leds/work/brightness; sleep 0.1;"
    " done";

// 启动 alarm 爆闪子进程。返回 true = 子进程成功起来并确认在闪
static bool led_alarm_start(std::string& err_node) {
    // 前置: trigger 必须先切到 none(heartbeat 等 trigger 会覆盖 brightness)
    if (!write_file_string("/sys/class/leds/work/trigger", "none")) {
        err_node = "/sys/class/leds/work/trigger";
        return false;
    }
    // 幂等: 若已有爆闪进程, 先停
    led_alarm_stop();

    pid_t pid = fork();
    if (pid < 0) {
        err_node = "fork()";
        return false;
    }
    if (pid == 0) {
        // 子进程: 脱离会话 + exec, 父进程退出后继续闪, 且不持有 NPU 映射
        setsid();
        // 【必须】把 0/1/2 全部重定向到 /dev/null:
        // 否则子进程继承父进程的 stdout/stderr 管道, 即使父进程已退出,
        // 管道写端仍被这个永不退出的 while 循环持有 → 上游(回归脚本/ssh/
        // shell 管道)永远读不到 EOF, 表现为"命令卡死不返回"直到超时强杀。
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execl("/bin/sh", "sh", "-c", LED_ALARM_SH_CMD, (char*)nullptr);
        _exit(1); // exec 失败才会到这里
    }
    // 父进程: 记录 pid, 稍等确认子进程活着
    std::ofstream pf(LED_ALARM_PID_FILE);
    pf << pid;
    pf.close();
    usleep(50000); // 50ms 后确认子进程没有立即暴毙
    if (kill(pid, 0) != 0) {
        err_node = "/sys/class/leds/work/brightness";
        unlink(LED_ALARM_PID_FILE);
        return false;
    }
    return true;
}

// JSON 字符串转义(回填给 RKLLM 的 JSON 不能出现裸引号/反斜杠)
static std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { // 其余控制字符直接丢弃, 保证 JSON 合法
                    continue;
                }
                out += (char)c;
        }
    }
    return out;
}

std::string tool_control_led(const std::string& mode_raw) {
    const std::string led_base     = "/sys/class/leds/work";
    const std::string trigger_path = led_base + "/trigger";
    const std::string bright_path  = led_base + "/brightness";
    const std::string delay_on     = led_base + "/delay_on";
    const std::string delay_off    = led_base + "/delay_off";

    // 去除首尾空白, 得到模型实际请求的模式原文
    std::string clean_mode = mode_raw;
    while (!clean_mode.empty() && (clean_mode.back() == '\n' || clean_mode.back() == '\r' || clean_mode.back() == ' ' || clean_mode.back() == '\t'))
        clean_mode.pop_back();
    while (!clean_mode.empty() && (clean_mode.front() == '\n' || clean_mode.front() == '\r' || clean_mode.front() == ' ' || clean_mode.front() == '\t'))
        clean_mode.erase(clean_mode.begin());

    std::stringstream ss;
    ss << "[{";

    // 0. --no-led 只读模式: 巡检守护拥有 LED 写权, Agent 一律拒绝且不碰任何 sysfs
    //    (放在白名单校验之前: 只读模式下即使合法模式也直接拒绝, 硬件零接触)
    if (g_no_led_mode) {
        ss << "\"success\": false, "
           << "\"error\": \"led_readonly_mode\", "
           << "\"note\": \"patrol daemon owns LED; agent is read-only\", "
           // 明确告诉模型别再换别的模式重试: 实测它会把 heartbeat/on/alarm/off 挨个试一遍,
           // 白白多跑 4 轮 NPU 推理(每轮约 10s)还污染上下文。
           << "\"hint\": \"do NOT retry control_led with another mode; "
              "the gate rejects every mode in read-only mode\"}]";
        return ss.str();
    }

    // 1. 白名单校验: 非法模式绝不写任何 sysfs
    if (clean_mode != "heartbeat" && clean_mode != "alarm" && clean_mode != "on" && clean_mode != "off") {
        ss << "\"success\": false, "
           << "\"error\": \"invalid_mode\", "
           << "\"requested_mode\": \"" << json_escape(mode_raw) << "\"";
        ss << "}]";
        return ss.str();
    }

    // 2. 逐步写入 + 检查返回值
    //    off  = trigger none    + brightness 0
    //    on   = trigger none    + brightness 255 (回读报实际值)
    //    alarm= 用户态爆闪子进程 100ms 周期翻转 brightness (本内核无 timer trigger)
    //    heartbeat = trigger heartbeat
    if (clean_mode == "alarm") {
        std::string err_node;
        if (!led_alarm_start(err_node)) {
            ss << "\"success\": false, \"error\": \"sysfs_write_failed\", \"failed_node\": \"" << err_node << "\"}]";
            return ss.str();
        }
    } else if (clean_mode == "on") {
        led_alarm_stop(); // 先停掉可能存在的爆闪, 再切模式
        // 先熄 trigger 再写亮度, 避免受旧 trigger 干扰; 默认亮度 255
        if (!write_file_string(trigger_path, "none")) {
            ss << "\"success\": false, \"error\": \"sysfs_write_failed\", \"failed_node\": \"" << trigger_path << "\"}]";
            return ss.str();
        }
        if (!write_file_string(bright_path, "255")) {
            ss << "\"success\": false, \"error\": \"sysfs_write_failed\", \"failed_node\": \"" << bright_path << "\"}]";
            return ss.str();
        }
    } else if (clean_mode == "off") {
        led_alarm_stop();
        // off = trigger none + brightness 0, 两者都写, 只清 trigger 会被 default-on 亮度残留点亮
        if (!write_file_string(trigger_path, "none")) {
            ss << "\"success\": false, \"error\": \"sysfs_write_failed\", \"failed_node\": \"" << trigger_path << "\"}]";
            return ss.str();
        }
        if (!write_file_string(bright_path, "0")) {
            ss << "\"success\": false, \"error\": \"sysfs_write_failed\", \"failed_node\": \"" << bright_path << "\"}]";
            return ss.str();
        }
    } else { // heartbeat
        led_alarm_stop();
        if (!write_file_string(trigger_path, "heartbeat")) {
            ss << "\"success\": false, \"error\": \"sysfs_write_failed\", \"failed_node\": \"" << trigger_path << "\"}]";
            return ss.str();
        }
    }

    // 3. 写后回读, 上报实际状态并与请求比对
    std::string actual_mode = get_led_current_mode();
    std::string br_str      = read_file_string(bright_path);
    int brightness = 0;
    if (!br_str.empty()) brightness = atoi(br_str.c_str());

    // 白名单语义态 -> sysfs 回读态的期望映射
    //   on  -> none(常亮靠 brightness 保持)   off -> none + 0
    //   alarm -> 爆闪子进程在跑, 回读呈 none+瞬时亮度, 以 pid 活性为准
    bool mismatch = false;
    if (clean_mode == "alarm") {
        std::ifstream pf(LED_ALARM_PID_FILE);
        pid_t apid = 0;
        pf >> apid;
        pf.close();
        if (apid <= 1 || kill(apid, 0) != 0) mismatch = true; // 爆闪进程没活着 = alarm 未生效
        if (!mismatch) {
            actual_mode = "alarm";
            brightness = -1; // 爆闪中, 瞬时亮度无意义, 用字符串报
        }
    } else {
        if (clean_mode == "heartbeat" && actual_mode != "heartbeat") mismatch = true;
        if (clean_mode == "on" && !(actual_mode == "on" && brightness > 0)) mismatch = true;
        if (clean_mode == "off" && !(actual_mode == "off" || (actual_mode == "on" && brightness == 0))) mismatch = true;
    }

    ss << "\"success\": " << (mismatch ? "false" : "true") << ", "
       << "\"requested_mode\": \"" << clean_mode << "\", "
       << "\"actual_mode\": \"" << actual_mode << "\", ";
    if (brightness >= 0) ss << "\"brightness\": " << brightness;
    else ss << "\"brightness\": \"blinking(100ms)\"";
    if (mismatch) ss << ", \"warning\": \"readback_mismatch\"";
    ss << "}]";
    return ss.str();
}

// -------------------------------------------------------------
// 工具 4: 视觉检测数据采集 (从本地 9000 端口抓取最新目标)
// -------------------------------------------------------------
#define MAX_BBOXES 10
#pragma pack(push, 1)
struct BBox {
    int32_t class_id;
    float   score;
    float   x, y, w, h;
};
struct DetectionHeader {
    uint32_t frame_id;
    uint32_t bbox_count;
};
struct FrameResult {
    DetectionHeader header;
    BBox boxes[MAX_BBOXES];
};
#pragma pack(pop)

static const char* COCO80_NAMES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
    "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant", "bed",
    "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave", "oven",
    "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
};

// 从 9000 网关收满一帧 248 字节 FrameResult
// 返回值: 0=收满成功; -1=连接失败; -2=收满前超时; -3=收满前连接断开(半帧)
static int recv_full_frame(FrameResult& frame) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 80000; // 80ms 单次 recv 超时
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return -1; // 网关离线
    }

    // 循环 recv 直到收满 sizeof(FrameResult), 防止单次 recv 拿到半帧
    char* buf = reinterpret_cast<char*>(&frame);
    size_t total = 0;
    while (total < sizeof(FrameResult)) {
        ssize_t n = recv(sock, buf + total, sizeof(FrameResult) - total, 0);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) { // 对端 orderly shutdown -> 半帧
            close(sock);
            return -3;
        }
        // n < 0: 超时(EAGAIN) 或真错误
        close(sock);
        if (total > 0) return -3; // 收了一半断流/超时
        return -2;                // 一字节都没收到, 视为超时
    }
    close(sock);
    return 0;
}

std::string tool_get_vision_telemetry() {
    FrameResult f1;
    int r1 = recv_full_frame(f1);
    if (r1 == -1) {
        return "[{\"status\": \"offline\", \"msg\": \"EdgeVision NPU vision gateway (port 9000) is currently offline or standby\"}]";
    }
    if (r1 == -2) {
        return "[{\"status\": \"timeout\", \"msg\": \"Connected to vision gateway but no frame data arrived within timeout\"}]";
    }
    if (r1 == -3) {
        return "[{\"status\": \"partial_frame\", \"msg\": \"Frame stream broke before a full 248-byte frame arrived\"}]";
    }

    // bbox_count 越界校验: 超过 MAX_BBOXES 视为畸形帧, 只报头不读框, 严禁越界读
    uint32_t raw_count = f1.header.bbox_count;
    if (raw_count > MAX_BBOXES) {
        std::stringstream ss;
        ss << "[{\"status\": \"malformed_frame\", "
           << "\"frame_id\": " << f1.header.frame_id << ", "
           << "\"bbox_count\": " << raw_count
           << ", \"msg\": \"bbox_count exceeds protocol limit, detections dropped\"}]";
        return ss.str();
    }

    // 双采样判新鲜: 立即重连再抓一帧, 比较 frame_id
    //   两次成功且 frame_id 相同 -> fresh=false (网关可能停推)
    //   两次成功且 frame_id 不同 -> fresh=true  (数据流活跃)
    //   第二次失败              -> fresh=unknown (以第一次为准)
    FrameResult f2;
    int r2 = recv_full_frame(f2);
    const char* fresh = "unknown";
    if (r2 == 0) {
        fresh = (f2.header.frame_id != f1.header.frame_id) ? "true" : "false";
    }

    std::stringstream ss;
    ss << "[{"
       << "\"status\": \"online\", "
       << "\"frame_id\": " << f1.header.frame_id << ", "
       << "\"fresh\": \"" << fresh << "\", "
       << "\"detected_count\": " << raw_count << ", "
       << "\"detections\": [";
    for (uint32_t i = 0; i < raw_count; ++i) {
        if (i > 0) ss << ", ";
        int cid = f1.boxes[i].class_id;
        std::string name = (cid >= 0 && cid < 80) ? COCO80_NAMES[cid] : "unknown";
        ss << "{\"class\": \"" << name << "\", \"confidence\": " << f1.boxes[i].score
           << ", \"x\": " << f1.boxes[i].x << ", \"y\": " << f1.boxes[i].y
           << ", \"w\": " << f1.boxes[i].w << ", \"h\": " << f1.boxes[i].h << "}";
    }
    ss << "]}]";
    return ss.str();
}

// -------------------------------------------------------------
// 工具 5: 读取巡检守护事件日志 (task-6: Agent 只读不写灯, 研判靠读守护黑匣子)
//   来源: /root/patrol/patrol_events.log (黑匣子全量)
//        /root/patrol/llm_pending.jsonl  (慢层契约, 仅 PERSON_ALERT/THERMAL_ALERT/RECOVER)
//   日志可能被 logrotate 轮转成 .1 (每份可达 5MB+), 因此严禁整文件读进内存:
//   用 seekg 从文件尾部只回退 (tail_n_lines * MAX_LINE_BYTES) 字节再取尾部 N 行。
//   两文件按 ts 排序合并去重后取最近 tail 条, 每条输出 ts/type/summary。
// -------------------------------------------------------------
#define PATROL_EVENT_LOG  "/root/patrol/patrol_events.log"
#define PATROL_LLM_LOG    "/root/patrol/llm_pending.jsonl"
#define PATROL_MAX_LINE_BYTES 1024 // 单行超过 1KB 视为畸形截断, 跳过

// 从文件尾部取最后 n 行(可能不足)。文件不存在/为空返回空列表。
// 只回退 n * PATROL_MAX_LINE_BYTES 字节, 内存占用 O(n), 与文件总大小无关。
static std::vector<std::string> read_tail_lines(const char* path, int n) {
    std::vector<std::string> lines;
    if (n <= 0) return lines;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return lines; // 文件不存在: 调用方报 "no patrol events yet"

    f.seekg(0, std::ios::end);
    std::streamoff fsize = f.tellg();
    if (fsize <= 0) return lines;

    std::streamoff back_bytes = (std::streamoff)n * PATROL_MAX_LINE_BYTES;
    if (back_bytes > fsize) back_bytes = fsize;
    f.seekg(fsize - back_bytes, std::ios::beg);

    // 若回退点落在行中间, 丢弃第一个不完整行(读到第一个 \n 之前的部分)
    if (back_bytes < fsize) {
        std::string partial;
        std::getline(f, partial); // 丢弃
    }

    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r'))
            line.pop_back();
        if (line.empty() || line.size() >= PATROL_MAX_LINE_BYTES) continue; // 畸形行跳过
        lines.push_back(line);
    }
    if ((int)lines.size() > n)
        lines.erase(lines.begin(), lines.end() - n);
    return lines;
}

// 极简 JSON 行字段抽取: 找 "key":"value" 或 "key":123, 返回值字符串。
// 守护日志为受控机器生成(固定键序/无嵌套转义同名键), 字符串搜索足够可靠。
static std::string extract_json_field(const std::string& line, const char* key) {
    std::string pat = "\"" + std::string(key) + "\":";
    size_t p = line.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    if (p < line.size() && line[p] == '"') {
        size_t e = line.find('"', p + 1);
        if (e == std::string::npos) return "";
        return line.substr(p + 1, e - p - 1);
    }
    // 数值/布尔/null 字面量
    size_t e = line.find_first_of(",}", p);
    if (e == std::string::npos) e = line.size();
    std::string v = line.substr(p, e - p);
    while (!v.empty() && (v.front() == ' ')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ')) v.pop_back();
    return v;
}

std::string tool_get_patrol_events(const std::string& tail_raw) {
    // 参数 tail: 默认 10, 上限 50, 非法值回退默认
    int tail_n = 10;
    {
        std::string t = tail_raw;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '\t'))
            t.pop_back();
        while (!t.empty() && (t.front() == '\n' || t.front() == '\r' || t.front() == ' ' || t.front() == '\t'))
            t.erase(t.begin());
        if (!t.empty()) {
            int v = atoi(t.c_str());
            if (v > 0) tail_n = v;
        }
        if (tail_n > 50) tail_n = 50;
    }

    // 两路日志合并: patrol_events.log 的 "event" 字段映射为统一的 "type"
    std::vector<std::string> raw = read_tail_lines(PATROL_EVENT_LOG, tail_n);
    std::vector<std::string> raw_llm = read_tail_lines(PATROL_LLM_LOG, tail_n);
    raw.insert(raw.end(), raw_llm.begin(), raw_llm.end());

    if (raw.empty()) {
        return "[{\"note\": \"no patrol events yet\"}]";
    }

    // 按 ts 字符串排序(ISO 格式字典序即时间序), 取最近 tail_n 条, 旧的在前
    std::sort(raw.begin(), raw.end(), [](const std::string& a, const std::string& b) {
        return extract_json_field(a, "ts") < extract_json_field(b, "ts");
    });
    if ((int)raw.size() > tail_n)
        raw.erase(raw.begin(), raw.end() - tail_n);

    std::stringstream ss;
    ss << "[";
    bool first = true;
    // Observation 体积熔断: 回填给模型的 JSON 超过 1500 字节就丢弃更老的行,
    // 否则多工具轮次里事件列表会把 8192 上下文里的历史/汇报挤爆(context not enough)
    const size_t PATROL_OBS_MAX_BYTES = 1500;
    size_t used_bytes = 1; // 已含开头的 '['
    for (const auto& line : raw) {
        std::string ts      = extract_json_field(line, "ts");
        std::string type    = extract_json_field(line, "type");
        std::string summary = extract_json_field(line, "summary");
        if (type.empty()) type = extract_json_field(line, "event"); // 黑匣子行用 event 键
        if (summary.empty()) summary = extract_json_field(line, "detail");
        if (!summary.empty() && summary.front() == '{')
            summary = ""; // detail 是嵌套对象, 拍平无意义(如 DAEMON_START), 置空留给 type 表意
        if (summary.size() > 200) summary = summary.substr(0, 200); // 控制回填体积
        if (ts.empty() && type.empty()) continue; // 既无 ts 也无 type 的行丢弃

        std::stringstream item;
        item << ((first) ? "" : ", ")
             << "{\"ts\": \"" << json_escape(ts) << "\", "
             << "\"type\": \"" << json_escape(type) << "\", "
             << "\"summary\": \"" << json_escape(summary) << "\"}";
        if (used_bytes + item.str().size() + 1 > PATROL_OBS_MAX_BYTES) {
            ss << ", {\"note\": \"older events truncated to fit context\"}";
            break; // 保留已拼好的较新行, 更老的全部丢弃
        }
        ss << item.str();
        used_bytes += item.str().size() + 1;
        first = false;
    }
    ss << "]";
    if (first) return "[{\"note\": \"no patrol events yet\"}]"; // 全是畸形行或首条就超限
    return ss.str();
}

// -------------------------------------------------------------
// 具身调度器: 解析模型输出中的所有 <tool_call> 并批量执行
// -------------------------------------------------------------
bool dispatch_all_tool_calls(const std::string& model_output, std::string& combined_observation_json,
                             std::string* tools_sig = nullptr) {
    size_t pos = 0;
    std::vector<std::string> results;

    while (true) {
        size_t call_start = model_output.find("<tool_call>", pos);
        if (call_start == std::string::npos) break;

        size_t fn_start = model_output.find("<function=", call_start);
        if (fn_start == std::string::npos) break;
        fn_start += 10;
        size_t fn_end = model_output.find(">", fn_start);
        if (fn_end == std::string::npos) break;

        std::string tool_name = model_output.substr(fn_start, fn_end - fn_start);

        size_t call_end = model_output.find("</tool_call>", fn_end);
        if (call_end == std::string::npos) call_end = model_output.length();

        // 提取该 tool_call 内部的参数
        std::string call_body = model_output.substr(fn_end, call_end - fn_end);
        std::string param_mode = "heartbeat";
        size_t p_start = call_body.find("<parameter=mode>");
        if (p_start != std::string::npos) {
            p_start += 16;
            size_t p_end = call_body.find("</parameter>", p_start);
            if (p_end != std::string::npos) {
                param_mode = call_body.substr(p_start, p_end - p_start);
                // 模型输出常把参数值包在换行里(如 "\nalarm\n"), 必须去首尾空白:
                // ① 否则打印成 "mode = " 换行接 "alarm", 人/回归脚本都读不到值;
                // ② 白名单比较也会因 "\nalarm\n" != "alarm" 被误判成非法模式。
                size_t b = param_mode.find_first_not_of(" \t\r\n");
                size_t e = param_mode.find_last_not_of(" \t\r\n");
                param_mode = (b == std::string::npos)
                                 ? std::string()
                                 : param_mode.substr(b, e - b + 1);
            }
        }

        std::string param_tail = "10"; // get_patrol_events 默认取最近 10 条
        size_t t_start = call_body.find("<parameter=tail>");
        if (t_start != std::string::npos) {
            t_start += 16;
            size_t t_end = call_body.find("</parameter>", t_start);
            if (t_end != std::string::npos) {
                param_tail = call_body.substr(t_start, t_end - t_start);
                size_t b = param_tail.find_first_not_of(" \t\r\n");
                size_t e = param_tail.find_last_not_of(" \t\r\n");
                param_tail = (b == std::string::npos)
                                 ? std::string("10")
                                 : param_tail.substr(b, e - b + 1);
            }
        }

        std::cout << "\n========================================================" << std::endl;
        std::cout << "⚙️  [Agent 决策] 触发物理硬件工具: " << tool_name << std::endl;

        std::string obs;
        if (tool_name == "get_system_health") {
            obs = tool_get_system_health();
        } else if (tool_name == "get_led_status") {
            obs = tool_get_led_status();
        } else if (tool_name == "control_led") {
            std::cout << "   -> 设定参数: mode = " << param_mode << std::endl;
            obs = tool_control_led(param_mode);
        } else if (tool_name == "get_vision_telemetry") {
            obs = tool_get_vision_telemetry();
        } else if (tool_name == "get_patrol_events") {
            std::cout << "   -> 设定参数: tail = " << param_tail << std::endl;
            obs = tool_get_patrol_events(param_tail);
        } else {
            obs = "[{\"error\": \"Unknown tool requested: " + tool_name + "\"}]";
        }

        std::cout << "🔍 [硬件感知 (Observation)] 返回: " << obs << std::endl;
        std::cout << "========================================================\n" << std::endl;

        // 记录本轮实际触发的工具+参数签名(空转判定用)
        if (tools_sig) {
            if (!tools_sig->empty()) *tools_sig += "|";
            *tools_sig += tool_name + ":" + (tool_name == "control_led" ? param_mode
                                             : (tool_name == "get_patrol_events" ? param_tail : ""));
        }

        // 去掉前后方括号，提取对象内容以便合并
        if (!obs.empty() && obs.front() == '[' && obs.back() == ']') {
            obs = obs.substr(1, obs.length() - 2);
        }
        results.push_back(obs);

        pos = call_end + 12;
    }

    if (results.empty()) return false;

    // 组装成一个合法的 JSON 数组回传给 RKLLM
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << results[i];
    }
    ss << "]";
    combined_observation_json = ss.str();
    return true;
}

// -------------------------------------------------------------
// Agent 交互核心流程 (带 ReAct 自主多步循环)
// -------------------------------------------------------------
void run_agent_turn(LLMHandle handle, RKLLMInferParam& infer, const std::string& user_query) {
    std::cout << "\n========================================================" << std::endl;
    std::cout << "👤 [User]: " << user_query << std::endl;
    std::cout << "🤖 [Edge Agent 思考与决策]:" << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;

    g_step_output.clear();
    RKLLMInput input{};
    input.role = "user";
    input.input_type = RKLLM_INPUT_PROMPT;
    input.prompt_input = user_query.c_str();
    rkllm_run(handle, &input, &infer, nullptr);

    // ReAct 递归多步循环: 只要模型还在请求 Tool Call，就持续执行并回填！
    int max_steps = 5;
    int stall_rounds = 0;            // 连续没有触发新工具调用的轮数
    std::string last_tools_sig;      // 上一轮实际触发的工具+参数签名
    while (max_steps-- > 0) {
        std::string observation_json;
        std::string this_tools_sig;  // 本轮实际触发的工具+参数签名(用于连续空转判定)
        if (dispatch_all_tool_calls(g_step_output, observation_json, &this_tools_sig)) {
            // 连续两轮触发的是同一批相同工具+相同参数 → 模型在原地打转, 提前收束
            if (!this_tools_sig.empty() && this_tools_sig == last_tools_sig) {
                stall_rounds++;
            } else {
                stall_rounds = 0;
            }
            last_tools_sig = this_tools_sig;

            std::cout << "🤖 [Edge Agent 推进决策 / 终局汇报]:" << std::endl;
            std::cout << "--------------------------------------------------------" << std::endl;

            g_step_output.clear();
            RKLLMInput tool_input{};
            tool_input.role = "tool";
            tool_input.input_type = RKLLM_INPUT_PROMPT;
            tool_input.prompt_input = observation_json.c_str();
            rkllm_run(handle, &tool_input, &infer, nullptr);

            if (stall_rounds >= 2) {
                std::cout << "⚠️  [Agent] 连续两轮重复调用相同工具+相同参数, 疑似原地打转, 提前收束。" << std::endl;
                break;
            }
        } else {
            // 没有更多的 Tool Call，模型已经输出终局自然语言回答
            break;
        }
    }

    // 轮数耗尽但模型输出里仍有 <tool_call>: 说明任务没有走完, 明确警告并停住, 不再执行
    if (g_step_output.find("<tool_call>") != std::string::npos) {
        std::cout << "\n⚠️  [Agent] 工具执行轮数已达上限，任务未完成。" << std::endl;
        std::cout << "    (已连续执行 5 轮工具调用仍收到新的 <tool_call> 请求, 已停止调度, 避免无限循环)" << std::endl;
    }
}

int main(int argc, char** argv) {
    std::cout << "========================================================\n"
              << "🚀 RK3588 纯端侧原生具身智能体 (On-Device Embodied Agent)\n"
              << "   - 大脑底座: Qwen3.5-2B (W8A8 高精度量化, 3核物理 NPU)\n"
              << "   - 神经手脚: Linux sysfs / DRM / POSIX Socket 原生闭环\n"
              << "========================================================" << std::endl;

    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = "qwen3.5-2b_w8a8_rk3588.rkllm";
    param.max_new_tokens = 512;
    // task-6: 多步研判一轮会回填 3~4 个工具 Observation(事件列表可达 1.5KB),
    // 2048 上下文直接把终局汇报挤爆(The context is not enough!), 提到 8192 (模型头上限 16384)
    param.max_context_len = 8192;

    // --no-led 只读模式: 从 argv 剥离该开关(可出现在任意位置), 其余参数仍作为查询
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-led") {
            g_no_led_mode = true;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (g_no_led_mode) {
        std::cout << "[Mode] --no-led 只读模式: 巡检守护拥有 LED 写权, 本进程 control_led 只返回"
                     " led_readonly_mode 拒绝且绝不触碰 sysfs。" << std::endl;
    }

    RKLLMCallback cb = {};
    cb.result_callback = callback;
    LLMHandle handle = nullptr;

    std::cout << "[Init] 加载端侧大模型并绑定 NPU 3核与 A76 大核..." << std::endl;
    int ret = rkllm_init(&handle, &param, &cb);
    if (ret != 0) {
        std::cerr << "rkllm_init failed: " << ret << std::endl;
        return 1;
    }

    const char* sys_prompt_base = 
        "你是一个部署在 Rockchip RK3588 边缘嵌入式计算设备上的智能具身运维 Agent。\n"
        "【硬件规格】:\n"
        "- 处理器: 8核 ARM (4x Cortex-A76 @ 2.4GHz + 4x Cortex-A55 @ 1.8GHz)\n"
        "- NPU: 3核独立神经算力核心，总峰值 6.0 TOPS，当前通过 rkllm-runtime v1.3.1 运行大模型\n"
        "- 内存: 8GB LPDDR4x，操作系统: Linux Debian 11 (bullseye), 内核 5.10.110\n"
        "- 外设: 板载工作指示灯 /sys/class/leds/work/，双目 UVC 相机 (/dev/video81, /dev/video83)\n"
        "- 视觉系统: 后台作为守护进程运行 EdgeVision YOLOv8 工业检测服务 (端口 9000)\n"
        "- 温度常识: RK3588 的 SOC 正常工作温度为 30~65°C，超过 75°C 才算过热告警，"
        "85°C 以上才需要担心降频/损坏。若巡检事件的温度阈值明显偏低(如 20°C)，"
        "说明守护进程是用评测阈值在跑验收，**不要据此判定设备真的过热**，"
        "应指出\"当前温度在正常范围内，告警是低阈值触发的演示\"。\n"
        "【工具调用准则】:\n"
        "- 查询指示灯状态请调用 get_led_status，改变指示灯才调用 control_led。\n"
        "- 【最高优先级】当用户明确要求把灯调成某个模式（如心跳呼吸/报警/常亮/关闭）时，"
        "这是用户的直接指令，必须立即调用 control_led 并把 mode 设为用户要求的值，"
        "即使查询到当前状态接近或相同也要显式执行（幂等写），绝不能因为\"当前已经是这样\"而跳过；"
        "也不要参考温度、巡检事件等其他信息来更改用户指定的模式。\n"
        "- 查询整体硬件健康和温度请调用 get_system_health。\n"
        "- 查询画面识别目标请调用 get_vision_telemetry。\n"
        "- 查询巡检守护产生的硬件告警事件请调用 get_patrol_events。\n"
        "- 【重要】遇到条件指令或需要多步研判的任务，必须先调用 get_system_health、"
        "get_vision_telemetry、get_patrol_events 等感知工具拿到全量真实数据，再决策下一步动作；"
        "绝对不能跳过感知直接执行动作。\n"
        "- 【重要】研判类任务（如询问设备是否发生过异常、综合巡检）应优先读取巡检事件"
        "进行研判并如实汇报；但当用户明确下达指示灯控制指令（如恢复心跳呼吸灯、调成报警、"
        "关闭指示灯）时，这是用户的直接指令，必须立即调用 control_led 按用户要求的模式执行，"
        "不要被巡检事件日志内容干扰或改判模式；告警时的灯自动控制由巡检守护进程负责。\n"
        "- 【最高优先级】条件指令的条件必须实测验证：条件里提到什么数据源（画面/温度/事件等），"
        "就必须调用对应的工具拿到实时结果来判断，绝不能用巡检历史事件或推测替代实测"
        "（如\"画面里有人就报警\"必须先调 get_vision_telemetry 查看当前画面，"
        "巡检事件里没有人员记录不代表画面里现在没有人）；巡检事件只用于研判过去，"
        "不能代替当前实时感知，也不能以\"守护进程已处理\"为由跳过用户要求且条件未触发的动作。\n"
        "- 严禁捏造虚假硬件数据，用流畅精炼的中文汇报。\n"
        "- 汇报时只陈述工具真实返回的字段，**不要臆测工具的内部实现**"
        "（如猜测 control_led 的枚举值编码、猜测 sysfs 节点的数值含义）。"
        "拿不准的就说不确定或直接省略，不要编一段\"若系统内部逻辑为0=off…\"之类的推测。\n"
        "- 温度是否异常要按上面的温度常识判断：40~55°C 属于正常工作温度，"
        "不要因为看到历史告警就把正常温度描述成\"长期偏高、存在永久损坏风险\"。\n"
        "- 【重要】若工具返回 success:false 或任何 error(如 led_readonly_mode/invalid_mode)，"
        "必须在汇报中如实告知用户该操作已被拒绝及原因，绝对不能声称操作已成功执行。";

    // --no-led 模式下追加最高优先级约束: 控制权在巡检守护, 模型不该再去试(实测会连试4种模式)
    const char* sys_prompt_noled =
        "\n- 【只读模式·最高优先级】当前运行在 --no-led 只读巡检模式，指示灯的控制权归属于"
        "常驻的巡检守护进程(它会根据温度/人员自动控制灯光)。因此你绝对不要调用 control_led，"
        "也不要尝试用任何方式改变指示灯；只需调用 get_led_status / get_system_health / "
        "get_patrol_events 读取真实状态并如实汇报即可。若用户要求你改灯，"
        "请说明\"指示灯当前由巡检守护接管，Agent 为只读模式\"，并建议用户停止守护后再手动控制。";

    std::string sys_prompt_holder = std::string(sys_prompt_base);
    if (g_no_led_mode) sys_prompt_holder += sys_prompt_noled;
    const char* sys_prompt = sys_prompt_holder.c_str();

    const char* tools_json = R"RAW([
      {
        "type": "function",
        "function": {
          "name": "get_system_health",
          "description": "获取开发板当前的物理温度(SOC/CPU/NPU各温区)、NPU三核实时利用率、可用内存及指示灯当前状态",
          "parameters": {
            "type": "object",
            "properties": {}
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "get_led_status",
          "description": "查询板载物理 LED 指示灯当前的工作模式(心跳heartbeat、急闪alarm、常亮on、关闭off)与亮度",
          "parameters": {
            "type": "object",
            "properties": {}
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "control_led",
          "description": "改变或控制开发板上的板载物理 LED 指示灯工作模式。仅接受 heartbeat/alarm/on/off 四个值，其他任何值都将被拒绝(返回 invalid_mode 错误且不会改动硬件)。alarm 为 100ms 周期爆闪报警",
          "parameters": {
            "type": "object",
            "properties": {
              "mode": {
                "type": "string",
                "enum": ["heartbeat", "alarm", "on", "off"],
                "description": "LED目标模式: heartbeat(正常呼吸闪烁), alarm(100ms紧急急闪), on(常亮), off(关闭)"
              }
            },
            "required": ["mode"]
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "get_vision_telemetry",
          "description": "获取边缘 AI 视觉检测网关当前检测到的现场画面目标，返回最新检测框坐标(x/y/w/h)、类别、置信度与数据新鲜度(fresh/frame_id)",
          "parameters": {
            "type": "object",
            "properties": {}
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "get_patrol_events",
          "description": "读取巡检守护进程产生的硬件告警事件日志，用于研判设备近期是否发生过异常(人员闯入/过热/恢复等)。返回最近的按时间排序的事件列表(ts/type/summary)",
          "parameters": {
            "type": "object",
            "properties": {
              "tail": {
                "type": "string",
                "description": "可选, 读取最近多少条事件, 默认 10, 最大 50"
              }
            }
          }
        }
      }
    ])RAW";

    ret = rkllm_set_function_tools(handle, sys_prompt, tools_json, "<tool_response>");
    if (ret != 0) {
        std::cerr << "rkllm_set_function_tools failed: " << ret << std::endl;
    }

    RKLLMInferParam infer{};
    infer.mode = RKLLM_INFER_GENERATE;
    infer.keep_history = 1; // 多轮会话记忆

    // 1. 如果指定了命令行参数，执行该单轮查询 (--no-led 已剥离)
    if (!args.empty()) {
        std::string query = args[0];
        run_agent_turn(handle, infer, query);
    } else {
        // 2. 无参数时，进入常驻交互式 REPL 控制台！
        std::cout << "\n========================================================" << std::endl;
        std::cout << "💬 已就绪！进入纯端侧 Agent 常驻交互控制台 (秒级响应)" << std::endl;
        std::cout << "   - 输入自然语言直接与智能体对话 (如: '指示灯是什么状态', '开发板运行怎么样')" << std::endl;
        std::cout << "   - 特殊指令: '/clear' 清空上下文, 'exit' 或 'quit' 退出" << std::endl;
        std::cout << "========================================================" << std::endl;

        std::string line;
        while (true) {
            std::cout << "\nEdgeAgent> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            
            // 去除空白
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;

            if (line == "exit" || line == "quit" || line == "/quit" || line == "q") {
                std::cout << "正在退出 Agent 控制台..." << std::endl;
                break;
            }
            if (line == "/clear" || line == "/reset") {
                rkllm_clear_kv_cache(handle, 1, nullptr, nullptr);
                std::cout << "🧹 上下文缓存已清空，系统 Prompt 已保留。" << std::endl;
                continue;
            }

            run_agent_turn(handle, infer, line);
        }
    }

    rkllm_destroy(handle);
    std::cout << "\n[Agent] 会话结束，NPU 上下文已优雅释放。" << std::endl;
    return 0;
}
