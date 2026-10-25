// ============================================================================
//  face_3d.h —— 人脸检测 + 框内深度中值 -> 3D 坐标
//  实现文件：src/face_3d.cpp  （owner: face3d-view）
//  ★ 头文件是契约，签名不许改。
// ============================================================================
#pragma once
#include "stereo_types.h"
#include "stereo_pipeline.h"
#include <string>
#include <vector>

namespace s3d {

class Face3DLocator {
public:
    // 载入 OpenCV Haar 级联。返回 false 表示文件不可用 -> 上层应切到「无脸模式」
    bool loadCascade(const std::string& xml_path, std::string* err = nullptr);
    bool ready() const { return !cascade_.empty(); }

    // 在【处理尺度】灰度左图上检测；返回的 rect 也是处理尺度坐标
    // min_size / max_size 单位为处理尺度像素
    void detect(const cv::Mat& gray_proc, std::vector<cv::Rect>& faces);

    // 把一个检测框（处理尺度）投影成 Face3D：
    //   1. 框内缩 10%（避开背景），统计有效视差/深度
    //   2. 取「有效深度中位数」作为该脸深度（比均值抗离群）
    //   3. 用框中心像素 + 中值视差反投影成 3D 点
    //   4. 同时算出全分辨率坐标 box_full（乘以 1/scale）
    void locate(const cv::Rect& box_proc,
                const cv::Mat& disp16,
                const cv::Mat& depth_mm,
                const StereoCalib& calib,
                const StereoConfig& cfg,
                Face3D& out);

    // 批处理：detect + locate
    void detectAndLocate(const cv::Mat& gray_proc,
                         const cv::Mat& disp16,
                         const cv::Mat& depth_mm,
                         const StereoCalib& calib,
                         const StereoConfig& cfg,
                         std::vector<Face3D>& out);

    // 画框 + 距离标签（在【全分辨率 BGR】上画，自动缩放坐标）
    static void draw(cv::Mat& bgr_full,
                     const std::vector<Face3D>& faces,
                     const StereoConfig& cfg,
                     double fps);

    // 在灰度图上画一个更「好看」的 HUD（可选，给调试视频用）
    static void drawHud(cv::Mat& bgr, const FrameStats& st, const StereoCalib& calib);

private:
    cv::CascadeClassifier cascade_;
};

}  // namespace s3d
