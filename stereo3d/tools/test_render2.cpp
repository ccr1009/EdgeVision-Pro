// /tmp/test_render2.cpp —— 修正版：draw() 的标签画在框上方，断言区域要跟着走
#include <cstdio>
#include <cmath>
#include <vector>
#include "face_3d.h"

int main() {
    s3d::StereoCalib calib; calib.valid = true;
    calib.image_size = cv::Size(1280, 720);
    calib.fx = 550; calib.fy = 550; calib.cx = 640; calib.cy = 360;
    calib.baseline_m = 0.06; calib.rms_reproj = 0.34; calib.source = "RENDER_TEST";
    s3d::StereoConfig cfg; cfg.proc_w = 640; cfg.proc_h = 480; cfg.scale = 0.5;

    s3d::Face3D f;
    f.box_proc = cv::Rect(240, 140, 160, 180);
    f.box_full = cv::Rect(480, 280, 320, 360);
    f.depth_median_mm = 1830;
    f.center_m = cv::Point3f(0.f, 0.f, 1.83f);
    f.distance_m = 1.83f;
    f.valid_ratio = 0.90f; f.valid_pixels = 16588; f.total_pixels = 18432;

    cv::Mat canvas(720, 1280, CV_8UC3, cv::Scalar(20, 20, 20));
    std::vector<s3d::Face3D> vs{f};
    s3d::Face3DLocator::draw(canvas, vs, cfg, 21.5);

    // ---- 1) 框线：沿 box_full 四条边数绿色 ----
    int green = 0;
    for (int y = f.box_full.y - 2; y < f.box_full.y + f.box_full.height + 2; ++y)
        for (int x = f.box_full.x - 2; x < f.box_full.x + f.box_full.width + 2; ++x)
            if (y >= 0 && y < canvas.rows && x >= 0 && x < canvas.cols) {
                cv::Vec3b p = canvas.at<cv::Vec3b>(y, x);
                if (p[1] > 180 && p[0] < 130 && p[2] < 130) ++green;
            }
    std::printf("[1 框线] 绿色像素 = %d\n", green);

    // ---- 2) 标签：真代码把标签放在框上方 (y_text = box_full.y - 8) ----
    //     色块 = 绿填充，文字 = 纯黑 -> 「绿色块内的黑像素」就是字形
    const int band_y0 = f.box_full.y - 40, band_y1 = f.box_full.y + 2;
    const int band_x0 = f.box_full.x,      band_x1 = f.box_full.x + 320;
    int fill = 0, glyph = 0;
    for (int y = band_y0; y <= band_y1; ++y)
        for (int x = band_x0; x < band_x1; ++x) {
            if (y < 0 || y >= canvas.rows || x < 0 || x >= canvas.cols) continue;
            cv::Vec3b p = canvas.at<cv::Vec3b>(y, x);
            if (p[1] > 150 && p[0] < 140 && p[2] < 140) ++fill;               // 绿底
            else if (p[0] < 60 && p[1] < 60 && p[2] < 60 && y >= band_y0 + 4) ++glyph;  // 黑字
        }
    std::printf("[2 标签] 框上方标签色块绿像素 = %d, 字形黑像素 = %d (都 >0 说明 D=1.83m 真渲染了)\n", fill, glyph);

    // ---- 3) 同一位置附近不该是 0：对照，框正上方 200px 高处应仍是空背景 ----
    int empty = 0;
    for (int y = band_y0 - 120; y < band_y0 - 60; ++y)
        for (int x = band_x0; x < band_x1; ++x)
            if (canvas.at<cv::Vec3b>(y, x) != cv::Vec3b(20, 20, 20)) ++empty;
    std::printf("[3 对照] 标签上方留白区被改动像素 = %d (应为 0)\n", empty);

    // ---- 4) HUD 条 ----
    int hud_px = 0;
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 700; ++x)
            if (canvas.at<cv::Vec3b>(y, x) != cv::Vec3b(20, 20, 20)) ++hud_px;
    std::printf("[4 HUD条] 改动像素 = %d\n", hud_px);

    // ---- 5) 无效深度 -> 橙色警示框 ----
    s3d::Face3D bad; bad.box_proc = cv::Rect(100, 100, 120, 120);
    bad.box_full = cv::Rect(200, 200, 240, 240);
    bad.depth_median_mm = 0; bad.distance_m = 0; bad.valid_pixels = 0; bad.total_pixels = 14400;
    cv::Mat c2(720, 1280, CV_8UC3, cv::Scalar(20, 20, 20));
    std::vector<s3d::Face3D> vs2{bad};
    s3d::Face3DLocator::draw(c2, vs2, cfg, 21.5);
    int orange = 0, green2 = 0;
    for (int y = bad.box_full.y - 2; y < bad.box_full.y + bad.box_full.height + 2; ++y)
        for (int x = bad.box_full.x - 2; x < bad.box_full.x + bad.box_full.width + 2; ++x)
            if (y >= 0 && y < c2.rows && x >= 0 && x < c2.cols) {
                cv::Vec3b p = c2.at<cv::Vec3b>(y, x);
                if (p[2] > 200 && p[1] > 120 && p[1] < 220 && p[0] < 120) ++orange;
                if (p[1] > 180 && p[0] < 130 && p[2] < 130) ++green2;
            }
    std::printf("[5 无深度] 橙像素 = %d, 绿像素 = %d (橙>100 且 绿=0 才对)\n", orange, green2);

    // ---- 6) drawHud 多行面板 ----
    s3d::FrameStats st; st.cap_ms=33.3; st.rect_ms=1.8; st.sgbm_ms=26.4; st.depth_ms=1.1;
    st.face_ms=19.6; st.draw_ms=2.2; st.encode_ms=6.5; st.fps=21.5; st.frame_id=7; st.n_faces=1;
    cv::Mat c3(720, 1280, CV_8UC3, cv::Scalar(20, 20, 20));
    s3d::Face3DLocator::drawHud(c3, st, calib);
    int hud2 = 0;
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 460; ++x)
        if (c3.at<cv::Vec3b>(y, x) != cv::Vec3b(20, 20, 20)) ++hud2;
    std::printf("[6 drawHud] 面板改动像素 = %d\n", hud2);

    // ---- 7) 未标定必须红字 ----
    s3d::StereoCalib uncal; uncal.valid = false;
    cv::Mat c4(720, 1280, CV_8UC3, cv::Scalar(20, 20, 20));
    s3d::Face3DLocator::drawHud(c4, st, uncal);
    int red = 0;
    for (int y = 0; y < 280; ++y) for (int x = 0; x < 950; ++x) {
        cv::Vec3b p = c4.at<cv::Vec3b>(y, x);
        if (p[2] > 150 && p[1] < 120 && p[0] < 120) ++red;
    }
    std::printf("[7 未标定] 红字像素 = %d\n", red);

    // ---- 8) 空 faces 不能崩 ----
    cv::Mat c5(720, 1280, CV_8UC3, cv::Scalar(20, 20, 20));
    std::vector<s3d::Face3D> none;
    s3d::Face3DLocator::draw(c5, none, cfg, 0.0);
    std::printf("[8 空列表] draw 未崩, 仅 HUD 改动像素 = %d\n",
        (int)cv::countNonZero(cv::Mat(c5(cv::Rect(0,0,700,30)) != cv::Scalar(20,20,20)).reshape(1)));

    bool pass = green > 300 && fill > 200 && glyph > 80 && empty == 0 &&
                hud_px > 5000 && orange > 300 && green2 == 0 && hud2 > 8000 && red > 100;
    std::printf("\n==== 渲染结论: %s ====\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}