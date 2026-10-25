// ============================================================================
//  stereo_types.h —— 全项目共享的数据结构（Lead 维护，不许改字段名）
// ============================================================================
#pragma once
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace s3d {

// ---------------------------------------------------------------------------
// 标定结果：由 PC 端 calib_py/calibrate.py 产出 stereo_calib.yml，板端直接读
// 口径：所有内参/映射表都对应「标定时使用的原始分辨率」image_size
// ---------------------------------------------------------------------------
struct StereoCalib {
    bool     valid = false;
    cv::Size image_size{0, 0};          // 标定分辨率，如 1280x720

    cv::Mat  K_L, D_L, K_R, D_R;        // 3x3 / 1x5（或 1x8）畸变系数 + 内参
    cv::Mat  R_L, R_R, P_L, P_R, Q;     // 3x3 矫正旋转 / 3x4 投影 / 4x4 视差-深度矩阵

    // 原图 -> 全分辨率矫正图 的 remap 表（由 cv::initUndistortRectifyMap 生成）
    cv::Mat  mapL1, mapL2, mapR1, mapR2;

    // 矫正后左目（P_L）内参，单位像素
    double   fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;
    // 基线，单位米；由 |P2(0,3)/P2(0,0)| 换算
    double   baseline_m = 0.0;

    double   rms_reproj = 0.0;          // 标定重投影 RMS（像素）
    std::string source;                 // 来源文件路径
};

// ---------------------------------------------------------------------------
// 运行参数：决定处理分辨率与 SGBM 代价
// ---------------------------------------------------------------------------
struct StereoConfig {
    int    proc_w = 640;        // 处理分辨率（矫正后缩到这个尺寸再跑 SGBM）
    int    proc_h = 480;
    int    num_disp = 64;       // 视差搜索范围，必须是 16 的倍数
    int    block_size = 5;      // SGBM 块大小，奇数
    bool   use_wls = false;     // 是否用 WLS 滤波（需要 opencv_contrib/ximgproc）
    int    min_depth_mm = 250;  // 深度有效下限
    int    max_depth_mm = 12000;// 深度有效上限
    double scale = 1.0;         // proc_w / image_size.width，由 prepare() 计算
};

// ---------------------------------------------------------------------------
// 一帧里被检出的人脸（含 3D 定位结果）
// 坐标系：以「矫正后左目相机」为原点
//   X 向右（米），Y 向下（米），Z 向前（米）
// ---------------------------------------------------------------------------
struct Face3D {
    cv::Rect    box_full;               // 全分辨率左图坐标（画图用）
    cv::Rect    box_proc;               // 处理尺度坐标（取深度用）
    int         depth_median_mm = 0;    // 框内有效深度中位数
    cv::Point3f center_m{0, 0, 0};      // 人脸框中心反投影到 3D
    float       distance_m = 0.0f;      // 到相机的直线距离
    float       valid_ratio = 0.0f;     // 框内有效视差像素占比（可信度）
    int         valid_pixels = 0;       // 有效视差像素数
    int         total_pixels = 0;       // 框内总像素数
};

// ---------------------------------------------------------------------------
// 每帧耗时统计（毫秒），HUD 与 /stats 直接用
// ---------------------------------------------------------------------------
struct FrameStats {
    double cap_ms = 0;      // 双摄取一对图
    double rect_ms = 0;     // 矫正 + 缩放 + 灰度
    double sgbm_ms = 0;     // 视差
    double depth_ms = 0;    // 视差 -> 深度
    double face_ms = 0;     // 人脸检测 + 3D 定位
    double draw_ms = 0;     // 画框/拼图
    double encode_ms = 0;   // JPEG 编码
    double fps = 0;         // 端到端
    int    frame_id = 0;
    int    n_faces = 0;
};

// ---------------------------------------------------------------------------
// 命令行配置
// ---------------------------------------------------------------------------
struct AppOptions {
    std::string dev_left  = "/dev/video81";
    std::string dev_right = "/dev/video83";
    int         cap_w = 1280, cap_h = 720, cap_fps = 30;

    std::string calib_yml;      // 标定文件路径，空 = 未标定模式
    double      hfov_deg = 60.0;    // 仅未标定模式：水平视场角
    double      baseline_m = 0.06;  // 仅未标定模式：基线（米）
    std::string cascade_xml = "/root/stereo3d/models/haarcascade_frontalface_default.xml";
    int         port = 8081;
    StereoConfig stereo;
    bool        save_frames = false;   // 存图调试
    std::string save_dir = "/root/stereo3d/captures";
};

}  // namespace s3d
