// ============================================================================
//  stereo_pipeline.h —— 双目矫正 + SGBM 视差 + 深度 + 点云
//  实现文件：src/stereo_pipeline.cpp  （owner: stereo-core）
//  ★ 头文件是契约，字段/签名不许改；要加功能就加新方法。
// ============================================================================
#pragma once
#include "stereo_types.h"
#include <string>

namespace s3d {

class StereoPipeline {
public:
    StereoPipeline() = default;

    // -----------------------------------------------------------------
    // 1) 标定装载
    // -----------------------------------------------------------------
    // 读 PC 端 calibrate.py 生成的 stereo_calib.yml（键名见 docs/CALIB_FORMAT.md）
    // 成功后 calib().valid == true，并根据 image_size 建好 remap 表
    bool loadCalib(const std::string& yml_path, std::string* err = nullptr);

    // 未标定兜底：用「理想平行双目」假设（fx 由水平 FOV 推、畸变=0、基线给定）
    // 语义明确为「仅供相对深度参考」，calib().source 会写成 "SYNTHETIC/UNCAILBRATED"
    bool initIdeal(const cv::Size& image_size, double hfov_deg, double baseline_m,
                   std::string* err = nullptr);

    // 无论走哪条路，最后都要调用；负责按 image_size + config 建 remap 表
    bool prepare(const cv::Size& image_size, std::string* err = nullptr);

    // -----------------------------------------------------------------
    // 2) 每帧处理
    // -----------------------------------------------------------------
    // 原图(1280x720 BGR) -> 矫正后【全分辨率】灰度图；同时缓存内部的处理尺度版本
    void rectify(const cv::Mat& leftBGR, const cv::Mat& rightBGR,
                 cv::Mat& rectLfull, cv::Mat& rectRfull);

    // 由上一次 rectify() 得到的处理尺度灰度图跑 SGBM
    // 输出 disp16：CV_16S，定点视差 = 真实像素视差 * 16，无效像素 = 最小视差值*16
    void computeDisparity(const cv::Mat& rectLproc, const cv::Mat& rectRproc, cv::Mat& disp16);

    // 处理尺度灰度图的访问（供人脸检测用，避免重复缩放）
    const cv::Mat& procLeftGray() const { return procL_; }
    const cv::Mat& procRightGray() const { return procR_; }
    cv::Size procSize() const { return cv::Size(cfg_.proc_w, cfg_.proc_h); }

    // 视差 -> 深度（毫米），尺寸与 disp16 一致；无效处置 0
    // 公式：Z = fx_proc * baseline_mm / disp_px，fx_proc = fx_full * scale
    void depthFromDisparity(const cv::Mat& disp16, cv::Mat& depth_mm);

    // 处理尺度像素 + 视差 -> 相机系 3D 点（米）
    bool backprojectProc(const cv::Point2f& pt_proc, float disp_px, cv::Point3f& out_m) const;

    // -----------------------------------------------------------------
    // 3) 可视化
    // -----------------------------------------------------------------
    static cv::Mat colorizeDisparity(const cv::Mat& disp16, int num_disp);
    static cv::Mat colorizeDepth(const cv::Mat& depth_mm, int max_depth_mm);

    // 点云：返回 Nx3 CV_32F（相机系，米），stride 抽稀；无效点跳过
    cv::Mat pointCloud(const cv::Mat& depth_mm, int stride = 4) const;

    // -----------------------------------------------------------------
    // 4) 访问器
    // -----------------------------------------------------------------
    const StereoCalib&  calib()  const { return calib_; }
    StereoCalib&        calib()        { return calib_; }
    const StereoConfig& config() const { return cfg_; }
    StereoConfig&       config()       { return cfg_; }

private:
    void rebuildMaps();

    StereoCalib  calib_;
    StereoConfig cfg_;
    cv::Mat      procL_, procR_;        // 处理尺度矫正灰度图
    cv::Size     prepared_size_{0, 0};
};

}  // namespace s3d
