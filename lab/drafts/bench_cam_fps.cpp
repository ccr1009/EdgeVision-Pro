// 纯采集测速：摄像头真实出帧率是多少（不含推理、不含编码）
#include <opencv2/opencv.hpp>
#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video81";
    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) { printf("[-] 打不开 %s\n", dev); return 1; }

    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);

    printf("驱动报告的 FPS: %.1f\n", cap.get(cv::CAP_PROP_FPS));

    cv::Mat f;
    for (int i = 0; i < 15; ++i) cap >> f;   // AE 预热

    const int N = 100;
    auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) cap >> f;
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t).count();

    printf("连续采集 %d 帧: %.2f ms/帧  =>  %.1f fps （含 MJPG 解码）\n",
           N, ms / N, 1000.0 * N / ms);
    printf("画面尺寸: %dx%d\n", f.cols, f.rows);
    cap.release();
    return 0;
}
