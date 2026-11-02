// /tmp/test_http.cpp —— http_server.cpp 运行时路由自测（只在测试程序里起服务，端口 18099，绝不占 8080/8081）
// 帧内容来自真实计算链路：合成标定 + 已知真值深度图 -> 真跑 locate() -> draw() -> imencode()
// 目的只是把 5 个路由打通，不是给项目提供假数据
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <unistd.h>
#include <ctime>
#include "face_3d.h"
#include "http_server.h"

static double now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static s3d::StereoCalib g_calib;
static s3d::StereoConfig g_cfg;
static s3d::Face3D g_face;
static cv::Mat g_disp16;

static void build_scene() {
    g_calib.valid = true;
    g_calib.image_size = cv::Size(1280, 720);
    g_calib.fx = 550.0; g_calib.fy = 550.0; g_calib.cx = 640.0; g_calib.cy = 360.0;
    g_calib.baseline_m = 0.06; g_calib.rms_reproj = 0.34;
    g_calib.source = "SMOKE_TEST";
    g_cfg.proc_w = 640; g_cfg.proc_h = 480; g_cfg.scale = 0.5;
    const double fx_proc = g_calib.fx * g_cfg.scale, B_mm = 60.0;

    cv::Rect face(240, 140, 160, 180);
    g_disp16 = cv::Mat(g_cfg.proc_h, g_cfg.proc_w, CV_16S, cv::Scalar(0));
    cv::Mat depth(g_cfg.proc_h, g_cfg.proc_w, CV_32F, cv::Scalar(0.f));
    for (int y = 0; y < g_cfg.proc_h; ++y)
        for (int x = 0; x < g_cfg.proc_w; ++x) {
            double z = face.contains(cv::Point(x, y)) ? 1830.0 : 4000.0;   // 脸在 1.83 m
            g_disp16.at<short>(y, x) = (short)std::lround(fx_proc * B_mm / z * 16.0);
            depth.at<float>(y, x) = (float)z;
        }
    s3d::Face3DLocator loc;
    loc.locate(face, g_disp16, depth, g_calib, g_cfg, g_face);
    std::printf("[test] locate: 真值 1830mm -> depth_median=%d mm, dist=%.3f m\n",
                g_face.depth_median_mm, (double)g_face.distance_m);
}

int main(int argc, char** argv) {
    // 模式: nopub = 只起服务不推帧（用来验 /snapshot /depth 的 503）
    //       pub   = 正常推帧（验 /  /stream /snapshot /stats /depth 200）
    const bool nopub = (argc > 1 && std::strcmp(argv[1], "nopub") == 0);
    const int  secs  = (argc > 2) ? std::atoi(argv[2]) : (nopub ? 6 : 14);

    build_scene();
    s3d::HttpPreview http(18099, "stereo3d face 3D smoke test");
    if (!http.start()) { std::printf("[test] start() 失败\n"); return 1; }
    std::printf("[test] READY port=18099 mode=%s running=%d\n",
                nopub ? "nopub" : "pub", (int)http.running());
    std::fflush(stdout);

    const double t_end = now_ms() + secs * 1000.0;
    int i = 0;
    while (now_ms() < t_end) {
        if (!nopub) {
            double t0 = now_ms();
            s3d::FrameStats st;
            std::vector<s3d::Face3D> faces{g_face};
            cv::Mat canvas(720, 1280, CV_8UC3, cv::Scalar(28, 24, 20));
            s3d::Face3DLocator::draw(canvas, faces, g_cfg, 20.0);
            s3d::Face3DLocator::drawHud(canvas, st, g_calib);
            double t1 = now_ms();
            std::vector<uint8_t> jpg;
            cv::imencode(".jpg", canvas, jpg, {cv::IMWRITE_JPEG_QUALITY, 80});
            double t2 = now_ms();

            st.cap_ms = 33.3; st.rect_ms = 1.8; st.sgbm_ms = 26.4; st.depth_ms = 1.1;
            st.face_ms = 19.6; st.draw_ms = t1 - t0; st.encode_ms = t2 - t1;
            st.fps = 20.0; st.frame_id = i; st.n_faces = 1;

            char js[640];
            std::snprintf(js, sizeof(js),
                "{\"frame_id\":%d,\"fps\":%.2f,\"n_faces\":%d,"
                "\"depth_median_mm\":%d,\"distance_m\":%.3f,"
                "\"valid_ratio\":%.3f,\"valid_pixels\":%d,\"total_pixels\":%d,"
                "\"cap_ms\":%.2f,\"rect_ms\":%.2f,\"sgbm_ms\":%.2f,\"depth_ms\":%.2f,"
                "\"face_ms\":%.2f,\"draw_ms\":%.2f,\"encode_ms\":%.2f,"
                "\"calib_valid\":true,\"depth_ready\":true}",
                st.frame_id, st.fps, st.n_faces, g_face.depth_median_mm,
                (double)g_face.distance_m, (double)g_face.valid_ratio,
                g_face.valid_pixels, g_face.total_pixels,
                st.cap_ms, st.rect_ms, st.sgbm_ms, st.depth_ms,
                st.face_ms, st.draw_ms, st.encode_ms);
            http.publish(jpg, std::string(js));

            if (i % 2 == 0) {   // 视差伪彩图（真 imencode）
                cv::Mat d8;
                g_disp16.convertTo(d8, CV_8U, 255.0 / (g_cfg.num_disp * 16.0));
                cv::applyColorMap(d8, d8, cv::COLORMAP_JET);
                std::vector<uint8_t> dj;
                cv::imencode(".jpg", d8, dj, {cv::IMWRITE_JPEG_QUALITY, 80});
                http.publishDepth(dj);
            }
            if (i % 20 == 0) { std::printf("[test] pub frame %d jpeg=%zu\n", i, jpg.size()); std::fflush(stdout); }
            ++i;
        }
        usleep(50000);
    }
    double ts = now_ms();
    http.stop();
    std::printf("[test] stop() 用时 %.1f ms, running=%d, 共推 %d 帧\n",
                now_ms() - ts, (int)http.running(), i);
    std::printf("[test] EXIT_OK\n");
    return 0;
}