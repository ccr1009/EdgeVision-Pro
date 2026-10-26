// ============================================================================
//  edgevision_pro_main.cpp —— EdgeVision Pro 真实链路 Demo
//
//  2026-10-24 重写：彻底移除假 zero_copy_pipeline.hpp 仿真桩（fd 硬编码
//  1000+i，"Memory Bandwidth Saved" 是编造数字），换成真实链路：
//    1. MPP 硬件 JPEG 解码 (MppJpegDecoder, 实测 p50 1.03ms, 7.6x vs 软解)
//    2. NV12 -> RGB888 (CPU 定点 BT.601 full-range, ~1ms)
//    3. letterbox 640x640 (OpenCV, 灰边 114, 与 eval_dump.cpp 口径一致)
//    4. RKNN NPU 推理 (YOLOv8n DFL-Free INT8)
//    5. 检测框还原原图坐标 -> BEV 地面投影 (BEVProjector 纯几何)
//
//  用法: edgevision_pro <jpg路径>
//    无参数时打印用法并退出 1, 不跑任何合成/仿真数据。
//  模型: 环境变量 EV_MODEL 可覆盖, 默认 data/models/yolov8n_dflfree_i8.rknn
// ============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <algorithm>
#include <cmath>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include "mpp_jpeg_decoder.h"
#include "bev_projector.hpp"
#include "rknn_engine_dfl_free.h"

using namespace edgevision;

static const cv::Scalar PALETTE[] = {
    cv::Scalar(255, 160, 40),  // Vivid Cyan-Blue
    cv::Scalar(0, 220, 255),    // Amber Yellow
    cv::Scalar(80, 240, 100),   // Bright Green
    cv::Scalar(255, 60, 240),   // Magenta Pink
    cv::Scalar(240, 240, 40),   // Cyan
    cv::Scalar(0, 180, 255)     // Orange
};

static void draw_3d_box(cv::Mat& img, const Box3D& box, const cv::Scalar& color, int thickness = 2) {
    if (box.corners_2d.size() != 8) return;
    const auto& c = box.corners_2d;

    // 1. Bottom face (0, 1, 2, 3)
    for (int i = 0; i < 4; ++i) {
        cv::line(img, cv::Point((int)c[i].u, (int)c[i].v),
                      cv::Point((int)c[(i + 1) % 4].u, (int)c[(i + 1) % 4].v), color, thickness);
    }
    // 2. Top face (4, 5, 6, 7)
    for (int i = 0; i < 4; ++i) {
        cv::line(img, cv::Point((int)c[i + 4].u, (int)c[i + 4].v),
                      cv::Point((int)c[((i + 1) % 4) + 4].u, (int)c[((i + 1) % 4) + 4].v), color, thickness);
    }
    // 3. Vertical pillars
    for (int i = 0; i < 4; ++i) {
        cv::line(img, cv::Point((int)c[i].u, (int)c[i].v),
                      cv::Point((int)c[i + 4].u, (int)c[i + 4].v), color, thickness);
    }
    // 4. Front face cross (0-5, 1-4)
    cv::Scalar front_color(255, 128, 0); // Orange/Cyan accent
    cv::line(img, cv::Point((int)c[0].u, (int)c[0].v),
                  cv::Point((int)c[5].u, (int)c[5].v), front_color, 1);
    cv::line(img, cv::Point((int)c[1].u, (int)c[1].v),
                  cv::Point((int)c[4].u, (int)c[4].v), front_color, 1);

    // 5. Distance label above box with dark background
    char dist_str[32];
    snprintf(dist_str, sizeof(dist_str), "%.1fm", box.distance_m);
    int tx = (int)c[4].u;
    int ty = std::max(18, (int)c[4].v - 6);
    int baseline = 0;
    cv::Size tsz = cv::getTextSize(dist_str, cv::FONT_HERSHEY_SIMPLEX, 0.48, 1, &baseline);
    cv::rectangle(img, cv::Rect(tx - 2, ty - tsz.height - 2, tsz.width + 4, tsz.height + 4),
                  cv::Scalar(18, 22, 26), -1);
    cv::rectangle(img, cv::Rect(tx - 2, ty - tsz.height - 2, tsz.width + 4, tsz.height + 4),
                  color, 1);
    cv::putText(img, dist_str, cv::Point(tx, ty),
                cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
}

static cv::Mat render_bev_radar(int radar_size, float range_fwd, float range_lat,
                                const std::vector<std::pair<std::string, Box3D>>& objects) {
    cv::Mat canvas(radar_size, radar_size, CV_8UC3, cv::Scalar(20, 24, 28));
    int ego_u = radar_size / 2;
    int ego_v = radar_size - 25;
    float scale_fwd = (ego_v - 20) / range_fwd;
    float scale_lat = (radar_size / 2.0f - 15) / range_lat;

    auto v2c = [&](float X_v, float Y_v) -> cv::Point {
        int u = (int)std::round(ego_u - Y_v * scale_lat);
        int v = (int)std::round(ego_v - X_v * scale_fwd);
        return cv::Point(u, v);
    };

    // Concentric circles
    cv::Scalar ring_color(45, 55, 65);
    cv::Scalar text_color(120, 140, 160);
    for (float dist_m : {10.0f, 20.0f, 30.0f, 40.0f, 50.0f}) {
        if (dist_m <= range_fwd) {
            int radius_px = (int)std::round(dist_m * scale_fwd);
            cv::circle(canvas, cv::Point(ego_u, ego_v), radius_px, ring_color, 1, cv::LINE_AA);
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%dm", (int)dist_m);
            cv::putText(canvas, lbl, cv::Point(ego_u + 6, ego_v - radius_px + 12),
                        cv::FONT_HERSHEY_SIMPLEX, 0.32, text_color, 1, cv::LINE_AA);
        }
    }

    // Lateral grid lines
    for (float lat_m : {-10.0f, -5.0f, 0.0f, 5.0f, 10.0f}) {
        cv::Point p = v2c(0.0f, lat_m);
        if (p.x >= 10 && p.x < radar_size - 10) {
            cv::Scalar col = (lat_m == 0.0f) ? cv::Scalar(50, 65, 80) : cv::Scalar(35, 42, 50);
            int th = (lat_m == 0.0f) ? 2 : 1;
            cv::line(canvas, cv::Point(p.x, 20), cv::Point(p.x, ego_v), col, th);
        }
    }

    // Camera FOV cone
    float fov_rad = 32.5f * 3.14159265f / 180.0f;
    float max_y = range_fwd * std::tan(fov_rad);
    cv::Point p_left = v2c(range_fwd, max_y);
    cv::Point p_right = v2c(range_fwd, -max_y);
    cv::line(canvas, cv::Point(ego_u, ego_v), p_left, cv::Scalar(40, 70, 90), 1, cv::LINE_AA);
    cv::line(canvas, cv::Point(ego_u, ego_v), p_right, cv::Scalar(40, 70, 90), 1, cv::LINE_AA);

    // Ego vehicle icon
    std::vector<cv::Point> ego_pts = {
        {ego_u, ego_v - 12},
        {ego_u - 7, ego_v + 6},
        {ego_u, ego_v + 2},
        {ego_u + 7, ego_v + 6}
    };
    cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{ego_pts}, cv::Scalar(0, 220, 255));

    // Title
    cv::putText(canvas, "BEV RADAR (ISO Ego)", cv::Point(12, 18),
                cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(0, 255, 200), 1, cv::LINE_AA);

    // Dynamic obstacle blips
    int idx = 0;
    for (const auto& item : objects) {
        const std::string& cls = item.first;
        const Box3D& b = item.second;
        if (b.longitudinal_m < 0.5f || b.longitudinal_m > range_fwd) continue;
        cv::Point c = v2c(b.longitudinal_m, b.lateral_m);
        if (c.x < 0 || c.x >= radar_size || c.y < 0 || c.y >= radar_size) continue;

        int half_w = std::max(2, (int)std::round((b.dims.y / 2.0f) * scale_lat));
        int half_l = std::max(3, (int)std::round((b.dims.x / 2.0f) * scale_fwd));

        cv::Scalar fill_col(255, 160, 40); // Cyan-blue BGR
        cv::Scalar edge_col(255, 220, 100);
        if (cls == "person") {
            fill_col = cv::Scalar(40, 220, 255);
            edge_col = cv::Scalar(100, 255, 255);
        }

        cv::rectangle(canvas, cv::Rect(c.x - half_w, c.y - half_l, half_w * 2, half_l * 2), fill_col, -1);
        cv::rectangle(canvas, cv::Rect(c.x - half_w, c.y - half_l, half_w * 2, half_l * 2), edge_col, 1);

        char tag[32];
        snprintf(tag, sizeof(tag), "#%d:%.1fm", ++idx, b.distance_m);
        cv::putText(canvas, tag, cv::Point(c.x + half_w + 3, c.y + 4),
                    cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    }
    return canvas;
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    if (sz <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return f.good();
}

static bool is_traffic_class(int id) {
    // 0=person, 1=bicycle, 2=car, 3=motorcycle, 5=bus, 7=truck
    return (id == 0 || id == 1 || id == 2 || id == 3 || id == 5 || id == 7);
}

// BEV 尺寸先验用的类别名 (person/car 等有专门先验, 其余走通用先验)
static std::string cls_name(int id) {
    static const char* names[] = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus",
        "train", "truck", "boat",
    };
    if (id >= 0 && id <= 8) return names[id];
    return "cls" + std::string(id < 10 ? "0" : "") + std::to_string(id);
}

int main(int argc, char** argv) {
    std::cout << "==================================================================" << std::endl;
    std::cout << "EdgeVision Pro: MPP HW-decode + YOLOv8n DFL-Free + BEV Ground Projection" << std::endl;
    std::cout << "Platform Target: Rockchip RK3588 (Tri-Core NPU 6.0 TOPS)" << std::endl;
    std::cout << "==================================================================" << std::endl;
    std::cout << "[NOTICE] 真实链路 demo (2026-10-24 重写), 旧版 zero-copy 仿真已删除"
              << " (违反 AGENTS.md 约定 3: 不许用假数据)" << std::endl;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <jpg_path>" << std::endl;
        std::cerr << "  本程序只处理真实图像输入, 不含任何合成/仿真数据。" << std::endl;
        return 1;
    }
    const std::string jpg_path = argv[1];

    // ------------------------------------------------------------------
    // 1. 读入真实 JPEG
    // ------------------------------------------------------------------
    std::vector<uint8_t> jpeg;
    if (!read_file(jpg_path, jpeg)) {
        std::cerr << "[FAIL] cannot read jpg: " << jpg_path << std::endl;
        return 1;
    }
    std::cout << "[+] Loaded JPEG: " << jpg_path << " (" << jpeg.size() << " bytes)" << std::endl;

    // ------------------------------------------------------------------
    // 2. MPP 硬件解码 (真实 DRM buffer, 实测 p50 1.03ms)
    // ------------------------------------------------------------------
    MppJpegDecoder dec;
    if (dec.init(1920, 1080) != 0) {
        std::cerr << "[FAIL] MppJpegDecoder init: " << dec.last_error() << std::endl;
        return 1;
    }

    DecodedFrame frame;
    auto t0 = std::chrono::high_resolution_clock::now();
    int rc = dec.decode(jpeg.data(), jpeg.size(), &frame);
    auto t1 = std::chrono::high_resolution_clock::now();
    double decode_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (rc != 0 || !frame.valid) {
        std::cerr << "[FAIL] MPP decode failed rc=" << rc
                  << " valid=" << frame.valid
                  << " err=" << dec.last_error() << std::endl;
        return 1;
    }
    std::cout << "[+] MPP HW decode: " << frame.width << "x" << frame.height
              << " NV12, hor_stride=" << frame.hor_stride
              << ", decode_ms=" << std::fixed << std::setprecision(3) << decode_ms
              << " (drm_fd=" << dec.get_dma_fd() << ")" << std::endl;

    // ------------------------------------------------------------------
    // 3. NV12 -> RGB888 (CPU 定点 BT.601 full-range) + letterbox 640x640
    //    (run_inference 不做缩放, 必须外部做; 口径与 eval_dump.cpp 一致)
    // ------------------------------------------------------------------
    std::vector<uint8_t> rgb(static_cast<size_t>(frame.width) * frame.height * 3);
    auto t2 = std::chrono::high_resolution_clock::now();
    dec.to_rgb888(frame, rgb.data());
    auto t3 = std::chrono::high_resolution_clock::now();
    double csc_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::cout << "[+] NV12->RGB888 CSC: " << std::fixed << std::setprecision(3) << csc_ms << " ms" << std::endl;

    // cv::Mat 包装 (不拷贝) -> letterbox 到 640x640 灰边 114
    cv::Mat src(frame.height, frame.width, CV_8UC3, rgb.data());
    cv::Mat lb;
    float scale = 0.f;
    int pad_x = 0, pad_y = 0;
    auto t4 = std::chrono::high_resolution_clock::now();
    {
        float s = std::min((float)640 / src.cols, (float)640 / src.rows);
        int nw = (int)(src.cols * s), nh = (int)(src.rows * s);
        cv::Mat resized;
        cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
        lb = cv::Mat(640, 640, CV_8UC3, cv::Scalar(114, 114, 114));
        pad_x = (640 - nw) / 2;
        pad_y = (640 - nh) / 2;
        resized.copyTo(lb(cv::Rect(pad_x, pad_y, nw, nh)));
        scale = s;
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double lb_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();
    std::cout << "[+] Letterbox 640: scale=" << scale
              << " pad=(" << pad_x << "," << pad_y << ")"
              << " " << std::fixed << std::setprecision(3) << lb_ms << " ms" << std::endl;

    // ------------------------------------------------------------------
    // 4. NPU 推理 (真实模型)
    // ------------------------------------------------------------------
    const char* model_env = std::getenv("EV_MODEL");
    std::string model_path = model_env ? model_env : "data/models/yolov8n_dflfree_rk3588_i8.rknn";
    RKNNEngineDflFree npu;
    if (!npu.init(model_path)) {
        std::cerr << "[FAIL] RKNN init: " << model_path
                  << " (设置 EV_MODEL 环境变量指定模型路径)" << std::endl;
        return 1;
    }

    std::vector<Detection> dets;
    auto t6 = std::chrono::high_resolution_clock::now();
    bool ok = npu.run_inference(lb.data, 640, 640, dets);
    auto t7 = std::chrono::high_resolution_clock::now();
    double npu_ms = std::chrono::duration<double, std::milli>(t7 - t6).count();
    if (!ok) {
        std::cerr << "[FAIL] inference failed" << std::endl;
        return 1;
    }
    std::cout << "[+] NPU inference: " << dets.size() << " detections, "
              << std::fixed << std::setprecision(2) << npu_ms << " ms" << std::endl;

    // ------------------------------------------------------------------
    // 5. 检测框坐标还原 -> 3D 边界框与 BEV 投影渲染
    // ------------------------------------------------------------------
    cv::cvtColor(src, src, cv::COLOR_RGB2BGR);
    bool is_1080p = (frame.width >= 1920);
    float hfov = is_1080p ? 72.0f : 65.0f;
    float mount_h = is_1080p ? 1.50f : 1.60f;
    float pitch = is_1080p ? 3.5f : 8.0f;
    float range_fwd = is_1080p ? 60.0f : 50.0f;
    float range_lat = 16.0f;
    int radar_size = is_1080p ? 340 : 320;

    BEVProjector bev(frame.width, frame.height, hfov, mount_h, pitch);

    std::cout << "------------------------------------------------------------------" << std::endl;
    std::cout << "  idx  class          conf    bbox(orig)                  ground X(m)  Y(m)  dist(m)" << std::endl;

    std::vector<std::pair<std::string, Box3D>> bev_objects;
    for (size_t i = 0; i < dets.size(); ++i) {
        const Detection& d = dets[i];
        if (!is_traffic_class(d.class_id)) continue;
        float ox1 = (d.x1 - pad_x) / scale;
        float oy1 = (d.y1 - pad_y) / scale;
        float ox2 = (d.x2 - pad_x) / scale;
        float oy2 = (d.y2 - pad_y) / scale;
        ox1 = std::max(0.0f, std::min(ox1, (float)src.cols - 1));
        ox2 = std::max(0.0f, std::min(ox2, (float)src.cols - 1));
        oy1 = std::max(0.0f, std::min(oy1, (float)src.rows - 1));
        oy2 = std::max(0.0f, std::min(oy2, (float)src.rows - 1));

        Box3D box3d;
        bool ground = bev.estimate_3d_box(ox1, oy1, ox2, oy2, cls_name(d.class_id), box3d);
        cv::Scalar col = PALETTE[i % 6];

        std::cout << "  " << std::setw(3) << i << "  "
                  << std::setw(12) << std::left << cls_name(d.class_id) << std::right
                  << "  " << std::fixed << std::setprecision(3) << d.score
                  << "  (" << std::setw(4) << (int)ox1 << "," << std::setw(4) << (int)oy1
                  << "," << std::setw(4) << (int)ox2 << "," << std::setw(4) << (int)oy2 << ")";

        if (ground) {
            std::cout << "  " << std::fixed << std::setprecision(2)
                      << std::setw(9) << box3d.longitudinal_m
                      << std::setw(8) << box3d.lateral_m
                      << std::setw(8) << box3d.distance_m;
            bev_objects.push_back({cls_name(d.class_id), box3d});

            // 绘制 3D 线框
            draw_3d_box(src, box3d, col, 2);
        } else {
            std::cout << "         (no ground contact)";
            // 降级绘制 2D 框
            cv::rectangle(src, cv::Point((int)ox1, (int)oy1), cv::Point((int)ox2, (int)oy2), col, 2);
        }

        // 绘制 2D 标签
        char tag[64];
        snprintf(tag, sizeof(tag), "#%zu %s", i + 1, cls_name(d.class_id).c_str());
        cv::putText(src, tag, cv::Point((int)ox1, std::max(18, (int)oy1 - 6)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);

        std::cout << std::endl;
    }
    std::cout << "------------------------------------------------------------------" << std::endl;

    // ------------------------------------------------------------------
    // 6. 渲染 BEV 鸟瞰雷达小地图并贴到右上角
    // ------------------------------------------------------------------
    cv::Mat radar = render_bev_radar(radar_size, range_fwd, range_lat, bev_objects);
    int margin = 16;
    int rx = src.cols - radar.cols - margin;
    int ry = margin;
    if (rx >= 0 && ry + radar.rows <= src.rows) {
        radar.copyTo(src(cv::Rect(rx, ry, radar.cols, radar.rows)));
        cv::rectangle(src, cv::Rect(rx - 1, ry - 1, radar.cols + 2, radar.rows + 2), cv::Scalar(0, 200, 255), 1);
    }

    // ------------------------------------------------------------------
    // 7. 顶部 HUD 遥测状态条 (真实链路数据)
    // ------------------------------------------------------------------
    double total_ms = decode_ms + csc_ms + lb_ms + npu_ms;
    cv::rectangle(src, cv::Rect(10, 8, src.cols - 20, 34), cv::Scalar(18, 22, 26), -1);
    cv::rectangle(src, cv::Rect(10, 8, src.cols - 20, 34), cv::Scalar(45, 55, 65), 1);
    char hud[256];
    snprintf(hud, sizeof(hud),
             "EdgeVision-Pro | RK3588 Tri-Core [RKNN_NPU_CORE_0] | NPU: %.1fms | MPP HW Dec: %.1fms | DFL-Free INT8 | 3D Targets: %zu",
             npu_ms, decode_ms, bev_objects.size());
    cv::putText(src, hud, cv::Point(20, 31),
                cv::FONT_HERSHEY_SIMPLEX, 0.50, cv::Scalar(0, 255, 200), 1, cv::LINE_AA);

    // ------------------------------------------------------------------
    // 8. 导出图像
    // ------------------------------------------------------------------
    std::string out_path = (argc >= 3) ? argv[2] : "docs/images/bev_demo_snapshot.png";
    bool written = cv::imwrite(out_path, src);
    if (written) {
        std::cout << "[+] Saved flagship 3D BEV snapshot to: " << out_path << std::endl;
    } else {
        std::cerr << "[-] Failed to save image to: " << out_path << std::endl;
    }

    std::cout << "[DONE] " << dets.size() << " dets, " << bev_objects.size()
              << " with valid BEV projection. pipeline: decode "
              << std::fixed << std::setprecision(2) << decode_ms
              << " + csc " << csc_ms << " + letterbox " << lb_ms
              << " + npu " << npu_ms
              << " = " << total_ms << " ms" << std::endl;

    return 0;
}
