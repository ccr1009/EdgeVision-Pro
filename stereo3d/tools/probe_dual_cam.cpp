// ============================================================================
//  probe_dual_cam.cpp —— 可行性探针：在写正式代码之前，先用真实数据回答三个问题
//
//  Q1 两颗摄像头能不能同时打开？（UVC 带宽/供电/驱动冲突）
//  Q2 各自的实际分辨率、帧率、单帧耗时是多少？
//  Q3 SGBM 在 640x480 上到底多慢？视差范围多少够用？
//
//  为什么先写这个？
//    端侧部署最忌讳「先写一周代码，最后发现硬件跑不动」。
//    先用 100 行探针把硬件能力拍成数字，再决定处理分辨率、视差范围、线程模型。
//    这套「先量数据再动手」的方法论本身就是面试可讲的点。
//
//  用法（板端）：
//    g++ -O2 -std=c++17 probe_dual_cam.cpp $(pkg-config --cflags --libs opencv4) -o probe
//    ./probe                 # 全部测
//    ./probe --quick         # 只测双摄能否同时打开
// ============================================================================
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <chrono>
#include <vector>
#include <algorithm>
#include <string>
#include <cmath>

using Clock = std::chrono::steady_clock;
static inline double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Stat { double mean, p50, p95, p99, min, max; };
static Stat summarize(std::vector<double> v) {
    Stat s{};
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / v.size();
    s.p50 = v[v.size() * 50 / 100];
    s.p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
    s.p99 = v[std::min(v.size() - 1, v.size() * 99 / 100)];
    s.min = v.front();
    s.max = v.back();
    return s;
}

int main(int argc, char** argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--quick") quick = true;

    const std::string devL = "/dev/video81";
    const std::string devR = "/dev/video83";

    printf("==================== 双目可行性探针 ====================\n\n");

    // ---------------------------------------------------------------
    // Q1: 能否同时打开
    // ---------------------------------------------------------------
    printf("---- Q1: 双摄同时打开 ----\n");
    cv::VideoCapture capL(devL, cv::CAP_V4L2);
    cv::VideoCapture capR(devR, cv::CAP_V4L2);
    printf("左目 %s opened=%d\n", devL.c_str(), (int)capL.isOpened());
    printf("右目 %s opened=%d\n", devR.c_str(), (int)capR.isOpened());
    if (!capL.isOpened() || !capR.isOpened()) {
        printf("[-] 至少一路打不开，退出\n");
        if (capL.isOpened()) printf("    （左目单独可用）\n");
        if (capR.isOpened()) printf("    （右目单独可用）\n");
        return 1;
    }

    for (auto* c : {&capL, &capR}) {
        c->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        c->set(cv::CAP_PROP_FRAME_WIDTH, 1280);
        c->set(cv::CAP_PROP_FRAME_HEIGHT, 720);
        c->set(cv::CAP_PROP_FPS, 30);
        c->set(cv::CAP_PROP_BUFFERSIZE, 1);
    }

    cv::Mat L, R;
    for (int i = 0; i < 20; ++i) { capL >> L; capR >> R; }     // 预热 AE/AWB
    if (L.empty() || R.empty()) { printf("[-] 预热取帧失败\n"); return 1; }
    printf("[+] 实际分辨率 左 %dx%d / 右 %dx%d\n", L.cols, L.rows, R.cols, R.rows);
    printf("[+] 结论: 双摄可同时工作\n\n");

    if (quick) return 0;

    // ---------------------------------------------------------------
    // Q2: 取帧耗时
    // ---------------------------------------------------------------
    printf("---- Q2: 取一对帧(先grab后retrieve)的耗时 x200 ----\n");
    {
        std::vector<double> ts;
        for (int i = 0; i < 200; ++i) {
            auto t0 = Clock::now();
            if (!capL.grab()) continue;
            capR.grab();
            capL.retrieve(L);
            capR.retrieve(R);
            ts.push_back(msSince(t0));
        }
        Stat s = summarize(ts);
        printf("  mean %.2f ms  p50 %.2f  p95 %.2f  p99 %.2f  min %.2f  max %.2f\n",
               s.mean, s.p50, s.p95, s.p99, s.min, s.max);
        printf("  -> 等效 %.1f fps（受摄像头出帧节奏限制）\n\n", 1000.0 / std::max(1e-3, s.mean));
    }

    // ---------------------------------------------------------------
    // Q3: SGBM 在不同分辨率下的耗时
    // ---------------------------------------------------------------
    printf("---- Q3: SGBM 耗时（numDisp=64, blockSize=5）----\n");
    printf("  %-12s %-10s %-10s %-10s %-10s %-12s\n",
           "分辨率", "scale", "mean(ms)", "p50", "p95", "有效像素%");
    const int numDisp = 64, bs = 5;
    for (const auto& sz : {cv::Size(320, 240), cv::Size(480, 360),
                           cv::Size(640, 480), cv::Size(960, 540)}) {
        cv::Mat gl, gr;
        cv::cvtColor(L, gl, cv::COLOR_BGR2GRAY);
        cv::cvtColor(R, gr, cv::COLOR_BGR2GRAY);
        cv::resize(gl, gl, sz);
        cv::resize(gr, gr, sz);

        // 注意：OpenCV 4.5.x 的 StereoSGBM::create 只有 11 个参数
        //（没有 4.8+ 才有的 mode 参数），板子是 4.5.1，必须按 11 参调用
        auto sgbm = cv::StereoSGBM::create(0, numDisp, bs,
                                          8 * bs * bs, 32 * bs * bs,
                                          1, 10, 100, 32, 63,
                                          0);
        std::vector<double> ts;
        cv::Mat disp;
        int valid = 0, total = 0;
        for (int i = 0; i < 15; ++i) {
            auto t0 = Clock::now();
            sgbm->compute(gl, gr, disp);
            ts.push_back(msSince(t0));
            if (i == 14) {
                for (int y = 0; y < disp.rows; ++y)
                    for (int x = 0; x < disp.cols; ++x) {
                        ++total;
                        if (disp.at<short>(y, x) > 0) ++valid;
                    }
            }
        }
        Stat s = summarize(ts);
        printf("  %dx%-8d %-10.2f %-10.2f %-10.2f %-10.2f %.1f%%\n",
               sz.width, sz.height, (double)sz.width / L.cols,
               s.mean, s.p50, s.p95, 100.0 * valid / std::max(1, total));
    }
    printf("\n");

    // ---------------------------------------------------------------
    // Q4: 双摄真并行（两个线程各读一路）vs 串行
    // ---------------------------------------------------------------
    printf("---- Q4: HTTP JPEG 编码耗时（1280x720 四宫格 1280x720）----\n");
    {
        cv::Mat grid(720, 1280, CV_8UC3);
        cv::randu(grid, 0, 255);
        std::vector<double> ts;
        std::vector<uchar> buf;
        for (int i = 0; i < 20; ++i) {
            auto t0 = Clock::now();
            cv::imencode(".jpg", grid, buf, {cv::IMWRITE_JPEG_QUALITY, 80});
            ts.push_back(msSince(t0));
        }
        Stat s = summarize(ts);
        printf("  mean %.2f ms  p95 %.2f ms  -> %zu KB\n\n", s.mean, s.p95, buf.size() / 1024);

        // 640x360 半尺寸对比
        cv::Mat half;
        cv::resize(grid, half, cv::Size(640, 360));
        ts.clear();
        for (int i = 0; i < 20; ++i) {
            auto t0 = Clock::now();
            cv::imencode(".jpg", half, buf, {cv::IMWRITE_JPEG_QUALITY, 80});
            ts.push_back(msSince(t0));
        }
        s = summarize(ts);
        printf("  640x360: mean %.2f ms  -> %zu KB\n\n", s.mean, buf.size() / 1024);
    }

    // ---------------------------------------------------------------
    // Q5: Haar 人脸检测耗时
    // ---------------------------------------------------------------
    printf("---- Q5: Haar 人脸检测耗时（640x480 灰度）----\n");
    {
        cv::CascadeClassifier cas("/root/stereo3d/models/haarcascade_frontalface_default.xml");
        if (cas.empty()) {
            printf("  [!] 级联文件不在，跳过\n\n");
        } else {
            cv::Mat g;
            cv::cvtColor(L, g, cv::COLOR_BGR2GRAY);
            cv::resize(g, g, cv::Size(640, 480));
            std::vector<double> ts;
            std::vector<cv::Rect> faces;
            for (int i = 0; i < 20; ++i) {
                auto t0 = Clock::now();
                cas.detectMultiScale(g, faces, 1.1, 3, 0, cv::Size(60, 60));
                ts.push_back(msSince(t0));
            }
            Stat s = summarize(ts);
            printf("  mean %.2f ms  p95 %.2f ms  检出 %zu 张脸\n\n",
                   s.mean, s.p95, faces.size());
        }
    }

    // ---------------------------------------------------------------
    // 结论
    // ---------------------------------------------------------------
    printf("==================== 结论 ====================\n");
    printf("以上数字全部来自真实硬件测量，不是估算。\n");
    printf("据此决定：处理分辨率 / 视差范围 / 是否需要并行化。\n");
    return 0;
}
