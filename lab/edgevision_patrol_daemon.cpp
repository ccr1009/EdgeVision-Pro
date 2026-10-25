// ============================================================================
//  edgevision_patrol_daemon.cpp — EdgeVision 巡检守护「快层」(规则反射)
//  项目: edgevision-rk3588   板: EVM3588-B (RK3588, Debian 11)
//
//  职责: 每 interval 秒采样 ①温区0 温度 ②NPU 三核负载 ③9000 端口视觉帧,
//        用确定性状态机(NORMAL / PERSON_ALERT / THERMAL_ALERT) + 迟滞控制
//        板载 LED;状态翻转写黑匣子 /root/patrol/patrol_events.log,并同步
//        追加 /root/patrol/llm_pending.jsonl 供未来「慢层 LLM」消费。
//
//  铁律: 快层绝不加载 RKLLM / 任何模型,不链接 rknn_api —— 避免与 9000
//        推流网关(edgevision.service, 7x24, 禁止 stop)抢 NPU。
//
//  v2 变更(task-5):
//    ① alarm 改用户态爆闪: 板上内核 5.10.110 无 CONFIG_LEDS_TRIGGER_TIMER,
//       trigger 列表没有 timer, 写 "timer" 返回 EINVAL(旧版是假成功)。
//       现在 fork+exec /bin/sh 循环翻转 brightness(100ms 亮/100ms 灭),
//       exec 前 0/1/2 必须重定向 /dev/null(否则永不退出的循环持有父进程
//       管道写端, 调用方等 EOF 卡死 —— TROUBLESHOOTING 第 22 条同坑);
//       exec 同时重建地址空间, 不继承本进程任何映射。
//    ② 阈值可配置: --temp-enter / --temp-exit / --person-score,
//       不重编译即可验收(如 --temp-enter 20 --temp-exit 18 触发 THERMAL_ALERT)。
//    ③ flock 单实例锁: 第二个实例退出码 3。
//
//  依赖: 仅 C++17 std + POSIX。逻辑上单线程即可(周期任务串行),未创建
//        线程;仍链接 -lpthread 以满足统一编译参数。
//
//  用法:
//    ./edgevision_patrol_daemon [选项]
//      --interval N        采样周期秒数, 默认 5 (1~3600)
//      --temp-enter C      温度告警阈值 °C, 默认 75.0 (1~150)
//      --temp-exit C       温度恢复阈值 °C, 默认 70.0 (必须 < temp-enter)
//      --person-score X    person 置信度门槛, 默认 0.5 (0~1)
//      --dry-run           一切照常, 但不写任何 LED 节点(动作字段记 would_*)
//      --no-led            完全不碰 LED(连读都不读)
//    退出码: 0 正常; 2 参数非法; 3 已有实例在运行(flock 失败)。
//    退出: SIGINT/SIGTERM/SIGHUP 优雅退出; 默认模式退出前杀爆闪进程并恢复
//          LED heartbeat。
// ============================================================================

// 注: 不再 #define _GNU_SOURCE —— 板上 g++ 对 C++ 源默认已预定义, 重复定义会有 warning
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <string>
#include <sstream>
#include <fstream>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/file.h>   // flock 单实例锁
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

// ---------------- 路径 -------------------------------------------------------
static const char* LED_TRIGGER   = "/sys/class/leds/work/trigger";
static const char* LED_BRIGHTNESS = "/sys/class/leds/work/brightness";
static const char* PATROL_DIR    = "/root/patrol";
static const char* EVENT_LOG     = "/root/patrol/patrol_events.log";
static const char* LLM_LOG       = "/root/patrol/llm_pending.jsonl";
static const char* PATROL_LOCK_FILE = "/tmp/edgevision_patrol.lock";

// ---------------- 迟滞参数表(详见 docs/PATROL_DAEMON_DESIGN.md) --------------
static const long MAX_LOG_BYTES    = 5LL * 1024 * 1024; // 黑匣子 >5MB 轮转, 保留一代 .1
static const int    TEMP_EXIT_N     = 3;                 // 连续 N 周期低于恢复阈值才恢复
static const int    PERSON_ENTER_N  = 2;                 // 连续 N 周期检出 -> PERSON_ALERT
static const int    PERSON_EXIT_N   = 5;                 // 连续 N 周期无 person -> 恢复
static const int    VISION_TIMEOUT_MS = 300;              // 视觉单次探测超时(连接/收帧各 300ms)

// 可配置阈值(默认值, 可被命令行覆盖)
static double g_cfg_temp_enter    = 75.0;  // zone0 高于此 -> THERMAL_ALERT
static double g_cfg_temp_exit     = 70.0;  // 恢复阈值(与进入阈值构成迟滞带)
static float  g_cfg_person_score  = 0.5f;  // person(class 0) 置信度门槛

static volatile sig_atomic_t g_stop = 0;
static void on_exit_signal(int) { g_stop = 1; }

// ---------------- 小工具 ------------------------------------------------------
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

static std::string now_ts() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static std::string f2(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%.2f", v);
    return b;
}

static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += ch;
        }
    }
    return o;
}

// ---------------- sysfs / LED ------------------------------------------------
static bool write_sysfs_str(const char* path, const std::string& val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;
    ssize_t n = write(fd, val.c_str(), val.size());
    close(fd);
    return n == (ssize_t)val.size();
}

// ---------------- LED: heartbeat 触发器 + 用户态爆闪 alarm --------------------
// 硬件事实(实测): 本板内核 5.10.110 出厂未编 CONFIG_LEDS_TRIGGER_TIMER,
// /sys/class/leds/work/trigger 列表里只有 [heartbeat] 等 21 项、没有 timer,
// 写 "timer" 返回 EINVAL —— 旧版写 trigger=timer 是假成功, 灯从没闪过。
// 因此 alarm 改为用户态爆闪: fork 一个 setsid 子进程, 以 100ms 周期翻转
// brightness; 切回 heartbeat / 退出前先杀该子进程。
static const char* LED_ALARM_PID_FILE = "/tmp/edgevision_led_alarm.pid"; // 与 agent 侧共用, 天然互斥
static const char* LED_ALARM_SH_CMD =
    "while true; do"
    " echo 255 > /sys/class/leds/work/brightness; sleep 0.1;"
    " echo 0 > /sys/class/leds/work/brightness; sleep 0.1;"
    " done";

// 停掉 alarm 爆闪子进程(若在跑)。无论 pid 文件状态如何, 都把 LED 拉回已知态
// (trigger=none + 灭), 防止爆闪残留。返回 true = 两个 sysfs 兜底写都成功。
static bool led_alarm_stop() {
    std::ifstream pf(LED_ALARM_PID_FILE);
    if (pf.is_open()) {
        pid_t pid = 0;
        pf >> pid;
        pf.close();
        if (pid > 1 && kill(pid, SIGTERM) == 0) {
            for (int i = 0; i < 20; ++i) { // 最多等 200ms 优雅退出
                if (kill(pid, 0) != 0) break;
                usleep(10000);
            }
            if (kill(pid, 0) == 0) kill(pid, SIGKILL); // 兜底强杀
        }
    }
    unlink(LED_ALARM_PID_FILE);
    bool ok_trigger    = write_sysfs_str(LED_TRIGGER, "none");
    bool ok_brightness = write_sysfs_str(LED_BRIGHTNESS, "0");
    return ok_trigger && ok_brightness;
}

// 启动用户态爆闪。返回 true = 子进程成功起来并确认存活。
static bool led_alarm_start() {
    led_alarm_stop(); // 幂等清场: 杀掉可能残留的爆闪(stop 会把 trigger 拉到 none)
    // 前置硬检查: trigger 必须是 none(heartbeat 等 trigger 会覆盖 brightness 写入)
    if (!write_sysfs_str(LED_TRIGGER, "none")) return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // 子进程: 脱离会话 + exec。exec 重建地址空间, 不携带本进程任何映射。
        setsid();
        // 【必须】0/1/2 全部重定向 /dev/null: 永不退出的 while 循环若继承
        // 父进程 stdout/stderr 管道写端, 调用方(timeout/ssh/管道)永远等不到
        // EOF, 表现为命令卡死到超时(TROUBLESHOOTING 第 22 条同坑)。
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execl("/bin/sh", "sh", "-c", LED_ALARM_SH_CMD, (char*)nullptr);
        _exit(1); // 只有 exec 失败才会到这里
    }
    // 父进程: 记 pid(与 agent 侧共用同一 pid 文件, 谁都能停掉对方的爆闪)
    std::ofstream pf(LED_ALARM_PID_FILE);
    if (!pf.is_open()) { kill(pid, SIGKILL); return false; }
    pf << pid;
    pf.close();
    usleep(50000); // 50ms 后确认子进程没有立即暴毙
    if (kill(pid, 0) != 0) {
        unlink(LED_ALARM_PID_FILE);
        return false;
    }
    return true;
}

static bool led_set_alarm() {
    return led_alarm_start();
}

// 恢复 heartbeat: 先杀爆闪(若在跑), 再写 trigger=heartbeat。
static bool led_set_heartbeat() {
    led_alarm_stop();
    return write_sysfs_str(LED_TRIGGER, "heartbeat");
}

// 读当前 LED 模式(trigger 里方括号括住的是生效模式), 仅默认模式使用
static std::string led_read_mode() {
    std::string trig = read_file_string(LED_TRIGGER);
    size_t l = trig.find('[');
    size_t r = trig.find(']');
    if (l != std::string::npos && r != std::string::npos && r > l)
        return trig.substr(l + 1, r - l - 1);
    return "unknown";
}

// ---------------- 传感器采样 --------------------------------------------------
static bool read_zone0_temp(double& out_c) {
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    if (!f.is_open()) return false;
    long mv = -1;
    f >> mv;
    if (f.fail() || mv < -40000 || mv > 150000) return false; // 毫摄氏度, 离谱值当读失败
    out_c = mv / 1000.0;
    return true;
}

static bool read_npu_load(int& c0, int& c1, int& c2) {
    std::string raw = read_file_string("/sys/kernel/debug/rknpu/load");
    if (raw.empty()) return false;
    // 实测格式: "NPU load:  Core0: 12%, Core1: 0%, Core2: 8%," (Core0 前空格数不定,
    // scanf 格式串里的单个空格可匹配任意长度空白)。解析失败按不可用处理, 不硬凑。
    if (sscanf(raw.c_str(), "NPU load: Core0: %d%%, Core1: %d%%, Core2: %d%%", &c0, &c1, &c2) == 3)
        return true;
    c0 = c1 = c2 = -1;
    return false;
}

// ---------------- 视觉采样: 连 127.0.0.1:9000 收一帧 248B 定长帧 --------------
#define PATROL_MAX_BBOXES 10
#pragma pack(push, 1)
struct PatrolBBox  { int32_t class_id; float score, x, y, w, h; };          // 24B
struct PatrolHeader{ uint32_t frame_id; uint32_t bbox_count; };             // 8B
struct PatrolFrame { PatrolHeader header; PatrolBBox boxes[PATROL_MAX_BBOXES]; }; // 248B
#pragma pack(pop)

struct VisionSample {
    bool   ok = false;
    std::string reason = "-";     // online / gateway_refused / connect_timeout / recv_short ...
    uint32_t frame_id   = 0;
    uint32_t bbox_count = 0;
    bool   person_seen       = false;  // 存在 class=0 且 score>=0.5
    float  best_person_score = -1.0f;  // 帧内 person 最高分(没有 person 框则 -1)
};

static VisionSample probe_vision_once() {
    VisionSample s;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { s.reason = "socket_error"; return s; }

    // 连接阶段: 非阻塞 connect + poll(300ms), 被硬超时框住, 不吃内核默认超时
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(9000);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int cr = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (cr < 0 && errno == EINPROGRESS) {
        struct pollfd p;
        p.fd = fd; p.events = POLLOUT; p.revents = 0;
        int pr = poll(&p, 1, VISION_TIMEOUT_MS);
        if (pr == 0) { close(fd); s.reason = "connect_timeout"; return s; }
        if (pr  < 0) { close(fd); s.reason = "poll_error";      return s; }
        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) soerr = errno;
        if (soerr != 0) {
            close(fd);
            s.reason = (soerr == ECONNREFUSED) ? "gateway_refused" : "connect_fail";
            return s;
        }
    } else if (cr < 0) {
        int e = errno;
        close(fd);
        s.reason = (e == ECONNREFUSED) ? "gateway_refused" : "connect_fail";
        return s;
    }

    // 收帧阶段: 恢复阻塞 + 300ms 收包超时, MSG_WAITALL 等满 248B
    if (fl >= 0) fcntl(fd, F_SETFL, fl);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = VISION_TIMEOUT_MS * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    PatrolFrame frame;
    memset(&frame, 0, sizeof(frame));
    ssize_t n = recv(fd, &frame, sizeof(frame), MSG_WAITALL);
    close(fd);

    if (n != (ssize_t)sizeof(frame)) { s.reason = "recv_short"; return s; }

    s.ok = true;
    s.reason = "online";
    s.frame_id   = frame.header.frame_id;
    uint32_t cnt = frame.header.bbox_count;
    if (cnt > PATROL_MAX_BBOXES) cnt = PATROL_MAX_BBOXES; // 防畸形头越界
    s.bbox_count = cnt;
    for (uint32_t i = 0; i < cnt; ++i) {
        const PatrolBBox& b = frame.boxes[i];
        if (b.class_id != 0) continue;              // COCO 0 = person
        if (!std::isfinite(b.score)) continue;
        if (b.score > s.best_person_score) s.best_person_score = b.score;
        if (b.score >= g_cfg_person_score) s.person_seen = true;
    }
    return s;
}

// ---------------- 黑匣子日志(带 >5MB 轮转, 写失败绝不拖死主循环) --------------
static void append_log_line(const std::string& path, const std::string& line) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > MAX_LOG_BYTES) {
        std::string old = path + ".1";
        rename(path.c_str(), old.c_str()); // rename 原子覆盖旧 .1, 保留一代
    }
    FILE* f = fopen(path.c_str(), "a");
    if (!f) {
        fprintf(stderr, "[%s] WARN: append %s failed: %s\n",
                now_ts().c_str(), path.c_str(), strerror(errno));
        return;
    }
    fprintf(f, "%s\n", line.c_str());
    if (fclose(f) != 0)
        fprintf(stderr, "[%s] WARN: close %s failed (磁盘满?)\n", now_ts().c_str(), path.c_str());
}

// ---------------- 周期采样数据 -------------------------------------------------
struct CycleData {
    bool   temp_ok = false;
    double temp_c  = 0.0;
    bool   npu_ok  = false;
    int    c0 = -1, c1 = -1, c2 = -1;
    VisionSample v;
    std::string action  = "none";          // set_alarm / would_set_alarm / kept_alarm ...
    std::string led_res = "not_attempted"; // ok / write_failed / dry_run_no_write ...
};

// 事件 detail 的公共字段(温度/person score/frame_id/NPU)
static std::string detail_common(const CycleData& cd) {
    std::stringstream ss;
    ss << "\"vision\":\"" << (cd.v.ok ? "online" : "offline") << "\""
       << ",\"vision_reason\":\"" << cd.v.reason << "\"";
    if (cd.temp_ok) ss << ",\"temp_c\":" << f2(cd.temp_c);
    else            ss << ",\"temp_c\":null";
    if (cd.v.ok) {
        ss << ",\"frame_id\":" << cd.v.frame_id
           << ",\"bbox_count\":" << cd.v.bbox_count
           << ",\"person_seen\":" << (cd.v.person_seen ? "true" : "false");
        if (cd.v.best_person_score >= 0.0f)
            ss << ",\"best_person_score\":" << f2(cd.v.best_person_score);
    }
    if (cd.npu_ok)
        ss << ",\"npu_core_load\":[" << cd.c0 << "," << cd.c1 << "," << cd.c2 << "]";
    return ss.str();
}

// 状态翻转写两份: patrol_events.log(黑匣子) + llm_pending.jsonl(慢层契约, 仅 3 类)
static void write_flip_event(const std::string& ts,
                             const std::string& event,
                             const std::string& summary,
                             const std::string& detail_inner) {
    append_log_line(EVENT_LOG,
        std::string("{\"ts\":\"") + ts + "\",\"event\":\"" + event +
        "\",\"summary\":\"" + json_escape(summary) +
        "\",\"detail\":{" + detail_inner + "}}");

    if (event == "PERSON_ALERT" || event == "THERMAL_ALERT" || event == "RECOVER") {
        append_log_line(LLM_LOG,
            std::string("{\"ts\":\"") + ts + "\",\"type\":\"" + event +
            "\",\"summary\":\"" + json_escape(summary) +
            "\",\"raw\":{" + detail_inner + "}}");
    }
}

static void write_start_event(const char* mode_name, int interval_s, const std::string& led_mode) {
    std::string inner =
        std::string("\"mode\":\"") + mode_name + "\""
        + ",\"interval_s\":" + std::to_string(interval_s)
        + ",\"pid\":" + std::to_string((long)getpid())
        + ",\"temp_enter_c\":" + f2(g_cfg_temp_enter)
        + ",\"temp_exit_c\":" + f2(g_cfg_temp_exit)
        + ",\"person_score_th\":" + f2(g_cfg_person_score)
        + ",\"led_trigger_at_start\":\"" + json_escape(led_mode) + "\""
        + ",\"action\":\"none\",\"led_result\":\"not_attempted\"";
    append_log_line(EVENT_LOG,
        std::string("{\"ts\":\"") + now_ts() + "\",\"event\":\"DAEMON_START\",\"detail\":{" + inner + "}}");
}

// ---------------- 状态机标签 ---------------------------------------------------
enum PatrolState { ST_NORMAL = 0, ST_PERSON = 1, ST_THERMAL = 2 };
static const char* state_name(int s) {
    switch (s) {
        case ST_PERSON:  return "PERSON_ALERT";
        case ST_THERMAL: return "THERMAL_ALERT";
        default:         return "NORMAL";
    }
}

static void print_usage(const char* prog) {
    printf("用法: %s [选项]\n"
           "  --interval N        采样周期秒数(默认 5, 范围 1~3600)\n"
           "  --temp-enter C      温度告警阈值°C(默认 75.0, 范围 1~150)\n"
           "  --temp-exit C       温度恢复阈值°C(默认 70.0, 必须小于 temp-enter)\n"
           "  --person-score X    person 置信度门槛(默认 0.5, 范围 0~1)\n"
           "  --dry-run           不写任何 LED 节点, 动作字段记 would_*\n"
           "  --no-led            完全不碰 LED\n"
           "  -h, --help          显示帮助\n"
           "退出码: 0 正常; 2 参数非法; 3 已有实例在运行(单实例锁)\n", prog);
}

// 严格解析 double 参数: 整串必须是合法数字(拒绝 "20abc" 之类尾部垃圾)
static bool parse_double_arg(const char* s, double& out) {
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s || end == nullptr || *end != '\0') return false;
    out = v;
    return true;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0); // 重定向到文件也按行刷, 便于观察

    enum { MODE_LED = 0, MODE_DRYRUN = 1, MODE_NOLED = 2 };
    int mode = MODE_LED;
    int interval_s = 5;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--dry-run") mode = MODE_DRYRUN;
        else if (a == "--no-led")  mode = MODE_NOLED;
        else if (a == "--interval" && i + 1 < argc) {
            interval_s = atoi(argv[++i]);
            if (interval_s <= 0) { fprintf(stderr, "[patrol] --interval 非法, 用默认 5s\n"); interval_s = 5; }
        }
        else if (a == "--temp-enter" && i + 1 < argc) {
            if (!parse_double_arg(argv[++i], g_cfg_temp_enter)) {
                fprintf(stderr, "[patrol] --temp-enter 不是合法数字: %s\n", argv[i]); return 2;
            }
        }
        else if (a == "--temp-exit" && i + 1 < argc) {
            if (!parse_double_arg(argv[++i], g_cfg_temp_exit)) {
                fprintf(stderr, "[patrol] --temp-exit 不是合法数字: %s\n", argv[i]); return 2;
            }
        }
        else if (a == "--person-score" && i + 1 < argc) {
            double v = 0;
            if (!parse_double_arg(argv[++i], v)) {
                fprintf(stderr, "[patrol] --person-score 不是合法数字: %s\n", argv[i]); return 2;
            }
            g_cfg_person_score = (float)v;
        }
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
        else { fprintf(stderr, "[patrol] 未知参数: %s\n", a.c_str()); print_usage(argv[0]); return 2; }
    }

    // ---- 参数校验(非法返回 2) ----
    bool args_bad = false;
    if (g_cfg_temp_enter < 1.0 || g_cfg_temp_enter > 150.0) {
        fprintf(stderr, "[patrol] --temp-enter %.2f 超出合理范围(1~150)\n", g_cfg_temp_enter);
        args_bad = true;
    }
    if (g_cfg_temp_exit >= g_cfg_temp_enter) {
        fprintf(stderr, "[patrol] --temp-exit %.2f 必须严格小于 --temp-enter %.2f (迟滞带要求)\n",
                g_cfg_temp_exit, g_cfg_temp_enter);
        args_bad = true;
    }
    if (g_cfg_person_score < 0.0f || g_cfg_person_score > 1.0f) {
        fprintf(stderr, "[patrol] --person-score %.2f 超出范围(0~1)\n", g_cfg_person_score);
        args_bad = true;
    }
    if (args_bad) return 2;

    if (interval_s > 3600) interval_s = 3600;
    const char* mode_name = (mode == MODE_DRYRUN) ? "dry-run" : (mode == MODE_NOLED) ? "no-led" : "led";

    // ---- 单实例锁: 防止两个守护同时控灯/写同一份黑匣子(失败返回 3) ----
    int lock_fd = open(PATROL_LOCK_FILE, O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) {
        fprintf(stderr, "[%s] ERROR: 打开单实例锁 %s 失败: %s\n",
                now_ts().c_str(), PATROL_LOCK_FILE, strerror(errno));
        return 3;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "[%s] 另一个巡检守护已在运行(%s 被占用), 本实例退出(码 3)\n",
                now_ts().c_str(), PATROL_LOCK_FILE);
        close(lock_fd);
        return 3;
    }
    // 锁随进程存活: lock_fd 故意不 close, 进程退出(含被 kill -9)时内核自动释放

    // 目录自建(mkdir -p), 失败只警告 —— 日志写不进去不能死主循环
    if (mkdir(PATROL_DIR, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "[%s] WARN: mkdir %s failed: %s (日志将写不进去, 主循环继续)\n",
                now_ts().c_str(), PATROL_DIR, strerror(errno));

    // 信号: SIGINT/SIGTERM/SIGHUP -> 优雅退出
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_exit_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);
    signal(SIGPIPE, SIG_IGN); // 本程序只收不发, 顺手屏蔽防意外

    std::string led_mode_now = "not_read(no-led/dry-run)";
    if (mode == MODE_LED) led_mode_now = led_read_mode();

    printf("[%s] edgevision_patrol_daemon start pid=%ld mode=%s interval=%ds\n",
           now_ts().c_str(), (long)getpid(), mode_name, interval_s);
    printf("[%s] 阈值: person score>=%.2f 连续%d周期->PERSON_ALERT(连续%d周期无person恢复); "
           "zone0>%.1fC->THERMAL_ALERT(<%.1fC连续%d周期恢复)\n",
           now_ts().c_str(), (double)g_cfg_person_score, PERSON_ENTER_N, PERSON_EXIT_N,
           g_cfg_temp_enter, g_cfg_temp_exit, TEMP_EXIT_N);
    printf("[%s] LED trigger 当前: %s (目标文件 %s)\n",
           now_ts().c_str(), led_mode_now.c_str(), LED_TRIGGER);

    write_start_event(mode_name, interval_s, led_mode_now);

    // ---- 状态机运行时变量 ----
    bool person_active  = false;
    bool thermal_active = false;
    int  person_strike  = 0;   // 连续检出 person 计数(未告警时)
    int  person_clear   = 0;   // 连续无 person 计数(告警中)
    int  thermal_clear  = 0;   // 连续低于恢复阈值计数(告警中)
    int  prev_overall   = ST_NORMAL;
    std::string last_applied = "unknown"; // LED 已落到的目标态: alarm/heartbeat/unknown

    while (!g_stop) {
        std::chrono::steady_clock::time_point tick_start = std::chrono::steady_clock::now();

        // ---- 1. 采样(单项失败只影响该项, 不重试拖死周期) ----
        CycleData cd;
        cd.temp_ok = read_zone0_temp(cd.temp_c);
        cd.npu_ok  = read_npu_load(cd.c0, cd.c1, cd.c2);
        cd.v       = probe_vision_once();

        bool person_entered = false, thermal_entered = false;
        bool person_cleared = false, thermal_cleared = false;

        // ---- 2. person 域(视觉数据缺失 -> 冻结计数, 不误触发也不误恢复) ----
        if (cd.v.ok) {
            if (!person_active) {
                if (cd.v.person_seen) { person_strike++; person_clear = 0; }
                else                  { person_strike = 0; }
                if (person_strike >= PERSON_ENTER_N) {
                    person_active = true;
                    person_clear  = 0;
                    person_entered = true;
                }
            } else {
                if (cd.v.person_seen) person_clear = 0;
                else                  person_clear++;
                if (person_clear >= PERSON_EXIT_N) {
                    person_active  = false;
                    person_cleared = true;
                }
            }
        }

        // ---- 3. thermal 域(温度读不到 -> 冻结, 告警中读不到则保持报警, 失败安全) ----
        if (cd.temp_ok) {
            if (!thermal_active) {
                if (cd.temp_c > g_cfg_temp_enter) {
                    thermal_active  = true;
                    thermal_clear   = 0;
                    thermal_entered = true;
                }
            } else {
                if (cd.temp_c < g_cfg_temp_exit) {
                    thermal_clear++;
                    if (thermal_clear >= TEMP_EXIT_N) {
                        thermal_active  = false;
                        thermal_cleared = true;
                    }
                } else {
                    thermal_clear = 0; // 迟滞带内: 不触发新告警也不计入恢复
                }
            }
        }

        // ---- 4. LED 决策(单人单资源: 任一告警=alarm, 否则 heartbeat) ----
        bool want_alarm   = person_active || thermal_active;
        std::string desired = want_alarm ? "alarm" : "heartbeat";
        if (desired != last_applied) {
            if (mode == MODE_LED) {
                bool ok = want_alarm ? led_set_alarm() : led_set_heartbeat();
                cd.action  = want_alarm ? "set_alarm" : "set_heartbeat";
                cd.led_res = ok ? "ok" : "write_failed";
                if (ok) last_applied = desired; // 写失败保留旧值, 下一周期自动重试
                printf("[%s] LED -> %s [%s] (%s)\n", now_ts().c_str(),
                       desired.c_str(), cd.action.c_str(), cd.led_res.c_str());
            } else if (mode == MODE_DRYRUN) {
                cd.action  = want_alarm ? "would_set_alarm" : "would_set_heartbeat";
                cd.led_res = "dry_run_no_write";
                last_applied = desired;
                printf("[%s] LED(dry-run) %s\n", now_ts().c_str(), cd.action.c_str());
            } else { // MODE_NOLED
                cd.action  = "skipped_no_led";
                cd.led_res = "not_attempted";
                last_applied = desired;
            }
        } else {
            cd.action  = std::string("kept_") + desired;
            cd.led_res = "no_change";
        }

        // ---- 5. 整体状态与事件(状态翻转才写日志) ----
        int overall = person_active ? ST_PERSON : (thermal_active ? ST_THERMAL : ST_NORMAL);
        std::string ts = now_ts();
        std::string d_common = detail_common(cd)
                             + ",\"action\":\"" + cd.action + "\""
                             + ",\"led_result\":\"" + cd.led_res + "\"";

        if (person_entered) {
            std::string summary = std::string("画面连续") + std::to_string(PERSON_ENTER_N) +
                                  "周期检出人员(最高置信度" +
                                  (cd.v.best_person_score >= 0 ? f2(cd.v.best_person_score) : "?") +
                                  ", 帧" + (cd.v.ok ? std::to_string(cd.v.frame_id) : "-") +
                                  "), LED切换为报警快闪";
            std::string d = d_common + ",\"prev_state\":\"" + state_name(prev_overall) +
                            "\",\"new_state\":\"PERSON_ALERT\"";
            write_flip_event(ts, "PERSON_ALERT", summary, d);
        }
        if (thermal_entered) {
            std::string summary = std::string("SOC温度") + f2(cd.temp_c) +
                                  "C超过" + f2(g_cfg_temp_enter) + "C阈值, LED切换为报警快闪";
            std::string d = d_common + ",\"prev_state\":\"" + state_name(prev_overall) +
                            "\",\"new_state\":\"THERMAL_ALERT\"";
            write_flip_event(ts, "THERMAL_ALERT", summary, d);
        }
        if (prev_overall != ST_NORMAL && overall == ST_NORMAL) {
            std::string cleared = "[";
            bool first = true;
            if (person_cleared)  { cleared += "\"PERSON_ALERT\"";  first = false; }
            if (thermal_cleared) { if (!first) cleared += ","; cleared += "\"THERMAL_ALERT\""; }
            cleared += "]";
            std::string why;
            if (person_cleared)  why += std::string("人员告警解除(连续") + std::to_string(PERSON_EXIT_N) + "周期无person)";
            if (thermal_cleared) { if (!why.empty()) why += "、";
                                   why += std::string("温度回落至") + f2(g_cfg_temp_exit) +
                                          "C以下(连续" + std::to_string(TEMP_EXIT_N) + "周期)"; }
            std::string summary = why + ", LED恢复心跳呼吸";
            std::string d = d_common + ",\"recovered\":" + cleared + ",\"new_state\":\"NORMAL\"";
            write_flip_event(ts, "RECOVER", summary, d);
        }
        prev_overall = overall;

        // ---- 6. 心跳行(stdout) ----
        std::stringstream hb;
        hb << "[" << ts << "] state=" << state_name(overall)
           << " temp=" << (cd.temp_ok ? f2(cd.temp_c) + "C" : "NA")
           << " vision=" << cd.v.reason;
        if (cd.v.ok) {
            hb << " frame=" << cd.v.frame_id
               << " person=" << (cd.v.best_person_score >= 0 ? f2(cd.v.best_person_score) : "-");
        } else {
            hb << " frame=- person=-";
        }
        hb << " npu=" << (cd.npu_ok ?
                (std::to_string(cd.c0) + "%/" + std::to_string(cd.c1) + "%/" + std::to_string(cd.c2) + "%")
                : "parse_fail")
           << " led=" << last_applied;
        if (person_active)  hb << " p_clear=" << person_clear << "/" << PERSON_EXIT_N;
        if (thermal_active) hb << " t_clear=" << thermal_clear << "/" << TEMP_EXIT_N;
        printf("%s\n", hb.str().c_str());
        fflush(stdout);

        // ---- 7. 睡到下一周期(200ms 步进, 信号随时打断退出) ----
        std::chrono::steady_clock::time_point deadline = tick_start + std::chrono::seconds(interval_s);
        while (!g_stop) {
            std::chrono::steady_clock::time_point nowc = std::chrono::steady_clock::now();
            if (nowc >= deadline) break;
            long remain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - nowc).count();
            if (remain_ms > 200) remain_ms = 200;
            usleep((useconds_t)remain_ms * 1000);
        }
    }

    // ---- 优雅退出: 默认模式恢复 heartbeat, dry-run/no-led 不碰 LED ----
    if (mode == MODE_LED) {
        bool ok = led_set_heartbeat();
        printf("[%s] exit: LED restore heartbeat [%s]\n", now_ts().c_str(), ok ? "ok" : "FAILED");
    } else {
        printf("[%s] exit: %s mode, LED untouched\n", now_ts().c_str(), mode_name);
    }
    printf("[%s] patrol daemon exit\n", now_ts().c_str());
    return 0;
}
