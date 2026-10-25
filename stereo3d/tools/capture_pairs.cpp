// ============================================================================
//  capture_pairs.cpp —— 板端「双 USB 摄像头同一时刻抓图」工具（标定用）
//
//  为什么不能直接 cv::read() 两次？
//    cv::VideoCapture::read() = grab() + retrieve()。
//    grab() 只把 V4L2 队列里的帧 dequeue（拿到裸 MJPG 包，不解码），
//    retrieve() 才做 MJPG 解码。1280x720 的 MJPG 解码在 RK3588 上要 7~10ms，
//    如果写成 read(L) 再 read(R)，右图的时间点就被「解码左图」这件事往后推了，
//    两帧的时差 ≈ 解码耗时 + 排队，容易被放大成几十毫秒。
//    正确做法：先连续 grab() 两次（都把帧「钉」下来），再逐个 retrieve() 解码。
//    这样两帧的采样时刻只相差两次 grab 的返回间隔（实测 0.1~5 ms 量级）。
//
//  诚实说明（面试会问）：
//    普通 UVC 摄像头**没有硬件同步线**，两颗芯片各自按自己的 30fps 自由跑，
//    所以「同一时刻」只能是尽最大努力对齐，物理上仍可能有 < 1 帧周期(33.3ms) 的抖动。
//    本工具做两件事：
//      ① 把软件侧引入的额外时延压到最小（先 grab 再 retrieve）；
//      ② 用 std::chrono::steady_clock 打点，输出每对图的 grab 时间差，
//         并顺带读取 V4L2 驱动给的帧时间戳（CAP_PROP_POS_MSEC，后端提供时才有意义），
//         把「我到底同步到什么程度」变成可量化、可写进报告的实测数字。
//
//  编译（板端原生编译，别交叉编译，glibc 版本不对会炸）：
//    g++ -O2 -std=c++17 capture_pairs.cpp $(pkg-config --cflags --libs opencv4) -o capture_pairs
//
//  用法：
//    # 交互：终端每按一次回车抓一对，输入 q 回车退出
//    ./capture_pairs --out /root/stereo3d/calib/captures
//
//    # 自动：抓 20 对，间隔 1.5 秒（配合 PC 端 capture_pairs.py 用）
//    ./capture_pairs --out /root/stereo3d/calib/captures --auto 20 --interval 1.5
//
//    # 断点续抓：从编号 12 开始
//    ./capture_pairs --out DIR --auto 10 --start 12
//
//  参数：
//    --left /dev/video81   左摄像头（默认）
//    --right /dev/video83  右摄像头（默认）
//    --out DIR             输出目录（默认 ./pairs）
//    --w 1280 --h 720      分辨率（默认 1280x720）
//    --fps 30              请求帧率（默认 30；注意：V4L2 不设帧率会协商成 20fps）
//    --auto N              自动抓 N 对（默认 0 = 交互模式）
//    --interval S          自动模式每对间隔秒数（默认 1.5）
//    --start N             起始编号（默认 0，断点续抓用）
//    --warmup N            预热帧数（默认 20，等自动曝光/白平衡收敛）
//    --verbose             额外打印每对的 grab 耗时、驱动时间戳
//    --help                帮助
//
//  输出：
//    left_000.png / right_000.png ...   无损 PNG（标定必须无损，JPEG 的块效应会污染角点亚像素精度）
//    capture_log.txt                    每对的时间戳差，供标定报告引用
// ============================================================================
#include <opencv2/opencv.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static double msBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// ---------------------------------------------------------------------------
// 命令行参数
// ---------------------------------------------------------------------------
struct Options {
    std::string left = "/dev/video81";
    std::string right = "/dev/video83";
    std::string out = "./pairs";
    int w = 1280;
    int h = 720;
    int fps = 30;
    int auto_n = 0;          // 0 = 交互模式
    double interval = 1.5;   // 自动模式间隔（秒）
    int start = 0;           // 起始编号
    int warmup = 20;         // 预热帧数
    bool verbose = false;
};

static void printUsage(const char* exe) {
    printf("用法: %s [选项]\n", exe);
    printf("  --left  DEV      左摄像头 (默认 /dev/video81)\n");
    printf("  --right DEV      右摄像头 (默认 /dev/video83)\n");
    printf("  --out   DIR      输出目录 (默认 ./pairs)\n");
    printf("  --w N --h N      分辨率 (默认 1280x720)\n");
    printf("  --fps N          帧率 (默认 30)\n");
    printf("  --auto N         自动抓 N 对 (默认 0 = 交互：回车抓一对，q 退出)\n");
    printf("  --interval S     自动模式间隔秒 (默认 1.5)\n");
    printf("  --start N        起始编号 (默认 0)\n");
    printf("  --warmup N       预热帧数 (默认 20)\n");
    printf("  --verbose        打印更多时间细节\n");
    printf("  --help           显示本帮助\n");
}

static bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                printf("[-] 参数 %s 缺少取值\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--left") {
            const char* v = need("--left"); if (!v) return false; o.left = v;
        } else if (a == "--right") {
            const char* v = need("--right"); if (!v) return false; o.right = v;
        } else if (a == "--out") {
            const char* v = need("--out"); if (!v) return false; o.out = v;
        } else if (a == "--w") {
            const char* v = need("--w"); if (!v) return false; o.w = atoi(v);
        } else if (a == "--h") {
            const char* v = need("--h"); if (!v) return false; o.h = atoi(v);
        } else if (a == "--fps") {
            const char* v = need("--fps"); if (!v) return false; o.fps = atoi(v);
        } else if (a == "--auto") {
            const char* v = need("--auto"); if (!v) return false; o.auto_n = atoi(v);
        } else if (a == "--interval") {
            const char* v = need("--interval"); if (!v) return false; o.interval = atof(v);
        } else if (a == "--start") {
            const char* v = need("--start"); if (!v) return false; o.start = atoi(v);
        } else if (a == "--warmup") {
            const char* v = need("--warmup"); if (!v) return false; o.warmup = atoi(v);
        } else if (a == "--verbose" || a == "-v") {
            o.verbose = true;
        } else if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return false;
        } else {
            printf("[-] 不认识的参数: %s\n", a.c_str());
            printUsage(argv[0]);
            return false;
        }
    }
    if (o.w <= 0 || o.h <= 0 || o.fps <= 0) {
        printf("[-] 分辨率/帧率必须为正数\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 打开一路 V4L2 摄像头
//   坑（项目里踩过，见 docs/CAMERA_FPS_ROOT_CAUSE.md）：
//   V4L2 后端如果不显式 set(CAP_PROP_FPS)，会默认协商成 20fps，而不是 30fps。
//   BUFFERSIZE=1 让驱动只保留 1 个缓冲，grab() 拿到的永远是「最新」那一帧，
//   否则队列里会积压旧帧，抓出来的图比真实时间晚好几帧（标定时表现为画面和姿势对不上）。
// ---------------------------------------------------------------------------
static bool openCam(cv::VideoCapture& cap, const std::string& dev, const Options& o) {
    cap.open(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        printf("[-] 打不开 %s（检查设备是否存在、是否被别的进程占用: fuser -v %s）\n",
               dev.c_str(), dev.c_str());
        return false;
    }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, o.w);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, o.h);
    cap.set(cv::CAP_PROP_FPS, o.fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    const double fourcc = cap.get(cv::CAP_PROP_FOURCC);
    char code[5] = {0};
    code[0] = (char)((long)fourcc & 0xFF);
    code[1] = (char)(((long)fourcc >> 8) & 0xFF);
    code[2] = (char)(((long)fourcc >> 16) & 0xFF);
    code[3] = (char)(((long)fourcc >> 24) & 0xFF);
    printf("  [ok] %s -> %dx%d  fourcc=%s  fps(回读)=%.1f  buffersize=%.0f\n",
           dev.c_str(),
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
           code,
           cap.get(cv::CAP_PROP_FPS),
           cap.get(cv::CAP_PROP_BUFFERSIZE));
    return true;
}

// ---------------------------------------------------------------------------
// 一对图的时间打点
// ---------------------------------------------------------------------------
struct PairStamp {
    bool   ok = false;         // 两路都成功取到非空图
    double grab_l_ms = 0.0;    // 左 grab 自身的阻塞时长（≈ 等下一帧的时间）
    double grab_r_ms = 0.0;    // 右 grab 自身的阻塞时长
    double skew_ms = 0.0;      // 左 grab 返回 -> 右 grab 返回 的间隔（核心同步指标，越小越好）
    double decode_ms = 0.0;    // 两次 retrieve（MJPG 解码）的总耗时
    double pos_l_ms = 0.0;     // V4L2 驱动帧时间戳（后端没给就是 0 / 不可信）
    double pos_r_ms = 0.0;
    bool   pos_valid = false;  // 两路都有 >0 的驱动时间戳，才算能拿来算时间差
};

// ---------------------------------------------------------------------------
// 抓一对：★ 先 grab 两次，再 retrieve 两次
//   1) grab(L)   只 dequeue 不解码
//   2) grab(R)   紧接着 dequeue，两帧的采样时刻差≈0
//   3) retrieve(L) / retrieve(R)  这时才 MJPG 解码（耗时不污染同步精度）
// ---------------------------------------------------------------------------
static PairStamp grabPair(cv::VideoCapture& cl, cv::VideoCapture& cr,
                          cv::Mat& L, cv::Mat& R) {
    PairStamp s;

    const auto t0 = Clock::now();
    const bool okL = cl.grab();
    const auto tL = Clock::now();
    const bool okR = cr.grab();
    const auto tR = Clock::now();
    if (!okL || !okR) {
        return s;  // ok=false
    }

    const bool rL = cl.retrieve(L);
    const bool rR = cr.retrieve(R);
    const auto tEnd = Clock::now();

    if (!rL || !rR || L.empty() || R.empty()) {
        return s;
    }

    s.ok = true;
    s.grab_l_ms = msBetween(t0, tL);
    s.grab_r_ms = msBetween(tL, tR);
    s.skew_ms = msBetween(tL, tR);       // 右图相对左图晚多少（毫秒，带符号）
    s.decode_ms = msBetween(tR, tEnd);
    s.pos_l_ms = cl.get(cv::CAP_PROP_POS_MSEC);
    s.pos_r_ms = cr.get(cv::CAP_PROP_POS_MSEC);
    s.pos_valid = (s.pos_l_ms > 0.0 && s.pos_r_ms > 0.0);
    return s;
}

// 统计小工具：均值 / p95 / 最大值
struct BasicStats { double mean = 0, p95 = 0, mx = 0, mn = 0; };
static BasicStats statOf(std::vector<double> v) {
    BasicStats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    const double sum = std::accumulate(v.begin(), v.end(), 0.0);
    s.mean = sum / (double)v.size();
    s.p95 = v[(size_t)std::min<double>((double)v.size() * 0.95, (double)v.size() - 1)];
    s.mx = v.back();
    s.mn = v.front();
    return s;
}

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) {
        return 1;
    }

    printf("=== 双摄同步抓图 capture_pairs ===\n");
    printf("左: %s   右: %s\n", o.left.c_str(), o.right.c_str());
    printf("分辨率: %dx%d @ %dfps   输出目录: %s\n", o.w, o.h, o.fps, o.out.c_str());
    printf("模式: %s", o.auto_n > 0 ? "自动" : "交互");
    if (o.auto_n > 0) {
        printf("（共 %d 对，间隔 %.2f 秒）\n", o.auto_n, o.interval);
    } else {
        printf("（回车抓一对，q 回车退出）\n");
    }
    printf("提示: 棋盘格要完整入镜、不反光、尽量铺满画面；每次换一个角度/距离。\n\n");

    // 输出目录（C++17 filesystem，g++10 自带，不用额外链接）
    std::error_code ec;
    std::filesystem::create_directories(o.out, ec);
    if (ec) {
        printf("[-] 建目录失败 %s: %s\n", o.out.c_str(), ec.message().c_str());
        return 1;
    }

    cv::VideoCapture cl, cr;
    if (!openCam(cl, o.left, o)) return 1;
    if (!openCam(cr, o.right, o)) return 1;

    // ------------------------------------------------------------------
    // 预热：CMOS 的自动曝光(AE)/自动白平衡(AWB) 需要十几帧才收敛，
    // 不预热直接抓，前几张图会明显偏暗或偏色，角点检测容易失败。
    // ------------------------------------------------------------------
    printf("\n[*] 预热 %d 帧（等自动曝光/白平衡收敛）...\n", o.warmup);
    cv::Mat tmp;
    int warmed = 0;
    for (int i = 0; i < o.warmup; ++i) {
        if (cl.grab()) warmed += 1;
        if (cr.grab()) warmed += 1;
        cl.retrieve(tmp);
        cr.retrieve(tmp);
    }
    printf("[*] 预热完成（成功 grab %d 次）\n\n", warmed);

    // 日志文件
    const std::string logPath = o.out + "/capture_log.txt";
    std::ofstream log(logPath, std::ios::out | std::ios::trunc);
    if (!log) {
        printf("[-] 打不开日志文件 %s\n", logPath.c_str());
        return 1;
    }
    log << "# capture_log.txt —— 双摄同步抓图记录（capture_pairs 自动生成）\n";
    log << "# device_left=" << o.left << "  device_right=" << o.right << "\n";
    log << "# size=" << o.w << "x" << o.h << "  fps=" << o.fps
        << "  warmup=" << o.warmup << "\n";
    log << "# grab_skew_ms = 左 grab 返回 -> 右 grab 返回 的间隔（越小越同步；UVC 无硬件同步）\n";
    log << "# pos_*_ms = CAP_PROP_POS_MSEC（V4L2 驱动帧时间戳，后端不支持时为 0/不可信）\n";
    log << "# idx  grab_skew_ms  grab_l_ms  grab_r_ms  decode_ms  pos_L_ms  pos_R_ms  pos_delta_ms\n";

    std::vector<double> skews, decodes, posDeltas;
    int saved = 0;
    int idx = o.start;

    auto captureOne = [&]() -> bool {
        cv::Mat L, R;
        const PairStamp s = grabPair(cl, cr, L, R);
        if (!s.ok) {
            printf("[-] 第 %d 对取图失败（grab/retrieve 返回空）\n", idx);
            return false;
        }
        // 时间戳差用 +3f 保持符号，便于看谁先谁后
        printf("[%03d] 同步差 grab_skew = %+7.3f ms | 解码 %.2f ms", idx, s.skew_ms, s.decode_ms);
        if (s.pos_valid) {
            printf(" | 驱动时间戳差 %+7.3f ms", s.pos_r_ms - s.pos_l_ms);
        }
        printf("\n");

        char fnL[256], fnR[256];
        snprintf(fnL, sizeof(fnL), "%s/left_%03d.png", o.out.c_str(), idx);
        snprintf(fnR, sizeof(fnR), "%s/right_%03d.png", o.out.c_str(), idx);
        // PNG 无损：JPEG 的 8x8 DCT 块效应会污染棋盘格角点的亚像素位置
        if (!cv::imwrite(fnL, L) || !cv::imwrite(fnR, R)) {
            printf("[-] 写图失败: %s / %s\n", fnL, fnR);
            return false;
        }
        printf("      -> left_%03d.png  right_%03d.png  (%dx%d)\n", idx, idx, L.cols, L.rows);

        if (o.verbose) {
            printf("      grab 左 %.3f ms / 右 %.3f ms; pos_L %.1f  pos_R %.1f\n",
                   s.grab_l_ms, s.grab_r_ms, s.pos_l_ms, s.pos_r_ms);
        }

        log << idx << "  " << s.skew_ms << "  " << s.grab_l_ms << "  " << s.grab_r_ms
            << "  " << s.decode_ms << "  " << s.pos_l_ms << "  " << s.pos_r_ms;
        if (s.pos_valid) log << "  " << (s.pos_r_ms - s.pos_l_ms);
        else             log << "  nan";
        log << "\n";
        log.flush();

        skews.push_back(s.skew_ms);
        decodes.push_back(s.decode_ms);
        if (s.pos_valid) posDeltas.push_back(s.pos_r_ms - s.pos_l_ms);
        ++idx;
        ++saved;
        return true;
    };

    if (o.auto_n > 0) {
        for (int k = 0; k < o.auto_n; ++k) {
            if (!captureOne()) break;
            if (k + 1 < o.auto_n && o.interval > 0.0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(o.interval));
            }
        }
    } else {
        std::string line;
        while (true) {
            printf("\n[%03d] 摆好棋盘格后按回车抓图（q 回车退出）: ", idx);
            fflush(stdout);
            if (!std::getline(std::cin, line)) {
                break;  // stdin 关闭（比如被脚本管道调用）就退出
            }
            const std::string t = [&] {
                std::string s2 = line;
                while (!s2.empty() && (s2.back() == '\r' || s2.back() == ' ')) s2.pop_back();
                return s2;
            }();
            if (t == "q" || t == "quit" || t == "Q") {
                break;
            }
            captureOne();
        }
    }

    // ------------------------------------------------------------------
    // 汇总：把「同步到什么程度」量化下来，写进日志和终端
    // ------------------------------------------------------------------
    const BasicStats ss = statOf(skews);
    const BasicStats sd = statOf(decodes);

    log << "#\n";
    log << "# summary: pairs=" << saved << "\n";
    log << "# grab_skew_ms  mean=" << ss.mean << "  p95=" << ss.p95
        << "  min=" << ss.mn << "  max=" << ss.mx << "\n";
    log << "# decode_ms     mean=" << sd.mean << "  p95=" << sd.p95
        << "  max=" << sd.mx << "\n";
    if (!posDeltas.empty()) {
        const BasicStats sp = statOf(posDeltas);
        log << "# pos_delta_ms  mean=" << sp.mean << "  p95=" << sp.p95
            << "  min=" << sp.mn << "  max=" << sp.mx << "\n";
    }
    log.close();

    printf("\n=== 完成：共 %d 对，输出 %s ===\n", saved, o.out.c_str());
    if (saved > 0) {
        printf("同步差 grab_skew : 均值 %+.3f ms  p95 %+.3f ms  min %+.3f  max %+.3f\n",
               ss.mean, ss.p95, ss.mn, ss.mx);
        printf("MJPG 解码耗时    : 均值 %.2f ms   p95 %.2f ms\n", sd.mean, sd.p95);
        if (!posDeltas.empty()) {
            const BasicStats sp = statOf(posDeltas);
            printf("驱动时间戳差     : 均值 %+.3f ms  p95 %+.3f ms（来自 V4L2，仅供参考）\n",
                   sp.mean, sp.p95);
        } else {
            printf("驱动时间戳差     : 该后端未提供 CAP_PROP_POS_MSEC，跳过\n");
        }
        printf("日志: %s\n", logPath.c_str());
        printf("\n下一步（PC 端）: python tools/calib/capture_pairs.py --check-only --out <本地目录>\n");
    }
    return saved > 0 ? 0 : 2;
}
