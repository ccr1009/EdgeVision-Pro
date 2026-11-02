// bench_cam_ts.cpp —— 用 OpenCV 逐帧打点，看帧间隔分布
// 目的：区分「摄像头实际帧率」和「平均耗时被算歪」
#include <opencv2/opencv.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video83";
    double req_fps  = argc > 2 ? atof(argv[2]) : 30.0;
    int    N        = argc > 3 ? atoi(argv[3]) : 120;

    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) { printf("[-] 打不开 %s\n", dev); return 1; }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
    cap.set(cv::CAP_PROP_FPS, req_fps);

    printf("请求 %.1f fps → 回读 %.2f fps\n", req_fps, cap.get(cv::CAP_PROP_FPS));

    // 预热：丢弃 3 秒的帧（按请求帧率算帧数），确保 AE 收敛 + 队列排空
    int warm = std::max(15, (int)(req_fps * 3));
    cv::Mat f;
    for (int i = 0; i < warm; ++i) cap >> f;

    std::vector<double> iv;
    iv.reserve(N);
    auto prev = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        cap >> f;
        auto now = std::chrono::steady_clock::now();
        iv.push_back(std::chrono::duration<double, std::milli>(now - prev).count());
        prev = now;
    }
    cap.release();

    // 第一个间隔含上一轮尾巴，丢掉
    iv.erase(iv.begin());
    std::vector<double> s = iv;
    std::sort(s.begin(), s.end());
    double sum = 0; for (double v : iv) sum += v;
    double mean = sum / iv.size();

    printf("N=%zu  均值 %.2f ms (%.1f fps)  min %.2f  中位 %.2f  p95 %.2f  max %.2f\n",
           iv.size(), mean, 1000.0/mean, s.front(), s[s.size()/2],
           s[(size_t)(s.size()*0.95)], s.back());

    // 间隔直方图（10ms 一档）——看是否有「一快一慢」的抖动
    int hist[21] = {0};
    for (double v : iv) {
        int b = (int)(v / 10.0);
        if (b > 20) b = 20;
        hist[b]++;
    }
    printf("帧间隔分布(ms档: 计数): ");
    for (int i = 0; i <= 20; ++i)
        if (hist[i]) printf("%d-%d:%d  ", i*10, i*10+10, hist[i]);
    printf("\n");
    return 0;
}
