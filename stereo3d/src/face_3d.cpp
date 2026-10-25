// ============================================================================
//  face_3d.cpp —— Haar 人脸检测 + 框内深度中值 -> 3D 坐标
//  目标板：RK3588 / Debian 11 / OpenCV 4.5.1 / g++ 10.2.1 / C++17
//  实现 include/face_3d.h 的契约（头文件只读，字段与签名一字未改）
//
//  ---------------------------------------------------------------------------
//  为什么这条链路要这么写（校招面试素材，讲的是「为什么」不是「是什么」）
//  ---------------------------------------------------------------------------
//  1) 为什么检测前先 equalizeHist（直方图均衡）
//     Haar 特征是「矩形区域内两块灰度的差之和」，本质是**相对**灰度差，
//     所以整体明暗它不是特别敏感；但它对**对比度**极其敏感。
//     暗光/逆光时灰度全挤在很窄的一段（比如 30~70），人脸五官的差值只有几个灰度级，
//     级联的弱分类器阈值直接判不过去 -> 漏检。
//     equalizeHist 用累积分布函数把灰度拉伸到 0~255 填满，等价于把弱对比度放大，
//     召回率明显上升；代价是噪点也一起被放大，所以才需要「有效视差占比」兜可信度。
//
//  2) 为什么框内深度取【中位数】而不是【均值】（本题最核心的一点）
//     人脸框里不只有脸：四个边落在「脸/背景」交界上，混进头发、耳朵、墙、桌子是常态；
//     SGBM 在弱纹理区和遮挡边界还会吐出**成片的错误视差**（离群值）。
//       - 均值对离群值没有任何免疫力：1 个 0.3 m 的错误点能把 2 m 的结果明显拉偏，
//         而且 100 个正确点 + 1 个错点，均值被拽走 —— 均值不是「多数表决」。
//       - 中位数只取「排序后正中间那个」，只要错误像素不到一半，结果完全不受影响。
//         它的崩溃点是 50%，均值的崩溃点是 1/(N+1)。
//     代价：中位数不能像均值那样用「累加」并行化，但可以用 nth_element
//     （快速选择，平均 O(n)）代替 std::sort（O(n log n)）—— 每帧实时跑必须这么选。
//
//  3) 中位数取在「视差空间」还是「深度空间」
//     Z = fx*B/d 单调但**非线性**，两个空间的中位数严格来说不是同一个数。
//     这里在视差空间取中位数 d_med，再算 Z = fx_proc*B/d_med：
//     这样 depth_median_mm 和 center_m.z 是同一个数，不会出现
//     「标签写 1.83 m、点云里却是 1.86 m」的自相矛盾。
//     对近似平面的脸，两种口径差异 <1%，真正的主误差源是 SGBM 离群点。
//
//  4) 为什么框内要内缩 10%
//     人脸框的边界正好是「前景/背景交界」，这里的视差最容易匹配错（遮挡 + 无纹理），
//     无效视差大量集中在边框那一圈。内缩 10% 只统计脸内部，有效占比立刻上一个台阶。
// ============================================================================
#include "face_3d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace s3d {
namespace {

// 人脸框每边内缩比例（见文件头第 4 点）
constexpr double kShrinkRatio = 0.10;

// Haar 检测参数 —— 与 edgevision-rk3588 已验证那套一致（640x480 约 24 ms）
constexpr double kHaarScaleFactor = 1.1;   // 金字塔缩放步长：越小越慢、越容易漏小脸
constexpr int    kHaarMinNeighbors  = 3;   // 邻域阈值：越大误检越少、漏检越多
constexpr int    kHaarMinSize       = 60;  // 最小脸边长（处理尺度像素）

inline int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// 求中位数（会打乱 v 的元素顺序）。v 为空时返回 false。
// 用 nth_element 而不是 sort：只要「第 n/2 小」这一个位置，平均 O(n)。
bool medianInPlace(std::vector<float>& v, float& out) {
    if (v.empty()) return false;
    const long mid = static_cast<long>(v.size() / 2);
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    float m = v[mid];
    if (v.size() % 2 == 0) {
        // 偶数个元素：中位数 = 中间两个的平均值
        const float lower = *std::max_element(v.begin(), v.begin() + mid);
        m = 0.5f * (lower + m);
    }
    out = m;
    return true;
}

// 读深度图一个像素（毫米）。契约只规定「无效置 0」，没说具体 CV 类型，
// 所以这里不假设单一类型 —— 换实现（16S/32S/32F）也不会把这套代码搞崩。
inline float depthAt(const cv::Mat& depth, int x, int y) {
    switch (depth.depth()) {
        case CV_16S: return static_cast<float>(depth.at<short>(y, x));
        case CV_16U: return static_cast<float>(depth.at<unsigned short>(y, x));
        case CV_32S: return static_cast<float>(depth.at<int>(y, x));
        case CV_32F: return depth.at<float>(y, x);
        case CV_8U:  return static_cast<float>(depth.at<unsigned char>(y, x));
        default:     return 0.0f;
    }
}

// 读视差（真实像素）。契约：CV_16S 定点，真值 = raw/16；无效像素 <= 0。
inline float dispAt(const cv::Mat& disp16, int x, int y) {
    if (disp16.depth() == CV_16S) {
        return static_cast<float>(disp16.at<short>(y, x)) / 16.0f;
    }
    if (disp16.depth() == CV_32F) {   // 兜底：万一上层直接给浮点视差
        return disp16.at<float>(y, x);
    }
    return 0.0f;
}

}  // namespace

// ===========================================================================
//  1) 载入 Haar 级联
// ===========================================================================
bool Face3DLocator::loadCascade(const std::string& xml_path, std::string* err) {
    cascade_ = cv::CascadeClassifier();   // 先清空：允许重复调用换模型
    if (xml_path.empty()) {
        if (err) *err = "Haar 级联路径为空";
        return false;
    }
    if (!cascade_.load(xml_path) || cascade_.empty()) {
        if (err) {
            *err = "无法加载 Haar 级联: " + xml_path +
                   "（检查文件是否存在/可读；板上默认 "
                   "/root/stereo3d/models/haarcascade_frontalface_default.xml）";
        }
        cascade_ = cv::CascadeClassifier();
        return false;
    }
    return true;
}

// ===========================================================================
//  2) 处理尺度灰度图上做人脸检测
// ===========================================================================
void Face3DLocator::detect(const cv::Mat& gray_proc, std::vector<cv::Rect>& faces) {
    faces.clear();
    if (cascade_.empty() || gray_proc.empty()) return;

    // 统一成单通道 CV_8U（契约说要灰度图，但这里不信任调用方）
    cv::Mat gray;
    if (gray_proc.channels() == 3) {
        cv::cvtColor(gray_proc, gray, cv::COLOR_BGR2GRAY);
    } else if (gray_proc.channels() == 4) {
        cv::cvtColor(gray_proc, gray, cv::COLOR_BGRA2GRAY);
    } else if (gray_proc.depth() != CV_8U) {
        gray_proc.convertTo(gray, CV_8U);
    } else {
        gray = gray_proc;   // 只读，不复制数据
    }

    // 直方图均衡：把灰度拉满 0~255，放大五官的相对对比度（见文件头第 1 点）
    cv::Mat eq;
    cv::equalizeHist(gray, eq);

    // scaleFactor=1.1 / minNeighbors=3 / flags=0 / minSize=60x60（处理尺度）
    cascade_.detectMultiScale(eq, faces, kHaarScaleFactor, kHaarMinNeighbors, 0,
                              cv::Size(kHaarMinSize, kHaarMinSize));
}

// ===========================================================================
//  3) 一个检测框 -> Face3D（框内深度中值 + 反投影）
// ===========================================================================
void Face3DLocator::locate(const cv::Rect& box_proc,
                           const cv::Mat& disp16,
                           const cv::Mat& depth_mm,
                           const StereoCalib& calib,
                           const StereoConfig& cfg,
                           Face3D& out) {
    out = Face3D{};            // 全部清零：绝不让上一帧的结果残留成「假数据」
    out.box_proc = box_proc;

    // ---- 3.0 处理尺度 -> 全分辨率坐标（画图用）----
    const double s = (cfg.scale > 1e-6) ? cfg.scale : 1.0;
    int fx0 = cvRound(box_proc.x / s);
    int fy0 = cvRound(box_proc.y / s);
    int fw  = cvRound(box_proc.width / s);
    int fh  = cvRound(box_proc.height / s);
    if (calib.image_size.width > 0 && calib.image_size.height > 0) {
        // 有标定分辨率就夹到图内，防止画框越界写内存
        fx0 = clampInt(fx0, 0, calib.image_size.width - 1);
        fy0 = clampInt(fy0, 0, calib.image_size.height - 1);
        fw = clampInt(fw, 1, calib.image_size.width - fx0);
        fh = clampInt(fh, 1, calib.image_size.height - fy0);
    }
    out.box_full = cv::Rect(fx0, fy0, fw, fh);

    // ---- 3.1 没有深度图 / 没标定尺寸时只能给 2D 框 ----
    if (disp16.empty() || depth_mm.empty()) return;

    // ---- 3.2 框内缩 10%（避开前景/背景交界）----
    cv::Rect inner = box_proc;
    const int dx = cvRound(box_proc.width * kShrinkRatio);
    const int dy = cvRound(box_proc.height * kShrinkRatio);
    inner.x      += dx;
    inner.y      += dy;
    inner.width  -= 2 * dx;
    inner.height -= 2 * dy;
    if (inner.width < 1 || inner.height < 1) {
        inner = box_proc;      // 框本来就很小（<10 px）就退回原框，否则一个像素都采不到
    }

    // 与深度/视差图求交，确保落在图内（两张图尺寸由契约保证一致，这里仍各自夹一遍）
    const cv::Rect img_rect(0, 0, std::min(disp16.cols, depth_mm.cols),
                            std::min(disp16.rows, depth_mm.rows));
    inner &= img_rect;
    if (inner.width <= 0 || inner.height <= 0) return;

    // ---- 3.3 收集有效像素的视差与深度 ----
    // 有效 = 深度 > 0（SGBM 无效视差在 depth_mm 里已经被置 0）且视差 > 0，
    //        并且落在 min_depth_mm ~ max_depth_mm 之间（stereo-core 已裁过一次，
    //        这里再兜一层：万一上层传了没裁的深度图也不会把 12 m 外的噪声算进来）
    const size_t cap = static_cast<size_t>(inner.width) * static_cast<size_t>(inner.height);
    std::vector<float> disps;
    std::vector<float> depths;
    disps.reserve(cap);
    depths.reserve(cap);

    for (int y = inner.y; y < inner.y + inner.height; ++y) {
        for (int x = inner.x; x < inner.x + inner.width; ++x) {
            const float z_mm = depthAt(depth_mm, x, y);
            if (!(z_mm > 0.0f)) continue;                      // 无效视差
            if (z_mm < static_cast<float>(cfg.min_depth_mm) ||
                z_mm > static_cast<float>(cfg.max_depth_mm)) {
                continue;                                      // 超出可信区间
            }
            const float d = dispAt(disp16, x, y);
            if (!(d > 0.0f)) continue;                         // 视差必须为正
            disps.push_back(d);
            depths.push_back(z_mm);
        }
    }

    // 有效占比 = 有效像素 / 框内（内缩后）总像素 —— 这是「这个 3D 结果有多可信」的量化指标
    out.total_pixels = static_cast<int>(cap);
    out.valid_pixels = static_cast<int>(disps.size());
    out.valid_ratio  = cap > 0 ? static_cast<float>(disps.size()) / static_cast<float>(cap) : 0.0f;

    if (disps.empty()) return;   // 该脸一个可信深度像素都没有 -> 保持 0，不编造

    // ---- 3.4 中值视差 -> 深度 -> 反投影 ----
    float d_med = 0.0f;
    if (!medianInPlace(disps, d_med) || !(d_med > 1e-6f)) return;

    // 全分辨率内参 * scale = 处理尺度内参（缩放不改变主点/焦距的比例关系）
    const double fx_p = calib.fx * s;
    const double fy_p = calib.fy * s;
    const double cx_p = calib.cx * s;
    const double cy_p = calib.cy * s;
    const double B_mm = calib.baseline_m * 1000.0;

    if (fx_p > 1e-6 && B_mm > 1e-6) {
        // 三角测量：Z = fx * B / d（这里 fx 用处理尺度、B 用毫米 -> Z 直接是毫米）
        const double z_mm = fx_p * B_mm / static_cast<double>(d_med);
        out.depth_median_mm = cvRound(z_mm);

        // 用【框中心像素】反投影：与「框中心 -> 3D 点」的语义一致
        const double u = box_proc.x + box_proc.width * 0.5;
        const double v = box_proc.y + box_proc.height * 0.5;
        const double Z = z_mm / 1000.0;   // 米
        out.center_m.x = static_cast<float>((u - cx_p) * Z / fx_p);
        out.center_m.y = static_cast<float>((v - cy_p) * Z / fy_p);
        out.center_m.z = static_cast<float>(Z);
        out.distance_m = static_cast<float>(
            std::sqrt(static_cast<double>(out.center_m.x) * out.center_m.x +
                      static_cast<double>(out.center_m.y) * out.center_m.y + Z * Z));
    } else {
        // 内参/基线不可用（理论上 prepare() 之后不会走到）：只能给 Z，X/Y 无从得知。
        // 这里宁可把 X/Y 留 0 并在注释里说清楚，也不拿「框大小估算」之类的假数据凑数。
        float z_med = 0.0f;
        if (!medianInPlace(depths, z_med)) return;
        out.depth_median_mm = cvRound(z_med);
        out.center_m = cv::Point3f(0.0f, 0.0f, z_med / 1000.0f);
        out.distance_m = z_med / 1000.0f;
    }
}

// ===========================================================================
//  4) 批处理：detect + locate
// ===========================================================================
void Face3DLocator::detectAndLocate(const cv::Mat& gray_proc,
                                    const cv::Mat& disp16,
                                    const cv::Mat& depth_mm,
                                    const StereoCalib& calib,
                                    const StereoConfig& cfg,
                                    std::vector<Face3D>& out) {
    out.clear();
    std::vector<cv::Rect> boxes;
    detect(gray_proc, boxes);
    out.reserve(boxes.size());
    for (const cv::Rect& b : boxes) {
        Face3D f;
        locate(b, disp16, depth_mm, calib, cfg, f);
        out.push_back(f);
    }
}

// ===========================================================================
//  5) 在全分辨率 BGR 上画框 + 距离标签 + 左上角 HUD
// ===========================================================================
void Face3DLocator::draw(cv::Mat& bgr_full, const std::vector<Face3D>& faces,
                         const StereoConfig& cfg, double fps) {
    (void)cfg;   // 坐标已经是全分辨率（box_full），cfg 保留在签名里方便上层统一传参
    if (bgr_full.empty()) return;
    if (bgr_full.channels() == 1) {
        cv::cvtColor(bgr_full, bgr_full, cv::COLOR_GRAY2BGR);   // 原地转换，调试视频也能画
    }

    // 绿色 = 有可信深度；橙色 = 只检到脸但框内没有有效深度（可信度不足）
    const cv::Scalar kGreen(60, 220, 60);
    const cv::Scalar kOrange(40, 165, 255);
    const cv::Scalar kBlack(0, 0, 0);

    for (const Face3D& f : faces) {
        const bool has_depth = (f.valid_pixels > 0 && f.depth_median_mm > 0);
        const cv::Scalar color = has_depth ? kGreen : kOrange;

        cv::rectangle(bgr_full, f.box_full, color, 2, cv::LINE_AA);

        char label[128];
        if (has_depth) {
            std::snprintf(label, sizeof(label), "D=%.2fm  v=%.0f%%", f.distance_m,
                          f.valid_ratio * 100.0f);
        } else {
            std::snprintf(label, sizeof(label), "D=--  v=0%%");
        }

        // 标签放在框上方；太靠顶就挪到框内，别被裁掉
        int th = 0;
        const cv::Size tsz = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.6, 1, &th);
        const int tw = tsz.width;
        const int x = f.box_full.x;
        const int y_text = (f.box_full.y > th + 10) ? (f.box_full.y - 8) : (f.box_full.y + th + 10);
        const int y_top  = y_text - tsz.height - 4;
        cv::rectangle(bgr_full, cv::Point(x, y_top), cv::Point(x + tw + 8, y_text + 4), color, -1);
        cv::putText(bgr_full, label, cv::Point(x + 4, y_text), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    kBlack, 1, cv::LINE_AA);
    }

    // ---- 左上角 HUD ----
    // 注意：draw() 的契约只给了 fps，拿不到 FrameStats；各阶段耗时请看 drawHud()。
    //
    // 这里对 fps <= 0 做特殊约定：表示「调用者不打算在这一层显示帧率」。
    // 典型场景是 main.cpp ——它把 FPS 和各阶段耗时统一画进独立的 HUD 面板
    // (buildHud)，所以调 draw() 时传 0.0 占位。如果这里照直 printf("%.1f", 0.0)，
    // 画面上就会出现一个 "0.0 FPS"，看起来像真实测量其实是占位符 —— 那就是
    // 假数据。宁可不显示，也不显示一个会被误读的 0。
    char hud[160];
    if (fps > 0.0) {
        std::snprintf(hud, sizeof(hud),
                      "stereo3d RK3588 | stereo depth + face 3D | %.1f FPS | %zu face",
                      fps, faces.size());
    } else {
        std::snprintf(hud, sizeof(hud), "stereo3d RK3588 | stereo depth + face 3D | %zu face",
                      faces.size());
    }
    int th = 0;
    cv::Size sz = cv::getTextSize(hud, cv::FONT_HERSHEY_SIMPLEX, 0.6, 1, &th);
    cv::rectangle(bgr_full, cv::Point(0, 0), cv::Point(sz.width + 24, 34), cv::Scalar(15, 23, 42),
                  -1);
    cv::putText(bgr_full, hud, cv::Point(12, 24), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                cv::Scalar(248, 200, 80), 1, cv::LINE_AA);
}

// ===========================================================================
//  6) 详细 HUD：FPS + 各阶段耗时 + 标定状态
// ===========================================================================
void Face3DLocator::drawHud(cv::Mat& bgr, const FrameStats& st, const StereoCalib& calib) {
    if (bgr.empty()) return;
    if (bgr.channels() == 1) {
        cv::cvtColor(bgr, bgr, cv::COLOR_GRAY2BGR);
    }

    const cv::Scalar kTitle(250, 245, 240);
    const cv::Scalar kText(230, 235, 240);
    const cv::Scalar kAccent(250, 190, 90);    // BGR 偏蓝 -> 画面里是淡蓝
    const cv::Scalar kWarn(80, 80, 255);

    std::vector<std::pair<std::string, cv::Scalar>> lines;
    char buf[256];

    lines.emplace_back("stereo3d RK3588 | stereo SGBM depth -> face 3D", kTitle);

    std::snprintf(buf, sizeof(buf), "FPS %.1f | frame #%d | faces %d", st.fps, st.frame_id,
                  st.n_faces);
    lines.emplace_back(buf, kAccent);

    std::snprintf(buf, sizeof(buf), "cap %.1f | rectify %.1f | sgbm %.1f | depth %.1f ms", st.cap_ms,
                  st.rect_ms, st.sgbm_ms, st.depth_ms);
    lines.emplace_back(buf, kText);

    std::snprintf(buf, sizeof(buf), "face %.1f | draw %.1f | jpeg %.1f ms", st.face_ms, st.draw_ms,
                  st.encode_ms);
    lines.emplace_back(buf, kText);

    if (calib.valid) {
        std::snprintf(buf, sizeof(buf), "calib RMS %.2f px | baseline %.1f mm | fx_full %.0f px",
                      calib.rms_reproj, calib.baseline_m * 1000.0, calib.fx);
        lines.emplace_back(buf, kText);
    } else {
        lines.emplace_back("UNCALIBRATED: relative depth OK, absolute distance NOT reliable",
                           kWarn);
    }

    const double fs = 0.55;
    const int line_h = 22;
    const int pad = 10;
    int max_w = 0;
    for (const auto& ln : lines) {
        int th = 0;
        max_w = std::max(max_w, cv::getTextSize(ln.first, cv::FONT_HERSHEY_SIMPLEX, fs, 1, &th).width);
    }

    const int box_w = max_w + pad * 2;
    const int box_h = static_cast<int>(lines.size()) * line_h + pad;
    cv::rectangle(bgr, cv::Point(0, 0), cv::Point(box_w, box_h), cv::Scalar(15, 23, 42), -1);

    int y = pad + line_h - 6;
    for (const auto& ln : lines) {
        cv::putText(bgr, ln.first, cv::Point(pad, y), cv::FONT_HERSHEY_SIMPLEX, fs, ln.second, 1,
                    cv::LINE_AA);
        y += line_h;
    }
}

}  // namespace s3d
