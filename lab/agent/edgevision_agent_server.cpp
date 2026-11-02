// ============================================================================
//  edgevision_agent_server.cpp —— RK3588 边缘智能体硬件控制面服务 (HTTP/JSON RPC)
//
//  架构职责：
//    1. 暴露标准 RESTful HTTP 接口，专门对接大模型 (LLM) 的 Tool Calling (函数调用)
//    2. 硬件原子能力：
//       - GET  /api/status  : 采集 7 大物理温区、3核 NPU 负载、内存状态
//       - POST /api/led     : 控制板载物理 LED（heartbeat呼吸 / alarm快闪 / on常亮 / off关闭）
//       - GET  /api/vision  : 采样最新一帧 NPU 目标检测结果（人、手机、车辆等）
//
//  编译运行 (板端原生编译，无需外部库):
//    g++ -O2 -std=c++17 edgevision_agent_server.cpp -o edgevision_agent_server -lpthread
// ============================================================================

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <fstream>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define AGENT_HTTP_PORT 8088
#define VISION_DATA_PORT 9000

// -------------------------------------------------------------
// 1. COCO 80 类别名称映射
// -------------------------------------------------------------
static const char* COCO80_NAMES[] = {
    "人", "自行车", "汽车", "摩托车", "飞机", "公交车", "火车", "卡车", "船", "红绿灯",
    "消防栓", "停止标志", "停车计时器", "长凳", "鸟", "猫", "狗", "马", "羊", "牛",
    "大象", "熊", "斑马", "长颈鹿", "背包", "雨伞", "手提包", "领带", "手提箱", "飞盘",
    "滑雪板", "单板滑雪", "运动球", "风筝", "棒球棒", "棒球手套", "滑板", "冲浪板", "网球拍", "瓶子",
    "红酒杯", "水杯", "叉子", "刀", "勺子", "碗", "香蕉", "苹果", "三明治", "橙子",
    "西兰花", "胡萝卜", "热狗", "披萨", "甜甜圈", "蛋糕", "椅子", "沙发", "盆栽", "床",
    "餐桌", "马桶", "电视/显示器", "笔记本电脑", "鼠标", "遥控器", "键盘", "手机", "微波炉", "烤箱",
    "烤面包机", "水槽", "冰箱", "书", "时钟", "花瓶", "剪刀", "泰迪熊", "吹风机", "牙刷"
};

// -------------------------------------------------------------
// 2. 9000 端口推流协议对齐结构体
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

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

// -------------------------------------------------------------
// 3. 硬件能力实现：读取温区、NPU 负载与内存
// -------------------------------------------------------------
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
    return true;
}

// 采集系统健康数据并组装为标准 JSON
std::string get_system_health_json() {
    std::stringstream json;
    json << "{\n";

    // 1. 采集 7 大物理温区
    json << "  \"thermal_zones\": {\n";
    bool first = true;
    for (int i = 0; i < 7; ++i) {
        std::string type = read_file_string("/sys/class/thermal/thermal_zone" + std::to_string(i) + "/type");
        std::string temp_str = read_file_string("/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp");
        if (type.empty() || temp_str.empty()) continue;

        double temp_c = std::stod(temp_str) / 1000.0;
        if (!first) json << ",\n";
        json << "    \"" << type << "\": " << temp_c;
        first = false;
    }
    json << "\n  },\n";

    // 2. 采集 NPU 3核实时负载
    json << "  \"npu_load\": {\n";
    std::string npu_raw = read_file_string("/sys/kernel/debug/rknpu/load");
    // 解析形如 "NPU load:  Core0: 14%, Core1: 14%, Core2: 14%,"
    int c0 = 0, c1 = 0, c2 = 0;
    if (!npu_raw.empty()) {
        sscanf(npu_raw.c_str(), "NPU load: Core0: %d%%, Core1: %d%%, Core2: %d%%", &c0, &c1, &c2);
    }
    json << "    \"core0_percent\": " << c0 << ",\n";
    json << "    \"core1_percent\": " << c1 << ",\n";
    json << "    \"core2_percent\": " << c2 << "\n";
    json << "  },\n";

    // 3. 采集内存使用 (MB)
    std::ifstream meminfo("/proc/meminfo");
    long total_kb = 0, avail_kb = 0;
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0) sscanf(line.c_str(), "MemTotal: %ld", &total_kb);
        if (line.rfind("MemAvailable:", 0) == 0) sscanf(line.c_str(), "MemAvailable: %ld", &avail_kb);
    }
    json << "  \"memory\": {\n";
    json << "    \"total_mb\": " << (total_kb / 1024) << ",\n";
    json << "    \"available_mb\": " << (avail_kb / 1024) << ",\n";
    json << "    \"used_mb\": " << ((total_kb - avail_kb) / 1024) << "\n";
    json << "  }\n";

    json << "}";
    return json.str();
}

// -------------------------------------------------------------
// 4. 硬件能力实现：控制板载物理 LED
// -------------------------------------------------------------
bool set_led_hardware_mode(const std::string& mode, std::string& err_msg) {
    const std::string trigger_path = "/sys/class/leds/work/trigger";
    const std::string bright_path  = "/sys/class/leds/work/brightness";
    const std::string delay_on     = "/sys/class/leds/work/delay_on";
    const std::string delay_off    = "/sys/class/leds/work/delay_off";

    if (mode == "heartbeat") {
        return write_file_string(trigger_path, "heartbeat");
    } else if (mode == "alarm") {
        // 报警模式：100ms 快速爆闪
        write_file_string(trigger_path, "timer");
        usleep(20000);
        write_file_string(delay_on, "100");
        write_file_string(delay_off, "100");
        return true;
    } else if (mode == "on") {
        write_file_string(trigger_path, "none");
        return write_file_string(bright_path, "1");
    } else if (mode == "off") {
        write_file_string(trigger_path, "none");
        return write_file_string(bright_path, "0");
    } else {
        err_msg = "Unknown mode. Supported: heartbeat, alarm, on, off";
        return false;
    }
}

// -------------------------------------------------------------
// 5. 硬件能力实现：从 9000 端口实时抽取最新视觉帧
// -------------------------------------------------------------
std::string get_vision_snapshot_json() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "{\"error\": \"Socket create failed\"}";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(VISION_DATA_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // 设置 1 秒超时
    timeval tv{1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "{\"error\": \"Cannot connect to EdgeVision 9000 stream service\"}";
    }

    // 接收定长 248 字节帧
    FrameResult res{};
    size_t received = 0;
    char* ptr = (char*)&res;
    while (received < sizeof(res)) {
        ssize_t n = recv(sock, ptr + received, sizeof(res) - received, 0);
        if (n <= 0) break;
        received += (size_t)n;
    }
    close(sock);

    if (received < sizeof(res)) {
        return "{\"error\": \"Incomplete frame received from vision stream\"}";
    }

    // 构造结构化 JSON
    std::stringstream json;
    json << "{\n";
    json << "  \"frame_id\": " << res.header.frame_id << ",\n";
    json << "  \"object_count\": " << res.header.bbox_count << ",\n";
    json << "  \"objects\": [\n";

    uint32_t count = std::min(res.header.bbox_count, (uint32_t)MAX_BBOXES);
    for (uint32_t i = 0; i < count; ++i) {
        int cid = res.boxes[i].class_id;
        std::string name = (cid >= 0 && cid < 80) ? COCO80_NAMES[cid] : "未知";
        json << "    {\n";
        json << "      \"class_name\": \"" << name << "\",\n";
        json << "      \"confidence\": " << (int)(res.boxes[i].score * 100) << ",\n";
        json << "      \"box\": {\"x\": " << (int)res.boxes[i].x << ", \"y\": " << (int)res.boxes[i].y 
             << ", \"w\": " << (int)res.boxes[i].w << ", \"h\": " << (int)res.boxes[i].h << "}\n";
        json << "    }" << (i + 1 < count ? ",\n" : "\n");
    }
    json << "  ]\n";
    json << "}";
    return json.str();
}

// -------------------------------------------------------------
// 6. 微型 HTTP 响应生成与路由分发
// -------------------------------------------------------------
static void send_http_response(int fd, int status_code, const std::string& body) {
    std::string header = 
        "HTTP/1.1 " + std::to_string(status_code) + " OK\r\n"
        "Server: RK3588-EdgeAgent-RPC/1.0\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n";

    send(fd, header.c_str(), header.size(), MSG_NOSIGNAL);
    send(fd, body.c_str(), body.size(), MSG_NOSIGNAL);
}

void* http_worker_thread(void* arg) {
    int c_fd = *(int*)arg;
    delete (int*)arg;

    char buf[2048] = {0};
    ssize_t n = recv(c_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(c_fd); return nullptr; }

    std::string req(buf);
    std::string method, path;
    std::stringstream ss(req);
    ss >> method >> path;

    std::cout << "[Agent RPC] " << method << " " << path << std::endl;

    if (method == "GET" && path == "/api/status") {
        // 工具 1：系统健康状态查询
        std::string json = get_system_health_json();
        send_http_response(c_fd, 200, json);
    } 
    else if (path.rfind("/api/led", 0) == 0) {
        // 工具 2：控制板载物理 LED (支持 GET /api/led?mode=xxx 或 POST)
        std::string mode = "heartbeat";
        size_t q_pos = path.find("mode=");
        if (q_pos != std::string::npos) {
            mode = path.substr(q_pos + 5);
        } else {
            // 从 POST body 中提取
            size_t b_pos = req.find("mode=");
            if (b_pos != std::string::npos) {
                mode = req.substr(b_pos + 5);
                while (!mode.empty() && (mode.back() == '\r' || mode.back() == '\n' || mode.back() == ' '))
                    mode.pop_back();
            }
        }

        std::string err;
        bool ok = set_led_hardware_mode(mode, err);
        if (ok) {
            send_http_response(c_fd, 200, "{\"success\": true, \"mode\": \"" + mode + "\"}");
        } else {
            send_http_response(c_fd, 400, "{\"success\": false, \"error\": \"" + err + "\"}");
        }
    } 
    else if (method == "GET" && path == "/api/vision") {
        // 工具 3：抽取最新视觉检测感知帧
        std::string json = get_vision_snapshot_json();
        send_http_response(c_fd, 200, json);
    } 
    else {
        // 欢迎页与 API 说明
        std::string help = 
            "{\n"
            "  \"service\": \"EdgeVision RK3588 Tool Calling Agent RPC Server\",\n"
            "  \"version\": \"1.0.0\",\n"
            "  \"tools\": [\n"
            "    {\"endpoint\": \"GET /api/status\", \"description\": \"Query 7-zone thermals, NPU 3-core load, and RAM usage\"},\n"
            "    {\"endpoint\": \"GET/POST /api/led?mode=heartbeat|alarm|on|off\", \"description\": \"Control physical onboard LED mode\"},\n"
            "    {\"endpoint\": \"GET /api/vision\", \"description\": \"Get real-time detected visual objects from NPU 30FPS pipeline\"}\n"
            "  ]\n"
            "}";
        send_http_response(c_fd, 200, help);
    }

    close(c_fd);
    return nullptr;
}

// -------------------------------------------------------------
// 7. 主循环：监听 8088 端口
// -------------------------------------------------------------
int main() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(AGENT_HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[-] bind error");
        return 1;
    }
    listen(server_fd, 10);

    std::cout << "=========================================================\n";
    std::cout << " 🤖 EdgeVision RK3588 Agent RPC Tool Server 启动就绪！\n";
    std::cout << " 硬件控制面端口: HTTP " << AGENT_HTTP_PORT << "\n";
    std::cout << " 已注入工具链: /api/status | /api/led | /api/vision\n";
    std::cout << "=========================================================\n";

    while (g_running) {
        timeval tv{1, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        int sel = select(server_fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int c_fd = accept(server_fd, (sockaddr*)&client_addr, &len);
        if (c_fd < 0) continue;

        pthread_t tid;
        int* pfd = new int(c_fd);
        pthread_create(&tid, nullptr, http_worker_thread, pfd);
        pthread_detach(tid);
    }

    close(server_fd);
    std::cout << "[Agent RPC] 服务已优雅退出。\n";
    return 0;
}
