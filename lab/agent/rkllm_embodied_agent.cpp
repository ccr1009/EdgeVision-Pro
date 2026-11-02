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
#include <signal.h>   // sigaction (POSIX, C++ <csignal> 不保证提供)
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

// 预热时抑制输出：预热只为把 system prompt 编码进 KV cache，不需要看模型回答
static bool g_quiet = false;

// SIGTERM/SIGINT → 只置标志，让主循环自然退出。
//
// 为什么必须有这个：rkllm 释放 NPU 那 2 GB 映射靠的是 rkllm_destroy(handle)。
// 没有 handler 时，进程收到 SIGTERM 会被内核直接干掉，永远走不到 destroy，
// IOVA 映射就留在驱动里 —— 表现就是"用完 kill 一下，下次启动 errno 14"，
// 只能重启板子复位（这个坑今晚踩了三次）。
static volatile sig_atomic_t g_stop = 0;
static void on_term_signal(int) { g_stop = 1; }

// 每轮对话里已执行过几次 control_service(start)。
// 用途：拦住模型"顺手把别的服务也启动"的越权行为 ——
// 实测它会把"打开看板"扩展成"看板 + 巡检守护"一起启动。
// prompt 里写了约束但它仍会犯，所以在代码层硬拦：一轮最多启动一个服务。
static int g_start_in_turn = 0;

// 本轮是否已经在操作"服务"。用于拦住模型的语义泛化：
// 实测说"打开车辆看板"时它会同时调 control_led(mode=heartbeat) ——
// 因为它把"打开"理解成了"开灯"。灯控和服务控制是两件完全不同的事，
// 一轮里既然在处理服务请求，就不该再动灯。
static bool g_service_in_turn = false;

int callback(RKLLMResult *result, void *userdata, LLMCallState state) {
    if (state == RKLLM_RUN_NORMAL) {
        if (!g_quiet) std::cout << result->text << std::flush;
        g_step_output += result->text;
    } else if (state == RKLLM_RUN_FINISH) {
        if (!g_quiet) {
            std::cout << std::endl;
            // 把 prefill 和 generate 分开打点。
            // 这两个数字必须分开看：prefill 反映"读进去多快"（受 prompt 长度主导），
            // generate 反映"吐出来多快"（受 NPU 算力和争用主导）。
            // 混在一起说"响应变快了"会让人误以为是吞吐提升，实际上多数时候只是工作量减少。
            const RKLLMPerfStat& p = result->perf;
            if (p.prefill_tokens > 0 || p.generate_tokens > 0) {
                double pf = (p.prefill_time_ms > 0.01f)
                                ? p.prefill_tokens * 1000.0 / p.prefill_time_ms : 0.0;
                double dg = (p.generate_time_ms > 0.01f)
                                ? p.generate_tokens * 1000.0 / p.generate_time_ms : 0.0;
                std::cout << "[perf] prefill " << (int)p.prefill_time_ms << " ms / "
                          << p.prefill_tokens << " tok = " << (int)pf << " tok/s"
                          << "  |  generate " << (int)p.generate_time_ms << " ms / "
                          << p.generate_tokens << " tok = " << (int)dg << " tok/s"
                          << "  |  peak " << (int)p.memory_usage_mb << " MB"
                          << std::endl;
            }
        }
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

// -------------------------------------------------------------
// UTF-8 合法性校验与清洗
// 背景（2026-10-29 实测）：Agent 的 stdin 来自终端，若客户端终端编码不是 UTF-8
// （Windows PowerShell 默认 GBK），中文会以非法字节序列送达。rkllm 的 tokenizer
// 收到非法 UTF-8 会抛 std::invalid_argument，此前无人捕获 → terminate / Aborted。
// 现象：输入 "led改为常亮" 回显成 "?led改为常?亮"，紧接着进程 abort。
// 结论：外部输入一律不可信。这里把非法字节替换为 '?'，并把"发生过替换"告诉调用方，
//       让它能提示用户去改终端编码，而不是静默带着乱码去推理。
// -------------------------------------------------------------
static bool sanitize_utf8(std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool changed = false;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        size_t len = 0;
        if (c < 0x80)                 len = 1;                              // ASCII
        else if ((c & 0xE0) == 0xC0)  len = 2;
        else if ((c & 0xF0) == 0xE0)  len = 3;
        else if ((c & 0xF8) == 0xF0)  len = 4;
        else { changed = true; out += '?'; ++i; continue; }                 // 非法首字节

        if (i + len > s.size()) { changed = true; out += '?'; break; }      // 截断的多字节
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = false; break; }  // 续字节不合法
        }
        if (!ok) { changed = true; out += '?'; ++i; continue; }

        out.append(s, i, len);
        i += len;
    }
    s.swap(out);
    return changed;
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
       // 这里刻意不再输出 normal_optimal / high_temperature_warning 这类「结论」字段。
       // 原因: 旧版写死 soc_temp > 75.0 判定, 而告警阈值实际由巡检守护的命令行参数决定
       // (验收时是 20C)。二者不一致时, 模型只看到 status=normal_optimal 就直接回
       // 「温度正常所以不会报警」, 把一个错误前提当成了事实。
       // 现在只给原始温度, 并把「会不会报警」明确指给 get_patrol_status。
       << "\"temp_warning_note\": \"本字段只给原始温度读数, 不含告警判断。"
       << "是否触发告警取决于巡检守护当前生效阈值, 请调用 get_patrol_status 查询。\""
       << "}]";
    return ss.str();
}

// -------------------------------------------------------------
// 工具 2: 查询当前 LED 状态 (读操作)
// -------------------------------------------------------------
// 采样一个时间窗内「亮」的比例(0~100%)。
// 为什么不能直接回填瞬时 brightness: heartbeat 由内核 trigger 周期性翻转,
// 实测占空比只有 20~25%(灭相占 75% 以上), 单点读 0 的概率约 78%。
// 旧版直接把裸 brightness 给模型, 模型据此答「亮度 0（关闭）」,
// 与同一句里的「正常呼吸闪烁」自相矛盾 —— 这是采样假象, 不是灯灭了。
// 采样窗必须 >= 1.2 个最慢周期(heartbeat 约 1s): 20 x 60ms = 1.2s。
// 判读参考(实测): heartbeat 20~25% / 用户态爆闪约 50% / 常亮 100% / 关闭 0%。
static int sample_brightness_duty_pct(int samples, int interval_us) {
    int hi = 0;
    for (int i = 0; i < samples; ++i) {
        std::string s = read_file_string("/sys/class/leds/work/brightness");
        if (!s.empty() && s != "0") hi++;
        usleep(interval_us);
    }
    if (samples <= 0) return -1;
    return (int)((long)hi * 100 / samples);
}

std::string tool_get_led_status() {
    std::string mode = get_led_current_mode();
    std::string br = read_file_string("/sys/class/leds/work/brightness");

    // 周期驱动的模式(心跳/爆闪)只报占空比; 静态模式(常亮/关闭)才报瞬时亮度。
    bool periodic = (mode == "heartbeat" || mode == "alarm" || mode == "alarm_blink");
    int duty = periodic ? sample_brightness_duty_pct(20, 60000) : -1;

    std::string desc = (mode == "heartbeat" ? "正常呼吸闪烁"
                      : (mode == "alarm" ? "100ms急闪报警"
                      : (mode == "alarm_blink" ? "100ms急闪报警(用户态爆闪运行中)"
                      : (mode == "on" ? "常亮模式" : "熄灭关闭"))));

    std::stringstream ss;
    ss << "[{"
       << "\"led_name\": \"work\", "
       << "\"current_mode\": \"" << mode << "\", "
       << "\"description\": \"" << desc << "\", ";
    if (periodic) {
        ss << "\"brightness_duty_pct\": " << duty << ", "
           << "\"brightness_note\": \"该模式由内核触发器/用户态爆闪周期性翻转, 瞬时亮度读数为 0 属正常"
              "(心跳的灭相占 75% 以上), 不代表关闭。判定请看 brightness_duty_pct: "
              "心跳约 20~25%, 爆闪报警约 50%, 常亮 100%, 关闭 0%。\"";
    } else {
        ss << "\"brightness\": " << (br.empty() ? "0" : br);
    }
    ss << "}]";
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
    // 跳过多余空白：JSON 里常见 "key": "value"（冒号后有空格）。
    // 不跳过的话 line[p] 是空格而不是引号，会误走下面的"数值字面量"分支，
    // 提取出带引号的原值（"heartbeat"），导致按值分支判断全部失效。
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
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

// -------------------------------------------------------------
// 工具 6: 读取巡检守护的「运行态」 (读操作)
//   与 get_patrol_events 的分工:
//     - get_patrol_events: 过去发生过什么(黑匣子/历史), 只能研判历史
//     - get_patrol_status: 现在谁在控灯、阈值多少、还活着吗(运行态), 决定「会不会报警」
//   为什么必须单开一个工具: 没有它时 Agent 只能拿到历史事件里那句「超过20C阈值」,
//   却无法区分这条是「正在生效」还是「三天前的旧记录」, 于是退回去用 prompt 里写死的
//   75C 默认值当当前阈值 —— 实测因此给出「44C 远低于 75C, 不会报警」的错误结论,
//   而当时守护正是用 20C 阈值在跑, 灯其实应该在闪。
// -------------------------------------------------------------
#define PATROL_STATE_FILE "/root/patrol/patrol_state.json"

static std::string tool_get_patrol_status() {
    std::string raw = read_file_string(PATROL_STATE_FILE);
    if (raw.empty()) {
        return "[{\"running\": false, \"reason\": \"state_file_missing\", "
               "\"verdict\": \"巡检守护从未启动或快照未生成, 当前没有任何进程会自动控制 LED, "
               "因此不会因温度/人员自动报警。\"}]";
    }

    // 新鲜度: updated_at 距今超过 stale_after_s 即判定失联。
    // 守护正常运行时每周期刷新一次; 被 kill -9 后文件就停住不再更新,
    // 这是「进程已死」唯一可靠的信号(锁文件也会随内核释放, 但没有时间戳可比)。
    // 手写 sscanf 解析而不上 strptime: strptime 要 _XOPEN_SOURCE 宏, 板上 g++ 下易踩未声明。
    long age_s = -1;
    std::string upd = extract_json_field(raw, "updated_at");
    if (!upd.empty()) {
        int Y = 0, M = 0, D = 0, h = 0, mi = 0, se = 0;
        if (sscanf(upd.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &se) == 6) {
            struct tm tmv;
            memset(&tmv, 0, sizeof(tmv));
            tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
            tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = se;
            tmv.tm_isdst = -1;
            time_t t_event = mktime(&tmv);
            if (t_event != (time_t)-1) age_s = (long)(time(nullptr) - t_event);
        }
    }

    long stale_after = 15; // 兜底: 快照里没写就按 15s 判
    std::string sa = extract_json_field(raw, "stale_after_s");
    if (!sa.empty()) { long v = atol(sa.c_str()); if (v > 0) stale_after = v; }

    bool file_running = (extract_json_field(raw, "running").find("true") != std::string::npos);
    bool fresh        = (age_s >= 0 && age_s <= stale_after);

    // 双重校验: 时间戳新鲜 且 pid 还活着 才算真的在跑。
    // 只看时间戳会漏「进程刚死但快照还没过期」的两三个周期窗口;
    // 只看 pid 会漏「pid 被内核复用给了别的进程」。
    bool pid_alive = false;
    std::string pid_s = extract_json_field(raw, "pid");
    if (!pid_s.empty()) {
        pid_t pid = (pid_t)atol(pid_s.c_str());
        if (pid > 1 && kill(pid, 0) == 0) pid_alive = true;
    }
    bool running = file_running && fresh && pid_alive;

    std::string reason;
    if (!file_running)      reason = "daemon_stopped";
    else if (!fresh)        reason = "snapshot_stale";
    else if (!pid_alive)    reason = "pid_gone";
    else                    reason = "running";

    std::string state   = extract_json_field(raw, "state");
    std::string temp    = extract_json_field(raw, "soc_temp_c");
    std::string enter_c = extract_json_field(raw, "temp_enter_c");
    std::string exit_c  = extract_json_field(raw, "temp_exit_c");
    std::string score   = extract_json_field(raw, "person_score_th");
    std::string led     = extract_json_field(raw, "led");
    std::string mode    = extract_json_field(raw, "mode");
    std::string itv     = extract_json_field(raw, "interval_s");

    std::stringstream ss;
    ss << "[{"
       << "\"running\": " << (running ? "true" : "false") << ", "
       << "\"reason\": \"" << reason << "\", "
       << "\"snapshot_age_s\": " << (age_s < 0 ? -1 : age_s) << ", "
       << "\"pid\": " << (pid_s.empty() ? "0" : pid_s.c_str()) << ", "
       << "\"mode\": \"" << (mode.empty() ? "unknown" : mode.c_str()) << "\", "
       << "\"interval_s\": " << (itv.empty() ? "0" : itv.c_str()) << ", "
       // 这三个是「当前真正生效」的阈值, 必须与日志里历史事件的阈值区分开
       << "\"active_thresholds\": {"
       <<   "\"soc_temp_enter_c\": " << (enter_c.empty() ? "0" : enter_c.c_str()) << ", "
       <<   "\"soc_temp_exit_c\": " << (exit_c.empty() ? "0" : exit_c.c_str()) << ", "
       <<   "\"person_score\": " << (score.empty() ? "0" : score.c_str())
       << "}, "
       << "\"last_state\": \"" << (state.empty() ? "unknown" : state.c_str()) << "\", "
       << "\"soc_temp_c\": " << (temp.empty() || temp == "null" ? "0" : temp.c_str()) << ", "
       << "\"led_target\": \"" << (led.empty() ? "unknown" : led.c_str()) << "\", "
       << "\"updated_at\": \"" << upd << "\", "
       << "\"verdict\": \"";

    if (!running) {
        ss << "巡检守护当前未运行(" << reason << "), 没有任何进程会自动根据温度/人员切换 LED; "
              "LED 停留在守护最后一次写入的状态。设备温度本身仍可正常读取, 但不会自动报警。";
    } else if (state == "THERMAL_ALERT") {
        ss << "巡检守护正在运行, 且当前处于温度告警态: SOC 温度已超过 active_thresholds.soc_temp_enter_c, "
              "LED 应为报警快闪。";
    } else if (state == "PERSON_ALERT") {
        ss << "巡检守护正在运行, 且当前处于人员告警态, LED 应为报警快闪。";
    } else {
        ss << "巡检守护正在运行, 当前无告警, LED 应在心跳呼吸态。";
    }
    ss << "\"}]";
    return ss.str();
}

// -------------------------------------------------------------
// 子系统服务控制（systemctl 包装）
// -------------------------------------------------------------
// 执行命令并抓 stdout。只用于 systemctl 这类受控命令 ——
// 服务名来自下方枚举白名单，不接受模型自由输入，所以不存在注入面。
static std::string run_cmd(const std::string& cmd) {
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// 端口/HTTP 探针：判断"服务真的能用"，而不是"systemd 认为它在跑"。
// 实测（2026-10-31）：NPU 内存不足时推流网关会先报 active，7 秒后才因模型加载失败退出；
// 那一刻 is-active 仍是 active —— Agent 于是对用户说"已打开"，用户连上去什么都没有。
static bool http_ok(const char* port, int tries) {
    for (int i = 0; i < tries; ++i) {
        std::string code = run_cmd(std::string("curl -s -o /dev/null -w '%{http_code}' "
                                               "--max-time 2 http://127.0.0.1:") + port + "/ 2>/dev/null");
        if (code == "200") return true;
        sleep(1);
    }
    return false;
}

static bool port_ok(const char* port, int tries) {
    for (int i = 0; i < tries; ++i) {
        std::string n = run_cmd(std::string("ss -tln 2>/dev/null | grep -c ':") + port + " '");
        if (!n.empty() && n != "0") return true;
        sleep(1);
    }
    return false;
}

struct ServiceEntry { const char* alias; const char* unit; const char* desc; };
static const ServiceEntry kServices[] = {
    {"dashboard", "edgevision-dashboard", "车辆实时检测看板 (HTTP :8090, 摄像头 /dev/video83)"},
    {"gateway",   "edgevision",           "TCP 推流网关 (定长帧 :9000, 摄像头 /dev/video81)"},
    {"patrol",    "edgevision-patrol",    "巡检守护 (快层, 纯 C 循环, 不占 NPU)"},
    {"voice",     "voice-gateway",        "语音交互网关 (本体: ASR + Agent + TTS)"},
};
static const int kServiceCount = (int)(sizeof(kServices) / sizeof(kServices[0]));

// 板子自己的局域网 IP。工具返回里要带上，否则模型会编 "localhost" ——
// 用户在**另一台电脑**上开浏览器，localhost 指向他自己的机器，必然打不开。
static std::string local_ip() {
    static std::string cached;
    if (!cached.empty()) return cached;
    cached = run_cmd("hostname -I 2>/dev/null | awk '{print $1}'");
    if (cached.empty()) cached = "127.0.0.1";
    return cached;
}

// 判断"我是不是语音网关拉起的子进程"（父进程 comm 为 voice_gateway）。
// 这个区分决定了 voice 的 start/stop 语义：
//   在网关内 → 说的是"我自己"，stop 要延迟自杀
//   在控制台里 → 说的是"另一个进程"，且控制台自己占着 NPU，必须先让出
static bool in_voice_gateway() {
    std::ifstream f("/proc/" + std::to_string((long)getppid()) + "/comm");
    std::string comm;
    if (!(f >> comm)) return false;
    return comm.find("voice_gateway") != std::string::npos;
}

std::string tool_control_service(const std::string& name, const std::string& action) {
    const ServiceEntry* svc = nullptr;
    for (int i = 0; i < kServiceCount; ++i)
        if (name == kServices[i].alias) { svc = &kServices[i]; break; }

    if (!svc)
        return "[{\"success\": false, \"error\": \"unknown_service\", "
               "\"note\": \"可用服务: dashboard / gateway / patrol / voice\"}]";
    if (action != "start" && action != "stop" && action != "status")
        return "[{\"success\": false, \"error\": \"invalid_action\", "
               "\"note\": \"只接受 start / stop / status\"}]";

    std::stringstream ss;
    ss << "[{\"service\": \"" << svc->alias << "\", \"unit\": \"" << svc->unit
       << "\", \"action\": \"" << action << "\", \"description\": \"" << svc->desc << "\", ";

    // ---- 自我指涉：语音网关把自己的启停当成"另一个服务"会出三种错 ----
    // ① 在网关内说 stop：等于杀掉正在说话的自己 → 先回应，3 秒后再执行
    // ② 在网关内说 start：它本来就在跑 → 直接说"已经在运行"
    // ③ 在手动控制台里说 start：控制台自己占着 2 GB NPU，再起网关必定 errno 14
    //    → 交接：本进程先退出释放 NPU，8 秒后由 systemd 拉起网关
    //
    // 两种启动方式都要覆盖：
    //   systemd 托管 → systemctl stop
    //   voice_start.sh 手动起 → systemctl 停不掉，必须 pkill
    if (std::string(svc->alias) == "voice") {
        const bool inside = in_voice_gateway();
        const bool alive  = (run_cmd("systemctl is-active voice-gateway 2>&1") == "active") ||
                            !run_cmd("pgrep -x voice_gateway 2>/dev/null").empty();

        if (action == "start" && (inside || alive)) {
            ss << "\"success\": true, \"active\": \"active\", \"already\": true, "
               << "\"note\": \"语音助手已经在运行，不需要再启动。\"}]";
            return ss.str();
        }

        if (action == "start" && !inside) {
            pid_t pid = fork();
            if (pid == 0) {
                setsid();                          // 脱离会话：父进程退出不影响自己
                int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
                sleep(8);                          // 等本进程把 2 GB NPU 映射释放干净
                execl("/bin/systemctl", "systemctl", "start", svc->unit, (char*)nullptr);
                _exit(0);
            }
            g_stop = 1;                            // 主循环退出 → rkllm_destroy 释放 NPU
            ss << "\"success\": true, \"active\": \"handover\", \"handover\": true, "
               << "\"note\": \"控制权交接中：本控制台即将退出以释放 NPU，8 秒后语音助手自动启动"
               << "(约需 70 秒就绪，之后直接喊「小瑞」)。\"}]";
            return ss.str();
        }

        if (action == "stop" && !inside && !alive) {
            ss << "\"success\": true, \"active\": \"inactive\", \"already\": true, "
               << "\"note\": \"语音助手本来就没在运行。\"}]";
            return ss.str();
        }

        // 控制台里 stop：立即停，并且**按 pid 兜底**。
        // 为什么不能只靠 systemctl：voice_start.sh 手动起的实例 systemd 管不到，
        // systemctl stop 返回 0、is-active 也是 inactive，于是报"已停止"——但实际上进程还活着（假成功）。
        // 也不用 pkill -f：run_cmd 的 sh 命令行里含同样的字符串，会误杀自己（第 9 条）。
        if (action == "stop" && !inside && alive) {
            run_cmd("systemctl stop voice-gateway 2>&1");
            for (int i = 0; i < 12; ++i) {
                std::string pids = run_cmd("pgrep -x voice_gateway 2>/dev/null");
                if (pids.empty()) break;
                std::stringstream ps(pids);
                std::string one;
                while (ps >> one) {
                    pid_t p = (pid_t)atoi(one.c_str());
                    if (p > 0) kill(p, SIGTERM);
                }
                sleep(1);
            }
            bool gone = run_cmd("pgrep -x voice_gateway 2>/dev/null").empty();
            ss << "\"success\": " << (gone ? "true" : "false")
               << ", \"active\": \"" << (gone ? "inactive" : "active") << "\", \"note\": \""
               << (gone ? "语音助手已停止。" : "停止失败，进程仍在运行。") << "\"}]";
            return ss.str();
        }

        if (action == "stop" && inside) {
            pid_t pid = fork();
            if (pid == 0) {
                setsid();                          // 脱离会话：父进程死了不影响自己
                int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
                sleep(3);
                execl("/bin/sh", "sh", "-c",
                      "systemctl stop voice-gateway 2>/dev/null; pkill -TERM -f voice_gatewa",
                      (char*)nullptr);
                _exit(0);
            }
            ss << "\"success\": true, \"active\": \"active\", "
               << "\"note\": \"将在 3 秒后关闭语音网关(包含我自己)。这是预期行为，会说完整句话再退出。\"}]";
            return ss.str();
        }
        // 控制台里 stop 且网关在跑：交给下面的通用路径（systemctl stop 是真有效的）
    }

    // 记录操作前的状态：用来区分"真的停了"和"本来就没在跑"。
    // 否则 systemctl stop 一个未运行的服务也返回 0，会被报成"已停止"（假成功）。
    std::string prev = (action == "status")
                     ? "" : run_cmd(std::string("systemctl is-active ") + svc->unit + " 2>&1");

    std::string out = run_cmd("systemctl " + action + " " + svc->unit + " 2>&1");

    // start/stop 是异步的：systemctl 返回不代表服务真的起来了。
    // 等 3 秒再查真实状态，否则会把"启动失败"报成成功（LED trigger 那次的教训）。
    if (action != "status") sleep(3);
    std::string active = run_cmd(std::string("systemctl is-active ") + svc->unit + " 2>&1");

    bool ok;
    if (action == "start")      ok = (active == "active");
    else if (action == "stop")  ok = (active != "active");
    else                        ok = true;

    // start 的判据再进一步：**端口/HTTP 真的通了**才算起来。
    // 只看 is-active 会在"进程活着但模型没加载成功"的窗口里误报成功（见上方探针注释）。
    bool not_ready = false;
    if (action == "start" && ok) {
        const std::string a = svc->alias;
        if      (a == "dashboard") ok = http_ok("8090", 10);
        else if (a == "gateway")   ok = port_ok("9000", 10);
        if (!ok) not_ready = true;
    }

    ss << "\"success\": " << (ok ? "true" : "false") << ", \"active\": \"" << active << "\"";
    if (!out.empty() && !ok)
        ss << ", \"error_detail\": \"" << out.substr(0, 160) << "\"";

    if (not_ready)
        ss << ", \"note\": \"进程起来了但端口没通——多半是 NPU 内存不足导致模型加载失败。"
              "建议先关掉语音助手或看板再试。\"";
    else if (action == "stop" && ok && prev != "active")
        ss << ", \"note\": \"本来就没在运行。\"";
    // 看板占 NPU，必须让模型主动告知用户"之后我会变慢"
    else if (std::string(svc->alias) == "dashboard" && ok)
        ss << ", \"note\": \"" << (action == "start"
                 ? "看板已启动(:8090)。视觉推理会占用 NPU，我之后的响应会变慢，这是正常的资源争用。"
                 : "看板已停止，NPU 已释放，响应速度恢复正常。") << "\"";
    else if (ok && action == "start")
        ss << ", \"note\": \"" << svc->desc << " 已启动。\"";

    // 访问地址必须由工具给出，不能让模型自己编 ——
    // 模型默认会说 localhost，而用户在另一台电脑上访问 localhost 就是他自己的机器。
    if (std::string(svc->alias) == "dashboard") {
        ss << ", \"url\": \"http://" << local_ip() << ":8090/\", "
           << "\"url_note\": \"这是用户在自己电脑浏览器里该打开的地址。"
              "不要说 localhost 或 127.0.0.1，那个指向用户自己的机器。\"";
    } else if (std::string(svc->alias) == "gateway") {
        ss << ", \"url\": \"tcp://" << local_ip() << ":9000\", "
           << "\"url_note\": \"TCP 定长帧协议，不是网页，不能直接用浏览器打开。\"";
    }

    ss << "}]";
    return ss.str();
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
        // DAEMON_START 的阈值/间隔只存在 detail 对象里(该行根本没有 summary 键),
        // 直接置空就丢掉了「这轮守护按什么阈值启动」这条唯一线索 —— 曾因此让 Agent
        // 以为阈值就是默认 75C。这里显式把关键配置重建成一句话。
        if (type == "DAEMON_START" && summary.empty()) {
            std::string te = extract_json_field(line, "temp_enter_c");
            std::string tx = extract_json_field(line, "temp_exit_c");
            std::string iv = extract_json_field(line, "interval_s");
            summary = std::string("巡检守护启动: 温度告警阈值 ") + (te.empty() ? "?" : te) +
                      "C(恢复 " + (tx.empty() ? "?" : tx) + "C), 采样间隔 " +
                      (iv.empty() ? "?" : iv) + "s";
        }
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

        // 通用参数提取（control_service 需要 name + action 两个）
        auto get_param = [&](const char* key, const char* dflt) -> std::string {
            std::string tag = std::string("<parameter=") + key + ">";
            size_t s = call_body.find(tag);
            if (s == std::string::npos) return dflt;
            s += tag.size();
            size_t e = call_body.find("</parameter>", s);
            if (e == std::string::npos) return dflt;
            std::string v = call_body.substr(s, e - s);
            size_t b = v.find_first_not_of(" \t\r\n");
            size_t en = v.find_last_not_of(" \t\r\n");
            return (b == std::string::npos) ? std::string(dflt) : v.substr(b, en - b + 1);
        };
        std::string param_name   = get_param("name", "");
        std::string param_action = get_param("action", "");

        std::cout << "\n========================================================" << std::endl;
        std::cout << "⚙️  [Agent 决策] 触发物理硬件工具: " << tool_name << std::endl;

        std::string obs;
        if (tool_name == "get_system_health") {
            obs = tool_get_system_health();
        } else if (tool_name == "get_led_status") {
            obs = tool_get_led_status();
        } else if (tool_name == "control_led") {
            std::cout << "   -> 设定参数: mode = " << param_mode << std::endl;
            // 语义泛化拦截：本轮在处理"服务"请求时，不许顺手改灯。
            // 实测"打开车辆看板"会连带 control_led(heartbeat) —— 它把"打开"理解成了"开灯"。
            if (g_service_in_turn) {
                obs = "[{\"success\": false, \"error\": \"rejected_by_guard\", "
                      "\"note\": \"本轮正在处理服务启停请求，与指示灯无关，灯控已被拒绝。\"}]";
            } else {
                obs = tool_control_led(param_mode);
            }
        } else if (tool_name == "get_vision_telemetry") {
            obs = tool_get_vision_telemetry();
        } else if (tool_name == "get_patrol_events") {
            std::cout << "   -> 设定参数: tail = " << param_tail << std::endl;
            obs = tool_get_patrol_events(param_tail);
        } else if (tool_name == "get_patrol_status") {
            obs = tool_get_patrol_status();
        } else if (tool_name == "control_service") {
            std::cout << "   -> 设定参数: name = " << param_name
                      << ", action = " << param_action << std::endl;
            // 硬拦越权：一轮里只允许启动一个服务。
            // 实测模型会把"打开看板"扩展成"看板 + 巡检守护"一起启动。
            if (param_action == "start" && g_start_in_turn >= 1) {
                obs = "[{\"success\": false, \"error\": \"rejected_by_guard\", "
                      "\"note\": \"本轮已经启动过一个服务，拒绝连带启动其他服务。\""
                      "\"如果用户确实需要，请单独再下达一次指令。\"}]";
            } else {
                if (param_action == "start") ++g_start_in_turn;
                g_service_in_turn = true;
                obs = tool_control_service(param_name, param_action);
            }
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
// -------------------------------------------------------------
// 快速通道（意图路由）
// -------------------------------------------------------------
// 高频固定问题绕开 LLM 直接回答：约 10 s → 0.05 s。
//
// 设计原则：**只匹配明确无歧义的说法，拿不准的一律交回 LLM**。
// 宁可慢一点，也不能答错 —— 用户听到一个"很快但驴唇不对马嘴"的回答，
// 比多等 10 秒糟得多。
//
// 顺序很重要：
//   服务控制 → LED 控制 → LED 状态 → 温度 → 告警 → 时间
// "打开看板" 必须先被服务分支吃掉，否则 "打开" 会被 LED 分支理解成开灯（实测犯过）。
static bool has_any(const std::string& s, std::initializer_list<const char*> ks) {
    for (const char* k : ks)
        if (s.find(k) != std::string::npos) return true;
    return false;
}

// 同一件事准备几种说法轮换，避免每次回答一字不差（听感差别很大）
static const char* pick_variant(std::initializer_list<const char*> vs, size_t seed) {
    size_t n = vs.size();
    if (n == 0) return "";
    return *(vs.begin() + (seed % n));
}

bool try_fast_path(const std::string& q, std::string& out) {
    const size_t seed = q.size() * 131 + q[0];

    // ---- 0. "日志怎么看" 这类元问题 ----
    // 实测踩过：说"语音助手日志怎么看"，被下面的服务分支当成 status 查询，
    // 回了句"正在运行。"——问的是运维方法，答的是运行状态。
    if (q.find("日志") != std::string::npos) {
        out = "语音链路看 journalctl -u voice-gateway -f，Agent 自己的输出也在里面；"
              "旧的滚动日志在 /tmp/vgw.log。";
        return true;
    }

    // ---- 0.5 寒暄：模型见到人就想找个理由调工具 ----
    // 实测：说"你好"，它调了 get_led_status，然后回答变成"指示灯当前心跳呼吸…"。
    // 寒暄不需要任何数据，直接在代码里答掉，既快又不可能答歪。
    // 长度限制是护栏：避免把"你好，灯什么状态"这种带诉求的句子也吞掉。
    if (q.size() <= 12 &&
        has_any(q, {"你好", "您好", "在吗", "嗨", "哈喽", "hello", "hi"})) {
        out = pick_variant({"我在。", "您好。", "在的。"}, seed);
        return true;
    }

    // ---- 0.6 社交用语：道谢/确认不需要任何硬件 ----
    // 实测：说"谢谢"，它调了 get_system_health + get_led_status + get_patrol_status，
    // 回了一屏设备状态。礼貌用语没有信息需求，代码答掉最稳。
    if (q.size() <= 12 &&
        has_any(q, {"谢谢", "多谢", "感谢", "收到", "明白", "好的", "辛苦了", "嗯", "ok", "OK"})) {
        out = pick_variant({"不客气。", "嗯。", "好。"}, seed);
        return true;
    }

    // ---- 1. 服务控制（必须最先：说"打开看板"不能被当成开灯）----
    // 但要排除"怎么/在哪/为什么"这类元问题——它们问的是方法，不是指令。
    const bool meta = has_any(q, {"怎么", "如何", "为什么", "是什么", "干什么",
                                  "有什么用", "在哪", "哪里"});
    if (!meta &&
        has_any(q, {"看板", "8090", "车辆检测", "推流", "9000", "网关", "巡检守护", "语音助手"})) {
        std::string name;
        if (has_any(q, {"看板", "8090", "车辆检测"}))          name = "dashboard";
        else if (has_any(q, {"推流", "9000", "网关"}))         name = "gateway";
        else if (q.find("巡检守护") != std::string::npos)       name = "patrol";
        else if (q.find("语音助手") != std::string::npos)       name = "voice";

        if (!name.empty()) {
            std::string action;
            if (has_any(q, {"关掉", "关闭", "停掉", "停止", "别开"})) action = "stop";
            else if (has_any(q, {"打开", "启动", "开启", "开始", "开一下", "开开", "开起来"}))
                                                                     action = "start";
            else                                                     action = "status";

            std::string obs;
            if (action == "start" && g_start_in_turn >= 1) {
                obs = "[{\"success\": false, \"active\": \"unknown\", "
                      "\"note\": \"本轮已启动过一个服务\"}]";
            } else {
                if (action == "start") ++g_start_in_turn;
                obs = tool_control_service(name, action);
            }

            std::string active = extract_json_field(obs, "active");
            std::string url    = extract_json_field(obs, "url");
            std::string note   = extract_json_field(obs, "note");
            // 回答必须带对象名：只说"已停止。"，用户不知道停的是哪台服务
            const std::string zh = (name == "dashboard") ? "看板"
                                 : (name == "gateway")   ? "推流网关"
                                 : (name == "patrol")    ? "巡检守护" : "语音助手";

            if (action == "status") {
                out = zh + (active == "active" ? "正在运行。" : "没在运行。");
            } else if (action == "start") {
                // 四种回执：交接 / 本来就在跑 / 正常启动 / 失败
                if (obs.find("\"handover\": true") != std::string::npos)
                    out = "好的，我先把控制台关掉让出 NPU，语音助手会在几秒后自动启动"
                          "（约需 70 秒就绪）。";
                else if (obs.find("\"already\": true") != std::string::npos)
                    out = zh + "已经在运行了。";
                else if (active == "active")
                    out = zh + pick_variant({"已启动。", "开好了。", "已经打开了。"}, seed);
                else
                    out = zh + "启动失败，请稍后再试。";
                if (active == "active" && !url.empty()) out += "访问地址 " + url;
            } else {
                // voice+stop 是「延迟 3 秒自杀」的特殊路径：此刻 active 仍是 active，
                // 但已经安排好了退出。不能按"没停掉"来报失败。
                if (note.find("本来就没在运行") != std::string::npos)
                    out = zh + "本来就没在运行。";
                else if (active != "active")
                    out = zh + pick_variant({"已停止。", "关掉了。"}, seed);
                else if (note.find("秒后") != std::string::npos)
                    out = "好的，3 秒后关闭语音助手。";
                else
                    out = zh + "停止失败，请稍后再试。";
            }
            return true;
        }
    }

    // ---- 2. LED 控制（写在"灯状态"之前，否则"关灯"会被状态分支吃掉）----
    if (q.find("灯") != std::string::npos &&
        has_any(q, {"关掉", "关闭", "打开", "改成", "切换", "设为", "设成",
                    "心跳", "报警", "常亮", "熄灭", "灭"})) {
        std::string mode;
        if (has_any(q, {"关", "灭"}))                       mode = "off";
        else if (has_any(q, {"报警", "急闪"}))               mode = "alarm";
        else if (has_any(q, {"常亮", "打开"}))               mode = "on";
        else if (q.find("心跳") != std::string::npos)        mode = "heartbeat";

        if (!mode.empty()) {
            std::string obs = tool_control_led(mode);
            out = (extract_json_field(obs, "success") == "true")
                ? pick_variant({"已切换。", "好了。", "设好了。"}, seed)
                : "切换失败。";
            return true;
        }
    }

    // ---- 3. LED 状态 ----
    // 直接用工具返回的 description，**不要在这里再维护一份模式表**。
    // 实测踩过：这里漏了 alarm_blink（用户态爆闪），用户听到"指示灯当前是alarm_blink"。
    if (q.find("灯") != std::string::npos &&
        has_any(q, {"状态", "怎么样", "在闪", "亮着", "什么模式", "什么情况"})) {
        std::string st   = tool_get_led_status();
        std::string desc = extract_json_field(st, "description");
        if (desc.empty()) desc = extract_json_field(st, "current_mode");
        out = "指示灯当前是" + desc + "。";
        return true;
    }

    // ---- 4. 温度 / 健康 ----
    if (has_any(q, {"温度", "多少度", "热不热", "多热"})) {
        std::string soc = extract_json_field(tool_get_system_health(), "soc_temp_c");
        if (!soc.empty()) {
            out = pick_variant({"SOC 温度 ", "当前温度 ", "板子温度 "}, seed) + soc + " 度，正常。";
            return true;
        }
    }

    // ---- 5. 告警 / 巡检 ----
    if (has_any(q, {"告警", "报警", "安全吗", "有没有人", "有人吗"})) {
        std::string running = extract_json_field(tool_get_patrol_status(), "running");
        out = (running == "true") ? "巡检守护在运行，当前没有告警。"
                                  : "巡检守护没在运行，不会自动报警。";
        return true;
    }

    // ---- 6. 时间（纯本地，不需要联网）----
    if (has_any(q, {"几点", "什么时间", "现在时间", "几号", "今天几"})) {
        std::string hm = run_cmd("date +'%H点%M分'");
        if (!hm.empty()) {
            out = pick_variant({"现在是 ", "当前时间 ", "这会儿是 "}, seed) + hm;
            return true;
        }
    }

    return false;   // 没命中：交回 LLM
}

void run_agent_turn(LLMHandle handle, RKLLMInferParam& infer, const std::string& user_query) {
    g_start_in_turn = 0;      // 每轮重置"已启动服务数"计数

    // 快速通道：高频固定问题绕开 LLM（实测 10 s → 0.05 s）。
    // 输出格式必须和 LLM 路径一致（用「终局汇报」标记），
    // 否则 voice_gateway 的段落解析抓不到回答，会降级成"我听到你说…"。
    {
        std::string fast;
        if (try_fast_path(user_query, fast)) {
            std::cout << "⚡ [快速通道] 规则命中，跳过 LLM\n" << std::endl;
            std::cout << "🤖 [Edge Agent 推进决策 / 终局汇报]:" << std::endl;
            std::cout << "--------------------------------------------------------" << std::endl;
            std::cout << fast << std::endl;
            return;
        }
    }

    // 按用户意图预判本轮是不是"服务类请求"，而不是等 control_service 被调用后再判断。
    // 原因：实测模型会**先**调 control_led(heartbeat) **再**调 control_service ——
    // 等第二个发生时灯已经被改了。所以必须在回合开始就封住灯控。
    g_service_in_turn =
        user_query.find("看板")     != std::string::npos ||
        user_query.find("网关")     != std::string::npos ||
        user_query.find("推流")     != std::string::npos ||
        user_query.find("巡检守护") != std::string::npos ||
        user_query.find("服务")     != std::string::npos ||
        user_query.find("8090")     != std::string::npos ||
        user_query.find("9000")     != std::string::npos ||
        user_query.find("车辆检测") != std::string::npos;

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

    // 空输出兜底（2026-10-29 实测补充）：
    // 模型返回 0 个 token 时，callback 只补一个换行，屏幕上什么都不出现，
    // 用户无法区分"它没听懂"和"它已经死了"——实测被误判为卡死。
    if (g_step_output.empty()) {
        std::cout << "\n⚠️  [Agent] 模型本轮未输出任何内容"
                     "（通常是输入过短/意图不明确，或终端编码异常）。"
                  << std::endl
                  << "    请换一种更完整的问法重试，例如「查一下开发板当前温度」。"
                  << std::endl;
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
        "- 温度常识: RK3588 的 SOC 正常工作温度为 30~65°C，85°C 以上才需要担心降频/损坏。\n"
        "- 【关键区分·告警阈值 ≠ 物理过热阈值】是否触发 LED 报警闪烁，唯一依据是巡检守护"
        "进程**当前实际生效**的阈值，必须调用 get_patrol_status 读取 active_thresholds.soc_temp_enter_c。"
        "严禁用 75°C 这类默认值、或用\"温度在正常范围内\"反推\"所以不会报警\"——"
        "守护验收时常以 --temp-enter 20 这类低阈值运行，此时 44°C 早已满足报警条件。\n"
        "- 若实际阈值明显偏低(如 20°C)，那是守护用评测阈值跑验收：应把两件事分开如实说明——"
        "\"设备物理温度正常(44°C 不会损坏)\"+\"但按守护当前 20°C 阈值已满足报警条件\"，"
        "绝不能因为\"物理上不过热\"就否定\"按阈值正在报警\"。\n"
        "- 【禁止混淆·常见错误】\"超过告警阈值\" ≠ \"温度超出正常范围\"。"
        "50°C 超过 20°C 阈值，但仍在 30~65°C 正常范围内，属于\"温度正常、阈值偏低\"。"
        "只有实测 >65°C 才可说\"偏高\"，>75°C 才可说\"过热\"。"
        "严禁出现\"50.85°C 也在正常范围之外\"这类自相矛盾的表述，"
        "也不要把\"超出告警阈值\"直接写成\"温度异常\"。\n"
        "- 【历史 ≠ 现状】get_patrol_events 只说明过去发生过什么，get_patrol_status 才说明此刻的"
        "运行态。回答\"现在会不会报警/灯在闪吗\"必须先看 get_patrol_status；"
        "若其 running=false，必须明确指出\"巡检守护当前未运行，不会有自动报警，"
        "LED 停留在守护最后一次写入的状态\"，不能只说\"温度正常所以不报警\"。\n"
        "【工具调用准则】:\n"
        "- 查询指示灯状态请调用 get_led_status，改变指示灯才调用 control_led。\n"
        "- 【LED 读数】heartbeat 与爆闪模式下 brightness 由触发器周期性翻转(心跳的灭相占 75% 以上)，"
        "瞬时读到 0 是正常的采样假象，**绝不等于\"关闭\"**。判断灯态只看 current_mode / description / "
        "brightness_duty_pct，严禁把 brightness 的瞬时值当成开关状态，也不许在同一句里既说"
        "\"亮度0关闭\"又说\"正常呼吸闪烁\"。\n"
        "- 用户问\"警告灯/报警灯/有没有告警/安全吗\"这类问题时，除 get_led_status 外还要调用 "
        "get_patrol_status，两者结合才能说清\"灯现在是什么模式\"和\"为什么是这个模式\"。\n"
        "- 【最高优先级】当用户明确要求把灯调成某个模式（如心跳呼吸/报警/常亮/关闭）时，"
        "这是用户的直接指令，必须立即调用 control_led 并把 mode 设为用户要求的值，"
        "即使查询到当前状态接近或相同也要显式执行（幂等写），绝不能因为\"当前已经是这样\"而跳过；"
        "也不要参考温度、巡检事件等其他信息来更改用户指定的模式。\n"
        "- 查询整体硬件健康和温度请调用 get_system_health。\n"
        "- 查询画面识别目标请调用 get_vision_telemetry。\n"
        "- 查询巡检守护产生的硬件告警事件请调用 get_patrol_events。\n"
        "- 查询巡检守护**当前是否在运行、实际生效阈值、当前告警状态**请调用 get_patrol_status。\n"
        "- 用户要求打开/关闭/查询**车辆检测看板、推流网关、巡检守护**时，调用 control_service(name, action)。"
        "name 与口头的对应关系：看板/车辆检测/8090 → dashboard；推流/网关/9000 → gateway；"
        "巡检守护 → patrol；语音助手/你自己 → voice。\n"
        "- 【失败必须如实说·最高优先级】工具返回 `success:false` 或带 `error` 字段时，"
        "必须明确告诉用户「操作失败」并转述原因，**绝对不能说成已经完成**。"
        "实测出现过把 `rejected_by_guard` 说成「LED 已关闭」的情况——工具明明拒绝了，"
        "却汇报成功，这比答得慢严重得多。看不清楚结果就说不确定，不要编。\n"
        "- 【严禁混淆·实测踩过】\"关闭车辆检测\"\"关掉看板\"\"停掉 8090\" 都指 dashboard 服务，"
        "必须用 control_service(name=dashboard, action=stop)。"
        "**绝不要用 control_led** —— control_led 只是改指示灯亮灭，跟看板完全无关；"
        "调错了不但没关看板，还顺手把灯关了。\n"
        "- 【地址必须照抄工具的 url 字段】用户问\"怎么访问/地址是什么\"时，"
        "直接用 control_service 返回的 `url`。**禁止说 localhost 或 127.0.0.1** —— "
        "用户是在自己电脑上开浏览器，localhost 指向他自己的机器，必然打不开。\n"
        "- 【只调必需的工具】用户只说\"打开/关闭某个服务\"时，只调 control_service 这一个，"
        "**不要顺带调 get_system_health / get_led_status** —— 那会让用户多等十几秒，"
        "而且答案里塞满他没问的温度和灯状态。\n"
        "- 【只动用户点名的那个服务·最高优先级】用户说\"打开看板\"就**只启动 dashboard 一个**，"
        "绝对不要连带启动巡检守护、推流网关或其他任何服务。"
        "每个服务都需要用户单独授权——自作主张启动用户没提的服务是越权，不可接受。"
        "一条消息里最多只能有**一个** control_service(start) 调用。\n"
        "- 【必须主动说明】启动 dashboard 或 gateway 会占用 NPU，执行成功后你**必须告诉用户"
        "\"我的响应会变慢\"**，不要只说\"已启动\"就完事。\n"
        "- 【为什么变慢·被追问时用真数字】NPU 三核是所有进程共享的：看板跑 YOLOv8 时"
        "每帧约 16 毫秒、30 帧/秒常驻占用；我每生成一个 token 都要过一遍 NPU，算力被分走就变慢。"
        "实测同一个问题：看板停止时约 10 tok/s，看板运行时约 3 tok/s。"
        "被问\"为什么占用 NPU 你就变慢\"时用这两个数字解释，**不要只复述\"占用 NPU 所以变慢\"**。\n"
        "- 【自我关闭是正常行为】用户说\"关掉语音助手/关掉你自己/退出\"时，"
        "调用 control_service(name=voice, action=stop)。它会在 3 秒后真正执行，"
        "目的是让你把这句话播报完。执行后你会被终止，**这是预期行为**，"
        "不要声称\"操作失败\"或\"发生异常\"。\n"
        "- 不要反复调 status 求安心，一次就够；启动失败时要如实转述 error_detail。\n"
        "- 【按需调用·严禁过度感知】只调用能直接回答本问题的那一个工具。"
        "用户打招呼、闲聊、问你是谁、道谢、告别时，**直接用一句话回应，不要调用任何工具**。"
        "只有问题确实涉及某项数据时才调对应工具（问温度→get_system_health，问灯→get_led_status，"
        "问画面→get_vision_telemetry，问告警/守护状态→get_patrol_status）。"
        "每次调工具前先自问：这条数据对回答本问题是必需的吗？"
        "严禁做\"全面体检\"式的批量调用——那会让用户干等一分多钟。\n"
        "- 【重要】研判类任务（如询问设备是否发生过异常、综合巡检）应优先读取巡检事件"
        "进行研判并如实汇报；但当用户明确下达指示灯控制指令（如恢复心跳呼吸灯、调成报警、"
        "关闭指示灯）时，这是用户的直接指令，必须立即调用 control_led 按用户要求的模式执行，"
        "不要被巡检事件日志内容干扰或改判模式；告警时的灯自动控制由巡检守护进程负责。\n"
        "- 【最高优先级】条件指令的条件必须实测验证：条件里提到什么数据源（画面/温度/事件等），"
        "就必须调用对应的工具拿到实时结果来判断，绝不能用巡检历史事件或推测替代实测"
        "（如\"画面里有人就报警\"必须先调 get_vision_telemetry 查看当前画面，"
        "巡检事件里没有人员记录不代表画面里现在没有人）；巡检事件只用于研判过去，"
        "不能代替当前实时感知，也不能以\"守护进程已处理\"为由跳过用户要求且条件未触发的动作。\n"
        "- 严禁捏造虚假硬件数据。\n"
        "- 【语音场景·必须极简】你的回答会被 TTS 朗读给用户听，用户是\"听\"不是\"读\"。"
        "默认限制在 **2 句话、40 字以内**，只给结论和最关键的一个数字；"
        "不要列字段清单、不要分点长篇、不要重复工具名。"
        "只有用户明确要求\"详细报告/完整汇报\"时才可展开。\n"
        "- 用户问\"日志在哪看/怎么排查\"时，直接给真实存在的命令：语音链路 "
        "`systemctl status voice-gateway`、`journalctl -u voice-gateway -f`（旧日志 /tmp/vgw.log）；"
        "**不要编造不存在的日志路径或文件名**。\n"
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
          "description": "查询板载物理 LED 指示灯当前的工作模式(心跳heartbeat、急闪alarm、常亮on、关闭off)。周期模式(心跳/爆闪)返回 brightness_duty_pct = 亮的采样点占比(心跳约20~25%、爆闪约50%), 静态模式(常亮/关闭)返回 brightness。注意: 心跳的瞬时亮度多数时间为 0, 不代表关闭",
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
      },
      {
        "type": "function",
        "function": {
          "name": "get_patrol_status",
          "description": "查询巡检守护进程的当前运行态: 是否在运行、当前实际生效的温度告警阈值/人员阈值、当前状态(NORMAL/PERSON_ALERT/THERMAL_ALERT)以及守护最后一次写入的LED目标态。判断「现在会不会报警」必须调用它, 不能用历史事件或默认阈值推断",
          "parameters": {
            "type": "object",
            "properties": {}
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "control_service",
          "description": "启停或查询板载子系统服务。dashboard=车辆实时检测看板(HTTP :8090, 车辆实时检测); gateway=TCP推流网关(:9000); patrol=巡检守护(快层); voice=语音交互网关(就是我自己)。注意: 启动 dashboard 或 gateway 会占用 NPU, 导致我之后的响应明显变慢",
          "parameters": {
            "type": "object",
            "properties": {
              "name": {
                "type": "string",
                "enum": ["dashboard", "gateway", "patrol", "voice"],
                "description": "服务名: dashboard(看板:8090) / gateway(推流:9000) / patrol(巡检守护) / voice(语音网关, 即我自己)"
              },
              "action": {
                "type": "string",
                "enum": ["start", "stop", "status"],
                "description": "动作: start 启动 / stop 停止 / status 查询当前状态"
              }
            },
            "required": ["name", "action"]
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
        if (sanitize_utf8(query)) {
            std::cerr << "⚠️  命令行参数含非 UTF-8 字节，已替换为 '?'（shell 编码与板端不一致）"
                      << std::endl;
        }
        try {
            run_agent_turn(handle, infer, query);
        } catch (const std::exception& e) {
            std::cerr << "❌ [Agent] 本轮异常: " << e.what() << std::endl;
        }
    } else {
        // 2. 无参数时，进入常驻交互式 REPL 控制台！
        //
        // 预热：首轮 prefill 要把 2000+ 字的 system prompt 全部编码进 KV cache，
        // 实测耗时约 28 秒（纯 CPU：图优化 + 权重预处理 + jieba/lexicon/FST 解析，
        // 与文本长度无关）。这一步躲不掉，只能决定"什么时候付"——
        // 放在启动阶段付掉，用户第一次提问就只需 prefill 问题本身（几十 token），
        // 几秒就能出结果；否则就是干等 28 秒，体验上完全不可接受。
        {
            std::cout << "\n🔥 预热中（首轮 system prompt 编码约 30s，之后每轮 10s 内）..."
                      << std::flush;
            time_t t_warm = time(nullptr);
            g_quiet = true;
            {
                RKLLMInput wi{};
                wi.role = "user";
                wi.input_type = RKLLM_INPUT_PROMPT;
                wi.prompt_input = "hi";
                rkllm_run(handle, &wi, &infer, nullptr);
            }
            g_quiet = false;
            g_step_output.clear();
            // keep_system_prompt=1 —— 只丢掉预热这轮说的话，system prompt 的 KV 必须留下
            rkllm_clear_kv_cache(handle, 1, nullptr, nullptr);
            std::cout << " 完成，耗时 " << (long)(time(nullptr) - t_warm)
                      << "s（上下文已重置，可直接开始对话）" << std::endl;
        }

        std::cout << "\n========================================================" << std::endl;
        std::cout << "💬 已就绪！进入纯端侧 Agent 常驻交互控制台 (秒级响应)" << std::endl;
        std::cout << "   - 输入自然语言直接与智能体对话 (如: '指示灯是什么状态', '开发板运行怎么样')" << std::endl;
        std::cout << "   - 特殊指令: '/clear' 清空上下文, 'exit' 或 'quit' 退出" << std::endl;
        std::cout << "========================================================" << std::endl;

        // 装信号处理：必须在进入交互循环之前。
        // 刻意不加 SA_RESTART —— 要让阻塞中的 getline 被信号打断返回，
        // 否则 Ctrl+C / systemctl stop 之后还得等用户敲一下回车才能退出。
        {
            struct sigaction sa{};
            sa.sa_handler = on_term_signal;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sigaction(SIGTERM, &sa, nullptr);
            sigaction(SIGINT,  &sa, nullptr);
        }

        std::string line;
        while (!g_stop) {
            std::cout << "\nEdgeAgent> " << std::flush;
            if (!std::getline(std::cin, line)) break;   // EOF 或被信号打断
            if (g_stop) break;
            
            // 去除空白
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;

            // 外部输入不可信：先校验 UTF-8 再送进 tokenizer。
            // 非法字节会让 rkllm 抛异常；这里替换成 '?' 并明确告知用户去改终端编码。
            if (sanitize_utf8(line)) {
                std::cout << "⚠️  输入含非 UTF-8 字节（终端编码可能不是 UTF-8），已替换为 '?'。\n"
                             "    Windows 下请先执行 `chcp 65001`，或在 SSH 客户端里把字符集设为 UTF-8。"
                          << std::endl;
                // 内容已经损坏，不能拿去问模型 ——
                // 否则模型会对着 "?" 之类猜一个话题、白调一次工具、再生成一段无关回答（实测发生过）
                std::cout << "    (本轮已丢弃，请重新输入)" << std::endl;
                continue;
            }

            // 单字节输入（误按键、输入法残留、粘贴了一半）直接忽略。
            // 汉字最短 3 字节，所以 <2 只会挡住 ASCII 噪声，不会误伤"灯""关"这类单字指令。
            if (line.size() < 2) {
                std::cout << "    (输入过短，已忽略)" << std::endl;
                continue;
            }

            if (line == "exit" || line == "quit" || line == "/quit" || line == "q") {
                std::cout << "正在退出 Agent 控制台..." << std::endl;
                break;
            }
            if (line == "/clear" || line == "/reset") {
                rkllm_clear_kv_cache(handle, 1, nullptr, nullptr);
                std::cout << "🧹 上下文缓存已清空，系统 Prompt 已保留。" << std::endl;
                continue;
            }

            // 异常边界：单轮失败不该带走整个常驻控制台。
            // 此前没有这层保护，rkllm 内部抛 std::invalid_argument 会直接 terminate。
            try {
                run_agent_turn(handle, infer, line);
            } catch (const std::exception& e) {
                std::cout << "\n❌ [Agent] 本轮异常已捕获，控制台继续运行: " << e.what() << std::endl;
            } catch (...) {
                std::cout << "\n❌ [Agent] 本轮未知异常已捕获，控制台继续运行。" << std::endl;
            }
        }
    }

    rkllm_destroy(handle);
    std::cout << "\n[Agent] 会话结束，NPU 上下文已优雅释放。" << std::endl;
    return 0;
}
