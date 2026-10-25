#include <opencv2/opencv.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video83";
    int req_fps = argc > 2 ? atoi(argv[2]) : 30;

    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        printf("[-] Cannot open %s\n", dev);
        return 1;
    }

    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
    cap.set(cv::CAP_PROP_FPS, req_fps);

    printf("[+] Device: %s | Requested FPS: %d\n", dev, req_fps);
    printf("[+] Driver reported CAP_PROP_FPS: %.2f\n", cap.get(cv::CAP_PROP_FPS));

    cv::Mat f;
    for (int i = 0; i < 15; ++i) {
        cap >> f; // AE warm-up
    }

    const int N = 100;
    auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        cap >> f;
    }
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t).count();

    printf("[+] Collected %d frames: %.2f ms/frame => Actual %.2f FPS (with MJPG decode)\n",
           N, ms / N, 1000.0 * N / ms);
    cap.release();
    return 0;
}
