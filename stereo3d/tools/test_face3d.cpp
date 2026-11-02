// /tmp/test_face3d.cpp —— face_3d.cpp 的地面真值自检（合成视差/深度，已知真实 Z）
// 目的：验证 locaate() 的数学与「中位数抗离群」，不是给项目提供假数据
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "face_3d.h"

int main() {
    // ---- 构造标定：全分辨率 fx=550, 基线 60mm, 处理尺度 scale=0.5 ----
    s3d::StereoCalib calib;
    calib.valid = true;
    calib.image_size = cv::Size(1280, 720);
    calib.fx = 550.0; calib.fy = 550.0; calib.cx = 640.0; calib.cy = 360.0;
    calib.baseline_m = 0.06;
    calib.source = "SYNTHETIC_TEST";
    s3d::StereoConfig cfg;
    cfg.proc_w = 640; cfg.proc_h = 480;
    cfg.scale = (double)cfg.proc_w / calib.image_size.width;   // 0.5
    cfg.min_depth_mm = 250; cfg.max_depth_mm = 12000;

    const double fx_proc = calib.fx * cfg.scale;   // 275
    const double B_mm = calib.baseline_m * 1000.0; // 60
    auto disp_for = [&](double z_mm) { return fx_proc * B_mm / z_mm; };  // 真实视差(像素)

    // ---- 合成一帧：背景 4m，脸 2m，脸内掺 20% 离群像素（模拟 SGBM 错配）----
    cv::Mat disp16(cfg.proc_h, cfg.proc_w, CV_16S, cv::Scalar(0));
    cv::Mat depth(cfg.proc_h, cfg.proc_w, CV_32F, cv::Scalar(0.f));
    cv::Rect face(240, 140, 160, 180);   // 处理尺度上的脸框

    cv::randu(disp16, cv::Scalar(0), cv::Scalar(0));  // 先清 0
    // 背景
    for (int y = 0; y < cfg.proc_h; ++y)
        for (int x = 0; x < cfg.proc_w; ++x) {
            double z = 4000.0;
            if (face.contains(cv::Point(x, y))) z = 2000.0;
            double d = disp_for(z);
            disp16.at<short>(y, x) = (short)std::lround(d * 16.0);
            depth.at<float>(y, x) = (float)z;
        }
    // 脸框内掺 20% 离群点：10% 无效(0) + 10% 错到 0.6m（SGBM 在弱纹理区典型失效）
    cv::RNG rng(12345);
    int n_out = 0, n_invalid = 0;
    for (int y = face.y; y < face.y + face.height; ++y)
        for (int x = face.x; x < face.x + face.width; ++x) {
            int r = rng.uniform(0, 100);
            if (r < 10) { disp16.at<short>(y, x) = 0; depth.at<float>(y, x) = 0.f; ++n_invalid; }
            else if (r < 20) {
                double dz = 600.0;   // 0.6 m 的错误深度
                disp16.at<short>(y, x) = (short)std::lround(disp_for(dz) * 16.0);
                depth.at<float>(y, x) = (float)dz;
                ++n_out;
            }
        }
    std::printf("[合成] 脸真值 Z=2000mm, 掺入 离群=%d + 无效=%d 像素 (框内共 %d)\n",
                n_out, n_invalid, face.width * face.height);

    s3d::Face3DLocator loc;
    s3d::Face3D f;
    loc.locate(face, disp16, depth, calib, cfg, f);

    const double err_mm = (double)f.depth_median_mm - 2000.0;
    std::printf("[locate] depth_median=%d mm (真值 2000, 误差 %+.1f mm = %+.3f%%)\n",
                f.depth_median_mm, err_mm, 100.0 * err_mm / 2000.0);
    std::printf("[locate] center_m=(%.4f, %.4f, %.4f) m  distance=%.4f m\n",
                f.center_m.x, f.center_m.y, f.center_m.z, f.distance_m);
    std::printf("[locate] valid=%d / total=%d -> ratio=%.4f\n",
                f.valid_pixels, f.total_pixels, f.valid_ratio);
    std::printf("[locate] box_full=(%d,%d,%d,%d)  期望≈(%d,%d,%d,%d)\n",
                f.box_full.x, f.box_full.y, f.box_full.width, f.box_full.height,
                (int)std::lround(face.x / cfg.scale), (int)std::lround(face.y / cfg.scale),
                (int)std::lround(face.width / cfg.scale), (int)std::lround(face.height / cfg.scale));

    // ---- 对照：同样一批像素，均值 vs 中位数（这是「为什么用中位数」的量化证据）----
    std::vector<float> zs;
    for (int y = face.y + face.height / 10; y < face.y + face.height * 9 / 10; ++y)
        for (int x = face.x + face.width / 10; x < face.x + face.width * 9 / 10; ++x) {
            float z = depth.at<float>(y, x);
            if (z > 0) zs.push_back(z);
        }
    double sum = 0; for (float v : zs) sum += v;
    double mean = zs.empty() ? 0 : sum / zs.size();
    std::vector<float> cp = zs;
    std::nth_element(cp.begin(), cp.begin() + cp.size() / 2, cp.end());
    std::printf("[对照] 同一批 %zu 个有效像素: 均值=%.1f mm (误差%+.0f) | 中位数=%.1f mm (误差%+.0f)\n",
                zs.size(), mean, mean - 2000.0, cp[cp.size() / 2], cp[cp.size() / 2] - 2000.0);

    // ---- 反投影自检：中心像素 (320,230) 在 Z=2m 下的 X,Y ----
    const double Z = f.center_m.z;
    const double expect_x = (face.x + face.width * 0.5 - calib.cx * cfg.scale) * Z / fx_proc;
    const double expect_y = (face.y + face.height * 0.5 - calib.cy * cfg.scale) * Z / fx_proc;
    std::printf("[反投影] 手算 X=%.5f Y=%.5f  vs 代码 X=%.5f Y=%.5f\n",
                expect_x, expect_y, f.center_m.x, f.center_m.y);

    // ---- 边界：没有深度图时不能崩、不能编造 ----
    cv::Mat empty;
    s3d::Face3D e2;
    loc.locate(face, empty, empty, calib, cfg, e2);
    std::printf("[边界] 空深度图 -> depth=%d valid=%d dist=%.2f (应为 0)\n",
                e2.depth_median_mm, e2.valid_pixels, e2.distance_m);

    // ---- Haar 真实加载 + 真实检测（用板上的真级联，喂一张合成灰度图）----
    std::string err;
    bool ok = loc.loadCascade("/root/stereo3d/models/haarcascade_frontalface_default.xml", &err);
    std::printf("[cascade] load=%d err='%s'\n", (int)ok, err.c_str());
    cv::Mat gray_proc(cfg.proc_h, cfg.proc_w, CV_8U, cv::Scalar(80));
    cv::rectangle(gray_proc, face, cv::Scalar(200), -1);
    std::vector<cv::Rect> faces;
    int64 t0 = cv::getTickCount();
    loc.detect(gray_proc, faces);
    double ms = (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();
    std::printf("[detect] 640x480 用时 %.1f ms, 检出 %zu 个 (合成方块不算人脸，预期 0)\n",
                ms, faces.size());

    // ---- draw / drawHud 不崩 ----
    cv::Mat canvas(720, 1280, CV_8UC3, cv::Scalar(30, 30, 30));
    std::vector<s3d::Face3D> vs{f};
    s3d::Face3DLocator::draw(canvas, vs, cfg, 25.0);
    s3d::FrameStats st; st.cap_ms = 12.3; st.rect_ms = 4.1; st.sgbm_ms = 28.6; st.depth_ms = 1.2;
    st.face_ms = 24.9; st.draw_ms = 3.3; st.encode_ms = 7.1; st.fps = 20.0; st.frame_id = 42; st.n_faces = 1;
    s3d::Face3DLocator::drawHud(canvas, st, calib);
    std::vector<uint8_t> jpg; cv::imencode(".jpg", canvas, jpg);
    std::printf("[draw] HUD+框渲染 OK, JPEG %zu 字节\n", jpg.size());

    bool pass = std::fabs(err_mm) <= 2.0 && f.valid_pixels > 0 &&
                std::fabs(f.box_full.width - 320) <= 2;
    std::printf("\n==== 结论: %s ====\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}