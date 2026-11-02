#include <opencv2/opencv.hpp>
#include <iostream>

int main() {
    cv::VideoCapture cap(" /dev/video81\, cv::CAP_V4L2);
 if (!cap.isOpened()) {
 std::cerr << \[-] 无法打开 /dev/video81\n\;
 return 1;
 }
 cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
 cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
 cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
 cv::Mat frame;
 cap >> frame;
 std::cout << \[+] OpenCV V4L2 成功捕获一帧! 尺寸: \ << frame.cols << \x\ << frame.rows << std::endl;
 cap.release();
 return 0;
}
