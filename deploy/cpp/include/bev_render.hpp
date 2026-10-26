#pragma once

// ============================================================================
//  bev_render.hpp —— 3D 线框 + 自车 BEV 雷达的共享渲染层
//
//  背景：这段绘制代码原先在 edgevision_pro_main.cpp 里是四个 static 函数，
//        新增的实时看板（edgevision_dashboard_main.cpp）需要同一套画法。
//        与其再复制一遍，抽成 header-only 的渲染层，两个可执行文件共用。
//
//  设计约束：
//    1. header-only + inline，不引入新的 .cpp / 新的链接依赖；
//    2. 只依赖 OpenCV core/imgproc + bev_projector.hpp，x86 仿真构建也能编；
//    3. 纯绘制，不碰任何业务逻辑，不产生/修改检测结果（AGENTS.md 约定 3）。
//
//  坐标约定（与 BEVProjector 一致）：
//    X = 车辆前向距离(m)，Y = 车辆左向偏移(m)，Z = 向上高度(m)
//    BEV 画布：u 轴向右 = 车辆右侧，v 轴向上 = 车辆前方
// ============================================================================

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "bev_projector.hpp"

namespace ev_render {

// Box3D / Point3D / Point2D 定义在 edgevision 命名空间里（bev_projector.hpp），
// 这里引入进来，调用方只需 using namespace edgevision; 或直接 ev_render::draw_3d_box。
using edgevision::Box3D;
using edgevision::Point2D;
using edgevision::Point3D;

// ---------------------------------------------------------------------------
//  类别配色：按类别固定颜色（不是按检测下标轮换）
//  按 index 轮换会让同一辆车在相邻帧换颜色，画面闪烁得像假数据；按类别配色
//  跨帧稳定，也让 BEV 雷达上的色块能直接读出目标类型。
// ---------------------------------------------------------------------------
inline cv::Scalar class_color(const std::string& cls) {
    if (cls == "car")        return cv::Scalar(255, 190, 40);   // 青蓝
    if (cls == "truck")      return cv::Scalar(40, 165, 255);   // 橙
    if (cls == "bus")        return cv::Scalar(90, 235, 120);   // 亮绿
    if (cls == "person")     return cv::Scalar(0, 235, 255);    // 琥珀黄
    if (cls == "motorcycle") return cv::Scalar(240, 90, 235);   // 品红
    if (cls == "bicycle")    return cv::Scalar(210, 210, 90);   // 青
    return cv::Scalar(220, 220, 220);
}

// ---------------------------------------------------------------------------
//  3D 边界框：8 个角点投影到图像后连 12 条棱
//    底面前 4 点 (0..3) 加粗 —— 视觉上锚在地面，最能表达"贴地"
//    顶面前 4 点 (4..7) 细线
//    4 条竖棱
//    距离标签贴在顶面前沿上方（带深色底衬，避免压到亮天空看不清）
// ---------------------------------------------------------------------------
inline void draw_3d_box(cv::Mat& img, const Box3D& box, const cv::Scalar& color,
                        int thickness = 2) {
    if (box.corners_2d.size() != 8) return;
    const auto& c = box.corners_2d;

    auto P = [&](int i) { return cv::Point((int)std::lround(c[i].u), (int)std::lround(c[i].v)); };

    // 底面（贴地）——加粗
    for (int i = 0; i < 4; ++i) {
        cv::line(img, P(i), P((i + 1) % 4), color, thickness + 1, cv::LINE_AA);
    }
    // 顶面
    for (int i = 0; i < 4; ++i) {
        cv::line(img, P(i + 4), P(((i + 1) % 4) + 4), color, 1, cv::LINE_AA);
    }
    // 竖棱
    for (int i = 0; i < 4; ++i) {
        cv::line(img, P(i), P(i + 4), color, thickness, cv::LINE_AA);
    }

    // 距离标签
    char dist_str[32];
    snprintf(dist_str, sizeof(dist_str), "%.1fm", box.distance_m);
    int baseline = 0;
    cv::Size tsz = cv::getTextSize(dist_str, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
    int tx = P(4).x;
    int ty = std::max(tsz.height + 8, P(4).y - 6);
    cv::Rect bg(tx - 3, ty - tsz.height - 4, tsz.width + 6, tsz.height + 6);
    bg &= cv::Rect(0, 0, img.cols, img.rows);   // 贴边时自动裁剪，避免 Rect 越界抛异常
    if (bg.width > 0 && bg.height > 0) {
        cv::rectangle(img, bg, cv::Scalar(18, 22, 26), -1);
        cv::rectangle(img, bg, color, 1);
    }
    cv::putText(img, dist_str, cv::Point(tx, ty), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                cv::Scalar(245, 245, 245), 1, cv::LINE_AA);
}

// ---------------------------------------------------------------------------
//  2D 兜底框：目标贴地投影失败时（例如几乎贴到镜头、或高到地平线以上）
//  不伪造 3D，只如实画检测框 + 置信度。
// ---------------------------------------------------------------------------
inline void draw_2d_fallback(cv::Mat& img, float x1, float y1, float x2, float y2,
                             const std::string& cls, float score, const cv::Scalar& color) {
    cv::rectangle(img, cv::Point((int)x1, (int)y1), cv::Point((int)x2, (int)y2), color, 2);
    char label[96];
    snprintf(label, sizeof(label), "%s %.0f%%", cls.c_str(), score * 100.0f);
    int baseline = 0;
    cv::Size tsz = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
    int ty = (int)y1 - 6;
    if (ty - tsz.height < 0) ty = (int)y1 + tsz.height + 8;
    cv::Rect bg((int)x1, ty - tsz.height - 4, tsz.width + 8, tsz.height + 8);
    bg &= cv::Rect(0, 0, img.cols, img.rows);
    if (bg.width > 0 && bg.height > 0) {
        cv::rectangle(img, bg, cv::Scalar(18, 22, 26), -1);
        cv::rectangle(img, bg, color, 1);
    }
    cv::putText(img, label, cv::Point((int)x1 + 4, ty), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                color, 1, cv::LINE_AA);
}

// ---------------------------------------------------------------------------
//  自车 BEV 雷达小地图
//    - 同心圆 10/20/30/40/50m（只画 <= range_fwd 的）
//    - 纵向车道线 -10/-5/0/+5/+10m
//    - 相机 FOV 锥
//    - 底部中央自车三角
//    - 每个 3D 目标一个按真实尺寸（长 L / 宽 W）缩放出来的方块
//  所有几何量都从对象自身字段来，没有任何"为了好看"的固定装饰数据。
// ---------------------------------------------------------------------------
inline cv::Mat render_bev_radar(int radar_size, float range_fwd, float range_lat,
                                const std::vector<std::pair<std::string, Box3D>>& objects) {
    cv::Mat canvas(radar_size, radar_size, CV_8UC3, cv::Scalar(20, 24, 28));

    const int ego_u = radar_size / 2;
    const int ego_v = radar_size - 25;
    const float scale_fwd = (ego_v - 20.0f) / range_fwd;
    const float scale_lat = (radar_size / 2.0f - 15.0f) / range_lat;

    auto v2c = [&](float X_v, float Y_v) -> cv::Point {
        int u = (int)std::lround(ego_u - Y_v * scale_lat);
        int v = (int)std::lround(ego_v - X_v * scale_fwd);
        return cv::Point(u, v);
    };

    const cv::Scalar ring_color(45, 55, 65);
    const cv::Scalar text_color(120, 140, 160);

    // 同心距离环
    for (float dist_m : {10.0f, 20.0f, 30.0f, 40.0f, 50.0f}) {
        if (dist_m > range_fwd) continue;
        int radius_px = (int)std::lround(dist_m * scale_fwd);
        cv::circle(canvas, cv::Point(ego_u, ego_v), radius_px, ring_color, 1, cv::LINE_AA);
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%dm", (int)dist_m);
        cv::putText(canvas, lbl, cv::Point(ego_u + 6, ego_v - radius_px + 12),
                    cv::FONT_HERSHEY_SIMPLEX, 0.32, text_color, 1, cv::LINE_AA);
    }

    // 纵向参考线
    for (float lat_m : {-10.0f, -5.0f, 0.0f, 5.0f, 10.0f}) {
        if (std::fabs(lat_m) > range_lat) continue;
        cv::Point p = v2c(0.0f, lat_m);
        if (p.x < 10 || p.x >= radar_size - 10) continue;
        cv::Scalar col = (lat_m == 0.0f) ? cv::Scalar(50, 65, 80) : cv::Scalar(35, 42, 50);
        cv::line(canvas, cv::Point(p.x, 20), cv::Point(p.x, ego_v), col,
                 (lat_m == 0.0f) ? 2 : 1);
    }

    // 相机 FOV 锥（半角 32.5°，与 65° HFOV 口径一致）
    {
        float fov_rad = 32.5f * 3.14159265f / 180.0f;
        float max_y = range_fwd * std::tan(fov_rad);
        cv::line(canvas, cv::Point(ego_u, ego_v), v2c(range_fwd, max_y),
                 cv::Scalar(40, 70, 90), 1, cv::LINE_AA);
        cv::line(canvas, cv::Point(ego_u, ego_v), v2c(range_fwd, -max_y),
                 cv::Scalar(40, 70, 90), 1, cv::LINE_AA);
    }

    // 自车
    std::vector<cv::Point> ego_pts = {
        {ego_u, ego_v - 12}, {ego_u - 7, ego_v + 6},
        {ego_u, ego_v + 2},  {ego_u + 7, ego_v + 6}};
    cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{ego_pts}, cv::Scalar(0, 220, 255));

    // 标题
    cv::putText(canvas, "BEV RADAR", cv::Point(12, 18), cv::FONT_HERSHEY_SIMPLEX, 0.44,
                cv::Scalar(0, 255, 200), 1, cv::LINE_AA);

    // 目标方块
    int idx = 0;
    for (const auto& item : objects) {
        const std::string& cls = item.first;
        const Box3D& b = item.second;
        if (b.longitudinal_m < 0.5f || b.longitudinal_m > range_fwd) continue;

        cv::Point c = v2c(b.longitudinal_m, b.lateral_m);
        if (c.x < 0 || c.x >= radar_size || c.y < 0 || c.y >= radar_size) continue;

        // 按真实车宽/车长缩放（最小 2/3 像素，保证远距离目标仍然可见）
        int half_w = std::max(2, (int)std::lround((b.dims.y * 0.5f) * scale_lat));
        int half_l = std::max(3, (int)std::lround((b.dims.x * 0.5f) * scale_fwd));

        cv::Scalar col = class_color(cls);
        cv::Rect r(c.x - half_w, c.y - half_l, half_w * 2, half_l * 2);
        r &= cv::Rect(0, 0, radar_size, radar_size);
        if (r.width <= 0 || r.height <= 0) continue;

        cv::rectangle(canvas, r, col, -1);
        cv::rectangle(canvas, r, cv::Scalar(250, 250, 250), 1);

        char tag[32];
        snprintf(tag, sizeof(tag), "#%d %.0fm", ++idx, b.distance_m);
        cv::Point tp(c.x + half_w + 3, c.y + 4);
        if (tp.x + 44 > radar_size) tp.x = c.x - half_w - 46;
        if (tp.x < 2) continue;
        cv::putText(canvas, tag, tp, cv::FONT_HERSHEY_SIMPLEX, 0.36,
                    cv::Scalar(235, 235, 235), 1, cv::LINE_AA);
    }
    return canvas;
}

// 贴图辅助：把 radar 贴到 src 的右上角（越界时自动跳过，不抛异常）
inline bool paste_radar(cv::Mat& dst, const cv::Mat& radar, int margin_x = 16, int margin_y = 16,
                        int top_reserved = 0) {
    int rx = dst.cols - radar.cols - margin_x;
    int ry = margin_y + top_reserved;
    if (rx < 0 || ry + radar.rows > dst.rows) return false;
    radar.copyTo(dst(cv::Rect(rx, ry, radar.cols, radar.rows)));
    cv::rectangle(dst, cv::Rect(rx - 1, ry - 1, radar.cols + 2, radar.rows + 2),
                  cv::Scalar(0, 200, 255), 1);
    return true;
}

}  // namespace ev_render
