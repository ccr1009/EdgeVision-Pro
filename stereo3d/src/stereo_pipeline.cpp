// ============================================================================
//  stereo_pipeline.cpp —— 双目矫正 + SGBM 视差 + 深度 + 点云
//
//  owner: stereo-core
//  目标板: RK3588 / Debian 11 / OpenCV 4.5.1 / g++ 10.2.1 / C++17
//
//  【一句话数据流】
//    左/右原始 BGR(1280x720)
//      --remap(标定得到的 原图->矫正图 映射)--> 全分辨率矫正灰度图
//      --resize(INTER_AREA)------------------> 处理尺度灰度图(640x360)
//      --StereoSGBM--------------------------> 定点视差 CV_16S (像素 x16)
//      --Z = fx_proc * B / d ----------------> 深度 CV_32F (毫米)
//      --内参反投影--------------------------> 相机系 3D 点(米) / 点云
//
//  【为什么深度公式是 Z = fx * B / d】  ← 面试要能从头推
//    矫正(rectify)之后，两张图的极线已经水平对齐：同一个物点在左右图上
//    只在 u 方向有位移 d，v 方向相同（这是 stereoRectify 的目标）。
//    设物点在「矫正后左相机」坐标系下是 (X, Y, Z)，右相机光心在左相机
//    X 正方向 B 米处（基线），则：
//        左图: u_L = fx * X / Z + cx
//        右图: u_R = fx * (X - B) / Z + cx      ← 右相机看到的东西"偏右 B"
//    相减: d = u_L - u_R = fx * B / Z   ⇒   Z = fx * B / d
//    ★ 反比关系：视差越大越近。也就是说 d 的 1 个像素误差，在 1m 处约
//      1.7cm，在 10m 处约 1.7m —— 这正是"双目测远不可靠"的物理根因，
//      不是实现 bug，所以后面要做 min/max 深度裁剪。
//
//  【为什么缩到处理分辨率，深度结果不变】  ← 这条必须能用实验验证
//    等比缩放 s 后：d_proc = d_full * s，fx_proc = fx_full * s，
//    而基线 B 是物理量、不随图像缩放变化，代入公式 s 直接约掉：
//        Z = fx_proc * B / d_proc = (fx_full*s)*B / (d_full*s) = fx_full*B/d_full
//    这个等式一旦不成立，几乎只能是三种错：
//      ① fx 不是从 P1 取的（例如直接用 K_left，少了矫正后的重投影）;
//      ② 做了非等比缩放（fx、fy 缩放系数不同，u/v 方向尺度不一致）;
//      ③ 视差忘了按同一尺度换算（把全分辨率 d 当成了 proc 尺度的 d）。
//    所以 prepare() 里强制"等比缩放"，proc_h 由宽度推出而不是随便填。
//
//  【坐标约定】（与 stereo_types.h 一致，别弄反）
//    X 向右、Y 向下（图像坐标同向）、Z 向前；单位米。所以 Y 为正表示在光心下方。
// ============================================================================

#include "stereo_pipeline.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#ifdef HAVE_XIMGPROC
// 只有装了 opencv_contrib 才编进来（CMakeLists.txt 里探测到 ximgproc 才定义该宏）
#include <opencv2/ximgproc.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>   // std::getenv / std::atoi —— task-5 消融实验的参数覆盖入口
#include <string>
#include <vector>

namespace s3d {
namespace {

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;

inline void setErr(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

// 统一转成 CV_64F：FileStorage 读回来的矩阵可能是 float('f') 也可能是
// double('d')，后面一律用 at<double>() 取值，先归一化类型才不会读出错值。
cv::Mat asF64(const cv::Mat& m) {
    if (m.empty()) return cv::Mat();
    cv::Mat out;
    if (m.type() == CV_64F) out = m.clone();
    else m.convertTo(out, CV_64F);
    return out;
}

// 图像等比缩放 k 倍后（像素坐标 u' = k*u），内参/投影矩阵只需把前两行整体乘 k：
//   K' = [[k*fx, 0, k*cx], [0, k*fy, k*cy], [0, 0, 1]]
//   P' = [[k*fx, 0, k*cx, k*Tx], [0, k*fy, k*cy, k*Ty], [0, 0, 1, 0]]
// 注意：OpenCV 的畸变模型是在"归一化像平面"上定义的（先 (u-cx)/fx 再套 k1,k2,p1,p2），
// 所以等比缩放图像后畸变系数 D 完全不用改，只有 K/P 需要乘 k —— 这一步很容易写错。
cv::Mat scaleRows01(const cv::Mat& M, double k) {
    if (M.empty()) return cv::Mat();
    cv::Mat out;
    if (M.type() == CV_64F) out = M.clone();
    else M.convertTo(out, CV_64F);
    const int rows = std::min(2, out.rows);
    for (int r = 0; r < rows; ++r) {
        double* p = out.ptr<double>(r);
        for (int c = 0; c < out.cols; ++c) p[c] *= k;
    }
    return out;
}

// 任意外观(BGR/BGRA/灰度) -> CV_8UC1。SGBM 只吃单通道 8 位。
void toGray8(const cv::Mat& in, cv::Mat& out) {
    if (in.empty()) { out.release(); return; }
    if (in.type() == CV_8UC1) { out = in; return; }
    if (in.channels() == 3) { cv::cvtColor(in, out, cv::COLOR_BGR2GRAY); return; }
    if (in.channels() == 4) { cv::cvtColor(in, out, cv::COLOR_BGRA2GRAY); return; }
    in.convertTo(out, CV_8U);
}

// 从「标定分辨率下的全分辨率内参」到「处理尺度内参」的合并缩放系数。
//   cfg.scale   = proc_w / prepared_w      （prepare() 里算的，处理尺度/采集尺度）
//   k           = prepared_w / calib_w     （采集尺度/标定尺度，通常是 1）
//   乘积正好 = proc_w / calib_w，与 prepared 无关；这里保留 k 是为了支持
//   "标定时 1280x720、实际相机协商成 640x360" 这种等比降采样场景（等比时 fx 同比缩）。
double procIntrinsicScale(const StereoCalib& c, const StereoConfig& cfg, const cv::Size& prepared) {
    double k = 1.0;
    if (prepared.width > 0 && c.image_size.width > 0) {
        k = static_cast<double>(prepared.width) / static_cast<double>(c.image_size.width);
    }
    return cfg.scale * k;
}

// SGBM 参数归一化：这些是 OpenCV 的硬约束，写错会直接 CV_Assert 崩在板子上。
//   numDisparities 必须是 16 的倍数（内部按 16 个视差一组做 SIMD 向量化）
//   blockSize      必须是奇数（匹配窗口以中心像素对称）
void normalizeSgbmParams(StereoConfig& cfg, int width) {
    int nd = cfg.num_disp;
    if (nd < 16) nd = 16;
    nd = ((nd + 15) / 16) * 16;                       // 向上取整到 16 的倍数
    // SGBM 内部要算 x 与 x-numDisp 的代价，视差范围不能吃掉整幅宽度
    const int max_nd = ((std::max(16, width) - 1) / 16) * 16;
    if (max_nd >= 16 && nd > max_nd) nd = max_nd;
    cfg.num_disp = nd;

    int bs = cfg.block_size;
    if (bs < 3) bs = 3;
    if ((bs & 1) == 0) ++bs;                          // 偶数 -> 加 1 变奇数
    cfg.block_size = bs;
}

// SGBM / WLS 的原始输出 -> 本模块契约的「CV_16S 定点视差（像素 x16）」。
// 为什么 SGBM 自己就用定点：块匹配内部是整数 SAD，视差细化到 1/16 像素，
// 存 short 比 float 省一半内存、也便于 SIMD。
// 无效像素：SGBM 用 minDisparity-1 标记（minDisparity=0 时就是 -1，定点 -16），
// 本模块契约规定无效值 = 最小视差值*16 = 0，所以统一把负数夹到 0（向量化 setTo+mask，
// 比手写循环快，且比 cv::max(mat, scalar) 的重载更不容易踩到类型歧义）。
cv::Mat toDisp16(const cv::Mat& raw) {
    if (raw.empty()) return cv::Mat();

    cv::Mat s16;
    if (raw.type() == CV_16SC1) {
        s16 = raw;                                  // 已是定点，直接拷一份再改，不动调用方数据
    } else if (raw.type() == CV_32FC1) {
        raw.convertTo(s16, CV_16S, 16.0);           // 浮点视差(像素) x16，convertTo 自带饱和截断
    } else if (raw.type() == CV_16UC1) {
        raw.convertTo(s16, CV_16S);
    } else {
        return cv::Mat();
    }

    cv::Mat out = s16.clone();
    out.setTo(0, out < 0);                          // 负数(-16 表示无匹配)归零
    return out;
}

// 取 P(3x4) 的左上 3x3：initUndistortRectifyMap 的 newCameraMatrix 只用到
// 内参部分，第 4 列(平移)与它无关。传 3x3 是最稳的写法（各版本 OpenCV 对
// 直接传 3x4 的 P 的处理不完全一致，官方 sample 虽然是那样传的）。
cv::Mat intrinsics3x3(const cv::Mat& P) {
    if (P.empty()) return cv::Mat();
    if (P.rows == 3 && P.cols == 3) return P.clone();
    if (P.rows == 3 && P.cols == 4) return P.colRange(0, 3).clone();
    return cv::Mat();
}

// 一次性抑制重复告警（每帧都刷屏会淹掉真正的错误信息）
bool g_warned_rect_prepare = false;
bool g_warned_wls = false;

}  // namespace

// ===========================================================================
//  1) 标定装载
// ===========================================================================
bool StereoPipeline::loadCalib(const std::string& yml_path, std::string* err) {
    // 先清空：避免"上一次标定残留 + 这次只读到一半"的混合状态
    calib_ = StereoCalib{};
    prepared_size_ = cv::Size(0, 0);
    procL_.release();
    procR_.release();

    if (yml_path.empty()) {
        setErr(err, "标定文件路径为空");
        return false;
    }

    cv::FileStorage fs;
    try {
        fs.open(yml_path, cv::FileStorage::READ);
    } catch (const cv::Exception& e) {
        setErr(err, std::string("FileStorage 打开标定文件异常: ") + e.what());
        return false;
    }
    if (!fs.isOpened()) {
        setErr(err, "打不开标定文件(路径/权限?): " + yml_path);
        return false;
    }

    int w = 0, h = 0;
    fs["image_width"]  >> w;
    fs["image_height"] >> h;

    fs["K_left"]  >> calib_.K_L;
    fs["D_left"]  >> calib_.D_L;
    fs["K_right"] >> calib_.K_R;
    fs["D_right"] >> calib_.D_R;
    fs["R1"]      >> calib_.R_L;
    fs["R2"]      >> calib_.R_R;
    fs["P1"]      >> calib_.P_L;
    fs["P2"]      >> calib_.P_R;
    fs["Q"]       >> calib_.Q;

    double rms = 0.0;
    double baseline_file = 0.0;
    fs["rms"]        >> rms;
    fs["baseline_m"] >> baseline_file;
    fs.release();

    // ---- 形状校验：宁可在这里明确报错，也不要带着错矩阵跑出"看起来正常"的垃圾深度 ----
    auto bad = [&](const char* what) {
        setErr(err, std::string("标定文件缺少字段或形状不对: ") + what + " (" + yml_path + ")");
        return false;
    };
    if (w <= 0 || h <= 0) return bad("image_width/image_height");
    if (!(calib_.K_L.rows == 3 && calib_.K_L.cols == 3)) return bad("K_left 必须是 3x3");
    if (!(calib_.K_R.rows == 3 && calib_.K_R.cols == 3)) return bad("K_right 必须是 3x3");
    if (calib_.D_L.empty() || calib_.D_R.empty())        return bad("D_left/D_right");
    if (!(calib_.R_L.rows == 3 && calib_.R_L.cols == 3)) return bad("R1 必须是 3x3");
    if (!(calib_.R_R.rows == 3 && calib_.R_R.cols == 3)) return bad("R2 必须是 3x3");
    if (!(calib_.P_L.rows == 3 && calib_.P_L.cols == 4)) return bad("P1 必须是 3x4");
    if (!(calib_.P_R.rows == 3 && calib_.P_R.cols == 4)) return bad("P2 必须是 3x4");

    calib_.K_L = asF64(calib_.K_L);
    calib_.D_L = asF64(calib_.D_L);
    calib_.K_R = asF64(calib_.K_R);
    calib_.D_R = asF64(calib_.D_R);
    calib_.R_L = asF64(calib_.R_L);
    calib_.R_R = asF64(calib_.R_R);
    calib_.P_L = asF64(calib_.P_L);
    calib_.P_R = asF64(calib_.P_R);
    if (!calib_.Q.empty()) calib_.Q = asF64(calib_.Q);

    calib_.image_size = cv::Size(w, h);

    // 矫正后左目内参 = P1 的左上 3x3（★ 契约规定，不能改用 K_left：
    // stereoRectify 会把内参微调成同一套虚拟相机，左右两目必须用 P 里的值才自洽）
    calib_.fx = calib_.P_L.at<double>(0, 0);
    calib_.fy = calib_.P_L.at<double>(1, 1);
    calib_.cx = calib_.P_L.at<double>(0, 2);
    calib_.cy = calib_.P_L.at<double>(1, 2);

    const double p2fx = calib_.P_R.at<double>(0, 0);
    if (std::abs(p2fx) < 1e-9) return bad("P2(0,0) 为 0，无法换算基线");
    // 基线的来源：P2 相对 P1 的 x 方向平移就是 -fx'*B，
    // 所以 B = |P2(0,3) / P2(0,0)|（单位与标定板方格边长一致，本项目用米）
    calib_.baseline_m = std::abs(calib_.P_R.at<double>(0, 3) / p2fx);

    if (!(calib_.fx > 0.0) || !(calib_.fy > 0.0)) {
        setErr(err, "P1 的 fx/fy 非法(<=0)，标定文件可能损坏: " + yml_path);
        return false;
    }
    if (!(calib_.baseline_m > 0.0) || calib_.baseline_m > 10.0) {
        setErr(err, "换算出的基线不合理(应在 0~10m 之间)，检查 P2: " + yml_path);
        return false;
    }

    // 文件里的 baseline_m(=norm(T)) 与 P2 换算是两个独立来源，顺手互校一下：
    // 相差超过 2% 说明标定/写入过程有问题（常见于 T 用 mm 而 Python 侧写了 m）
    if (baseline_file > 0.0) {
        const double rel = std::abs(baseline_file - calib_.baseline_m) / calib_.baseline_m;
        if (rel > 0.02) {
            std::fprintf(stderr,
                "[stereo_pipeline] 警告: baseline_m 双源不一致: 文件=%.5fm, P2换算=%.5fm (差 %.2f%%)，以 P2 为准\n",
                baseline_file, calib_.baseline_m, rel * 100.0);
        }
    }

    calib_.rms_reproj = rms;
    calib_.source = yml_path;
    calib_.valid = true;

    std::fprintf(stdout,
        "[stereo_pipeline] 标定装载成功: %s | %dx%d | fx=%.2f fy=%.2f cx=%.2f cy=%.2f | "
        "baseline=%.2fmm | rms=%.3fpx\n",
        yml_path.c_str(), calib_.image_size.width, calib_.image_size.height,
        calib_.fx, calib_.fy, calib_.cx, calib_.cy,
        calib_.baseline_m * 1000.0, calib_.rms_reproj);
    return true;
}

// ---------------------------------------------------------------------------
//  未标定兜底：理想平行双目
//  ⚠ 语义必须讲清楚（网页 HUD 也要提示）：这条路径下
//    「相对深度可信」（同一场景内远近对比是对的），
//    「绝对距离不可信」（基线是猜的、镜头畸变没修、两颗摄像头型号也可能不同）。
// ---------------------------------------------------------------------------
bool StereoPipeline::initIdeal(const cv::Size& image_size, double hfov_deg,
                               double baseline_m, std::string* err) {
    calib_ = StereoCalib{};
    prepared_size_ = cv::Size(0, 0);
    procL_.release();
    procR_.release();

    if (image_size.width <= 0 || image_size.height <= 0) {
        setErr(err, "initIdeal: image_size 非法");
        return false;
    }
    if (!(hfov_deg > 1.0 && hfov_deg < 179.0)) {
        setErr(err, "initIdeal: 水平 FOV 应在 (1, 179) 度之间");
        return false;
    }
    if (!(baseline_m > 0.0 && baseline_m < 10.0)) {
        setErr(err, "initIdeal: 基线应在 (0, 10) 米之间");
        return false;
    }

    const double W = static_cast<double>(image_size.width);
    const double H = static_cast<double>(image_size.height);

    // 由水平 FOV 反推焦距：视锥半角处满足 tan(hfov/2) = (W/2) / fx
    const double fx = (W * 0.5) / std::tan(hfov_deg * 0.5 * kPi / 180.0);
    // 没有竖直 FOV 信息，按"方形像元"假设 fy = fx（UVC 摄像头绝大多数成立）
    const double fy = fx;
    const double cx = W * 0.5;
    const double cy = H * 0.5;

    calib_.image_size = image_size;
    calib_.K_L = (cv::Mat_<double>(3, 3) << fx, 0.0, cx,
                                            0.0, fy, cy,
                                            0.0, 0.0, 1.0);
    calib_.K_R = calib_.K_L.clone();

    // 畸变全 0：没标定就没有畸变模型，硬塞一个猜测值只会让几何更错
    calib_.D_L = cv::Mat::zeros(1, 5, CV_64F);
    calib_.D_R = cv::Mat::zeros(1, 5, CV_64F);

    // 矫正旋转 = 单位阵：假设两相机已经"物理平行"，不需要旋转矫正
    calib_.R_L = cv::Mat::eye(3, 3, CV_64F);
    calib_.R_R = cv::Mat::eye(3, 3, CV_64F);

    // P1 = [K | 0]，P2 = [K | -fx*B]：右目相对左目在 X 方向平移 B，
    // 平移项换成像素单位就是 -fx*B（这样 P2(0,3)/P2(0,0) = -B，反算基线自洽）
    calib_.P_L = cv::Mat::zeros(3, 4, CV_64F);
    // 注意：cv::Mat 的 operator() 只支持 Range/Rect 切片，取单个元素必须用 at<double>
    calib_.P_L.at<double>(0, 0) = fx;
    calib_.P_L.at<double>(0, 2) = cx;
    calib_.P_L.at<double>(1, 1) = fy;
    calib_.P_L.at<double>(1, 2) = cy;
    calib_.P_L.at<double>(2, 2) = 1.0;

    calib_.P_R = calib_.P_L.clone();
    calib_.P_R.at<double>(0, 3) = -fx * baseline_m;

    // Q：视差->深度的 4x4 齐次矩阵。本模块的深度是直接用 P1/P2 算的，
    // Q 只作元数据（reprojectImageTo3D 之类的工具要用）。这里按本模块的
    // 公式 Z = fx*B/d 自洽构造：取 Z'/W' 展开后 W' = d/B，三行分别是
    //   X = (u-cx)*B/d, Y = (v-cy)*fx/fy*B/d, Z = fx*B/d
    calib_.Q = cv::Mat::zeros(4, 4, CV_64F);
    calib_.Q.at<double>(0, 0) = 1.0;
    calib_.Q.at<double>(0, 3) = -cx;
    calib_.Q.at<double>(1, 1) = fx / fy;
    calib_.Q.at<double>(1, 3) = -cy * fx / fy;
    calib_.Q.at<double>(2, 3) = fx;
    calib_.Q.at<double>(3, 2) = 1.0 / baseline_m;

    calib_.fx = fx;
    calib_.fy = fy;
    calib_.cx = cx;
    calib_.cy = cy;
    calib_.baseline_m = baseline_m;
    calib_.rms_reproj = 0.0;
    // ★ 契约字符串（docs/CALIB_FORMAT.md 第 5 节）。头文件注释里有个拼写笔误
    //   "UNCAILBRATED"，以文档/任务书为准写成 UNCALIBRATED。
    calib_.source = "SYNTHETIC/UNCALIBRATED";
    calib_.valid = true;

    std::fprintf(stdout,
        "[stereo_pipeline] 未标定模式(理想平行双目): %dx%d | hfov=%.1f° -> fx=fy=%.2f | "
        "baseline=%.1fmm(假设值, 绝对距离不可信)\n",
        image_size.width, image_size.height, hfov_deg, fx, baseline_m * 1000.0);
    return true;
}

// ===========================================================================
//  2) 准备：算尺度、归一化 SGBM 参数、建 remap 表
// ===========================================================================
bool StereoPipeline::prepare(const cv::Size& image_size, std::string* err) {
    if (!calib_.valid) {
        setErr(err, "prepare() 之前必须先调用 loadCalib() 或 initIdeal()");
        return false;
    }

    cv::Size sz = image_size;
    if (sz.width <= 0 || sz.height <= 0) sz = calib_.image_size;
    if (sz.width <= 0 || sz.height <= 0) {
        setErr(err, "prepare(): image_size 无效");
        return false;
    }
    if (calib_.image_size.width <= 0 || calib_.image_size.height <= 0) {
        calib_.image_size = sz;  // 标定文件没写分辨率时以实际采集为准
    }

    // 采集分辨率必须是标定分辨率的"等比缩放"，否则内参不能简单按比例换算
    //（fx、fy 缩放系数不同时，SGBM 的水平极线约束和 Z=fx*B/d 都不再成立）
    const double k = static_cast<double>(sz.width) / static_cast<double>(calib_.image_size.width);
    const double expect_h = static_cast<double>(calib_.image_size.height) * k;
    if (std::abs(expect_h - static_cast<double>(sz.height)) > 1.5) {
        setErr(err,
               "采集分辨率 " + std::to_string(sz.width) + "x" + std::to_string(sz.height) +
               " 与标定分辨率 " + std::to_string(calib_.image_size.width) + "x" +
               std::to_string(calib_.image_size.height) +
               " 宽高比不一致，请用相同的宽高比采集(或重新标定)");
        return false;
    }

    // ---- 处理分辨率：宽度由 config 给，高度按宽高比推 ----
    // 为什么不能直接信 cfg_.proc_h：默认配置是 640x480(4:3)，而采集是 1280x720(16:9)，
    // 强行缩到 640x480 会让 x 缩 0.5、y 缩 0.667 —— 非等比缩放。后果是：
    //   · SGBM 假设的"水平极线"仍在（行对齐不受影响），但 fy 尺度不对，
    //     反投影出来的 Y 坐标会偏；
    //   · 更直接的麻烦是 cfg_.scale 只有一个，face_3d 用 1/scale 换算全分辨率
    //     框坐标时，x 对 y 就错了。
    // 所以这里以宽度为准、等比推高度，并把修正结果写回 cfg_（procSize() 读的是 cfg_，
    // 这样上层拿到的处理尺寸和内存里真实的一致）。
    int pw = cfg_.proc_w;
    if (pw <= 0) pw = sz.width;
    pw = clampi(pw, 16, sz.width);
    pw &= ~1;  // 取偶数：后续 resize/SIMD 按对访问更友好
    int ph = static_cast<int>(std::lround(static_cast<double>(sz.height) * (static_cast<double>(pw) / sz.width)));
    ph &= ~1;
    if (ph < 2) ph = 2;

    if (cfg_.proc_h != ph) {
        if (cfg_.proc_h > 0) {
            std::fprintf(stderr,
                "[stereo_pipeline] proc_h=%d 与采集宽高比 %dx%d 冲突，已等比修正为 %d"
                "(非等比缩放会把 Y 方向的尺度搞错)\n",
                cfg_.proc_h, sz.width, sz.height, ph);
        }
        cfg_.proc_h = ph;
    }
    if (cfg_.proc_w != pw) {
        std::fprintf(stderr, "[stereo_pipeline] proc_w=%d 不合法，已修正为 %d\n", cfg_.proc_w, pw);
        cfg_.proc_w = pw;
    }
    // 契约公式：scale = proc_w / image_width
    cfg_.scale = static_cast<double>(cfg_.proc_w) / static_cast<double>(sz.width);

    // SGBM 参数归一化（16 的倍数 / 奇数），同时把结果写回 cfg_，
    // 保证 colorizeDisparity(disp16, cfg_.num_disp) 用的是同一个量程
    normalizeSgbmParams(cfg_, cfg_.proc_w);

    // ---- 建 remap 表 ----
    prepared_size_ = sz;
    procL_.release();
    procR_.release();
    rebuildMaps();
    if (calib_.mapL1.empty() || calib_.mapR1.empty()) {
        setErr(err, "initUndistortRectifyMap 生成映射表失败（检查 K/D/R/P 类型与数值）");
        return false;
    }

    const double s_proc = procIntrinsicScale(calib_, cfg_, prepared_size_);
    std::fprintf(stdout,
        "[stereo_pipeline] prepare: 采集 %dx%d -> 处理 %dx%d (scale=%.4f) | "
        "fx_proc=%.2f fy_proc=%.2f cx_proc=%.2f cy_proc=%.2f | num_disp=%d block=%d | "
        "标定来源=%s\n",
        sz.width, sz.height, cfg_.proc_w, cfg_.proc_h, cfg_.scale,
        calib_.fx * s_proc, calib_.fy * s_proc, calib_.cx * s_proc, calib_.cy * s_proc,
        cfg_.num_disp, cfg_.block_size, calib_.source.c_str());
    return true;
}

// 生成 4 张 remap 表（矫正图坐标 -> 原图坐标，CV_16SC2 + CV_16UC1 的定点组合）。
// 为什么用 CV_16SC2 + CV_16UC1 而不是两张 CV_32FC1（4+2=6 字节/像素 vs 8 字节/像素）：
//   1) 访存：定点模式下 remap 用整数双线性分支，每像素只读 2+2 字节的整数对，
//      而浮点模式要读 2 张 float 表(8 字节)。RK3588 的 DDR 带宽是共享瓶颈，
//      整幅 1280x720 表按 A53 的 cache 行粒度访问时，省下 25% 表流量直接体现在帧率上。
//   2) 精度：map1 存整数部分(x,y)，map2 存"亚像素插值表索引"——把小数部分
//      dx,dy 各量化到 1/32 像素，然后合成一个 10 位索引 dx*32+dy（范围 0~1023）。
//      remap 拿这个索引直接去查预计算的 INTER_REMAP_COEFS 权重表，不用现场算乘法。
//      1/32 px 的量化误差对 1280 宽的图完全够用（深度误差可忽略），
//      而 short 能表示到 32767，远大于图像尺寸，不会溢出。
//   3) 一个极易踩错的事实：initUndistortRectifyMap 在 m1type=CV_16SC2 时，
//      **OpenCV 一定把 map2 生成为 CV_16UC1**（不存在"16SC2 配 32FC1"的组合）。
//      想要 CV_32FC1 的 map2 就得传 m1type=CV_32FC1/FC2，那是另一条路（两张 float 表）。
void StereoPipeline::rebuildMaps() {
    calib_.mapL1.release();
    calib_.mapL2.release();
    calib_.mapR1.release();
    calib_.mapR2.release();
    if (!calib_.valid || prepared_size_.width <= 0 || prepared_size_.height <= 0) return;

    // 标定分辨率 -> 采集分辨率 的缩放（通常 =1，相机的实际分辨率可能和标定时不同）
    double k = 1.0;
    if (calib_.image_size.width > 0) {
        k = static_cast<double>(prepared_size_.width) / static_cast<double>(calib_.image_size.width);
    }

    const cv::Mat K_L = scaleRows01(calib_.K_L, k);
    const cv::Mat K_R = scaleRows01(calib_.K_R, k);
    // 矫正后虚拟相机内参：取 P 的左上 3x3 再缩放。
    const cv::Mat P_L = scaleRows01(intrinsics3x3(calib_.P_L), k);
    const cv::Mat P_R = scaleRows01(intrinsics3x3(calib_.P_R), k);
    if (K_L.empty() || K_R.empty() || P_L.empty() || P_R.empty()) {
        std::fprintf(stderr, "[stereo_pipeline] rebuildMaps: 内参矩阵为空，无法建映射表\n");
        return;
    }

    // initUndistortRectifyMap 的方向：给定"矫正图上每个像素"，反查"它在原图上的位置"。
    //   cameraMatrix    = 原图内参 K
    //   distCoeffs      = 原图畸变 D
    //   R               = 矫正旋转（把原相机坐标系转到虚拟平行相机）
    //   newCameraMatrix = 矫正后虚拟相机的内参（取 P 的左上 3x3）
    // map1 = CV_16SC2（short 整数坐标），map2 = CV_16UC1（1/32 px 定点小数索引）。
    // 这是 OpenCV 里 m1type=CV_16SC2 的唯一产出组合，remap 会自动走定点双线性分支。
    try {
        cv::initUndistortRectifyMap(K_L, calib_.D_L, calib_.R_L, P_L, prepared_size_,
                                    CV_16SC2, calib_.mapL1, calib_.mapL2);
        cv::initUndistortRectifyMap(K_R, calib_.D_R, calib_.R_R, P_R, prepared_size_,
                                    CV_16SC2, calib_.mapR1, calib_.mapR2);
    } catch (const cv::Exception& e) {
        std::fprintf(stderr, "[stereo_pipeline] initUndistortRectifyMap 抛异常: %s\n", e.what());
        calib_.mapL1.release();
        calib_.mapR1.release();
        return;
    }

    // 自检：m1type=CV_16SC2 时 map1 必须是 CV_16SC2、map2 必须是 CV_16UC1。
    // 这两个类型是 remap 定点分支的硬前提，任一不符 remap 会直接抛异常。
    if (calib_.mapL1.type() != CV_16SC2 || calib_.mapL2.type() != CV_16UC1) {
        std::fprintf(stderr, "[stereo_pipeline] 意外: map 类型 = (m1=%d, m2=%d)，期望 (%d, %d)\n",
                     calib_.mapL1.type(), calib_.mapL2.type(), CV_16SC2, CV_16UC1);
    }
}

// ===========================================================================
//  3) 每帧：矫正
// ===========================================================================
void StereoPipeline::rectify(const cv::Mat& leftBGR, const cv::Mat& rightBGR,
                             cv::Mat& rectLfull, cv::Mat& rectRfull) {
    rectLfull.release();
    rectRfull.release();

    if (leftBGR.empty() || rightBGR.empty()) return;
    if (leftBGR.size() != rightBGR.size()) {
        std::fprintf(stderr, "[stereo_pipeline] rectify: 左右图尺寸不一致 %dx%d vs %dx%d\n",
                     leftBGR.cols, leftBGR.rows, rightBGR.cols, rightBGR.rows);
        return;
    }

    // 没 prepare 过，或者相机协商出来的分辨率变了 -> 按当前分辨率重建一次映射表。
    // 这样上层就算忘了调 prepare()，或者摄像头插拔后分辨率变动，也能自愈。
    if (calib_.mapL1.empty() || leftBGR.size() != prepared_size_) {
        std::string e;
        if (!prepare(leftBGR.size(), &e)) {
            if (!g_warned_rect_prepare) {
                std::fprintf(stderr, "[stereo_pipeline] rectify: 自动 prepare 失败: %s\n", e.c_str());
                g_warned_rect_prepare = true;
            }
            return;
        }
    }

    // 先转灰度再 remap：remap 是逐像素双线性采样，1 通道比 3 通道省 3 倍访存；
    // 几何重采样与颜色空间无关（线性插值在灰度域与 BGR 域等价，只是取整误差极小）。
    cv::Mat gL, gR;
    toGray8(leftBGR, gL);
    toGray8(rightBGR, gR);

    cv::remap(gL, rectLfull, calib_.mapL1, calib_.mapL2, cv::INTER_LINEAR,
              cv::BORDER_CONSTANT, cv::Scalar(0));
    cv::remap(gR, rectRfull, calib_.mapR1, calib_.mapR2, cv::INTER_LINEAR,
              cv::BORDER_CONSTANT, cv::Scalar(0));

    // 缓存处理尺度灰度图（SGBM / 人脸检测都用它，避免重复缩放）
    // 用 INTER_AREA 而不是 INTER_LINEAR：缩小时 AREA 是做"像素面积平均"，
    // 等价于先低通再抽样，不会像双线性那样漏采样产生摩尔纹 —— 纹理混叠对
    // 块匹配是致命的（左右目混叠方式不同，匹配代价会乱跳）。
    const cv::Size ps(cfg_.proc_w, cfg_.proc_h);
    if (ps.width > 0 && ps.height > 0 && rectLfull.size() != ps) {
        cv::resize(rectLfull, procL_, ps, 0, 0, cv::INTER_AREA);
        cv::resize(rectRfull, procR_, ps, 0, 0, cv::INTER_AREA);
    } else {
        procL_ = rectLfull.clone();
        procR_ = rectRfull.clone();
    }
}

// ===========================================================================
//  4) SGBM 视差
// ===========================================================================
void StereoPipeline::computeDisparity(const cv::Mat& rectLproc, const cv::Mat& rectRproc,
                                      cv::Mat& disp16) {
    disp16.release();

    // 允许传空 -> 用上一次 rectify() 缓存的处理尺度图（上层写 p.computeDisparity({}, {}, d) 也行）
    const cv::Mat& L = rectLproc.empty() ? procL_ : rectLproc;
    const cv::Mat& R = rectRproc.empty() ? procR_ : rectRproc;
    if (L.empty() || R.empty() || L.size() != R.size()) {
        std::fprintf(stderr, "[stereo_pipeline] computeDisparity: 输入无效或左右尺寸不一致\n");
        return;
    }

    cv::Mat gL, gR;
    toGray8(L, gL);
    toGray8(R, gR);

    // 每帧再归一化一次：防止上层直接改 config().num_disp/block_size 绕过 prepare()
    StereoConfig local = cfg_;

    // -------------------------------------------------------------------
    // 【task-5 消融实验入口】环境变量覆盖 SGBM 参数（不接 main.cpp 的改动）
    //
    //   STEREO_NUM_DISP  : 覆盖 numDisparities（视差搜索范围，必须是 16 的倍数）
    //   STEREO_BLOCK     : 覆盖 blockSize（匹配窗口边长，必须是奇数）
    //
    // 【为什么要消融 —— O(W·H·N·D) 复杂度】
    //   SGBM 单帧成本 ≈ O(W · H · num_disp · 路径数)：每个像素 (W·H) 要为
    //   每个候选视差 D=num_disp 维护代价聚合，再沿 3WAY 的 3 个方向递推。
    //   所以 num_disp 从 64 砍到 32，理论耗时直接减半；blockSize 从 5 加到 7，
    //   SAD 单块代价从 25 像素涨到 49 像素（约 2 倍访存与加法），也会显著变慢。
    //   但换来的收益方向相反：num_disp 越大看得越近（最大可测深度越近）、
    //   blockSize 越大抗噪越强但小物体/边缘越糊 —— 速度与质量是一对矛盾。
    //   本实验用 6 组参数 (num_disp 64/48/32 × block 5/7) 实测 sgbm_ms 与
    //   有效视差占比，找出"速度/质量拐点"，为后续降分辨率(480x270)决策提供依据。
    //
    //   实现方式：getenv 每帧读一次成本可忽略（<1us），缺省 nullptr 时用 cfg_
    //   原值，行为与改造前完全一致 —— 生产环境不设这两个变量就零影响。
    // -------------------------------------------------------------------
    if (const char* env_nd = std::getenv("STEREO_NUM_DISP")) {
        const int v = std::atoi(env_nd);
        if (v > 0) local.num_disp = v;   // 非法值(<=0)忽略，走 cfg_ 缺省
    }
    if (const char* env_bs = std::getenv("STEREO_BLOCK")) {
        const int v = std::atoi(env_bs);
        if (v > 0) local.block_size = v; // 非法值(<=0)忽略，走 cfg_ 缺省
    }
    normalizeSgbmParams(local, gL.cols);

    const int bs = local.block_size;
    // P1 / P2 是 SGBM 里"相邻视差平滑性"的惩罚项，标准取法见 Hirschmüller 论文与
    // OpenCV 官方示例：P1 = 8 * 通道数 * blockSize²，P2 = 32 * 通道数 * blockSize²。
    // 直觉：P2 = 4*P1 使得"视差不连续处"（前后景边界）允许跳变，
    // 而"视差连续但有小抖动"处被 P1 压平。乘 blockSize² 是因为代价是按块累加的，
    // 块越大累计的 SAD 越大，惩罚项要同步放大才等价。
    const int p1 = 8 * bs * bs;
    const int p2 = 32 * bs * bs;

    // MODE_SGBM_3WAY：把一维路径代价的递推拆成两步做，结果与 MODE_SGBM 数学等价
    // 但访存少、SIMD 友好，实测比 MODE_SGBM 快约 20~30%，RK3588 上直接收益。
    cv::Ptr<cv::StereoSGBM> sgbm = cv::StereoSGBM::create(
        0,                 // minDisparity：本项目约定从 0 开始（P2 的平移已含在标定里）
        local.num_disp,    // numDisparities
        bs,                // blockSize
        p1, p2,
        2,                 // disp12MaxDiff：左右一致性检查容差(像素)，2 能滤掉遮挡区错误
        63,                // preFilterCap：Sobel 预处理截断，压缩亮暗差异的影响
        10,                // uniquenessRatio：最优代价需比次优好 10%，抑制重复纹理误匹配
        100,               // speckleWindowSize：小于 100 像素的孤立斑点判为噪声
        2,                 // speckleRange：斑点内视差允许波动
        cv::StereoSGBM::MODE_SGBM_3WAY);

    cv::Mat raw;
    if (local.use_wls) {
#ifdef HAVE_XIMGPROC
        // WLS(加权最小二乘)滤波：用左目图像做引导，把视差图"贴"回真实边缘，
        // 代价是额外一次反向匹配 + 全局优化，板上大约慢 1 倍，默认关闭。
        if (!g_warned_wls) {
            std::fprintf(stdout, "[stereo_pipeline] WLS 视差滤波已启用(ximgproc)\n");
            g_warned_wls = true;
        }
        cv::Ptr<cv::StereoMatcher> right = cv::ximgproc::createRightMatcher(sgbm);
        cv::Ptr<cv::ximgproc::DisparityWLSFilter> wls =
            cv::ximgproc::createDisparityWLSFilter(sgbm);
        wls->setLambda(8000.0);   // 数据项与平滑项的权重，越大越平滑
        wls->setSigmaColor(1.5);  // 颜色相似度带宽，越大越愿意跨越颜色边缘
        cv::Mat rawR, filtered;
        sgbm->compute(gL, gR, raw);
        right->compute(gR, gL, rawR);
        wls->filter(raw, gL, filtered, rawR);
        raw = filtered;
#else
        if (!g_warned_wls) {
            std::fprintf(stderr,
                "[stereo_pipeline] 配置要求 WLS，但编译时没有 opencv_contrib/ximgproc，自动降级为普通 SGBM\n");
            g_warned_wls = true;
        }
        sgbm->compute(gL, gR, raw);
#endif
    } else {
        sgbm->compute(gL, gR, raw);
    }

    disp16 = toDisp16(raw);
    if (disp16.empty()) {
        std::fprintf(stderr, "[stereo_pipeline] computeDisparity: SGBM 输出类型不支持\n");
    }
}

// ===========================================================================
//  5) 视差 -> 深度
// ===========================================================================
void StereoPipeline::depthFromDisparity(const cv::Mat& disp16, cv::Mat& depth_mm) {
    depth_mm.release();
    if (disp16.empty() || !calib_.valid) return;

    // fx_proc：矫正后左目焦距缩到处理尺度的值。见文件头注释，缩放前后深度一致。
    const double s = procIntrinsicScale(calib_, cfg_, prepared_size_);
    const double fx_proc = calib_.fx * s;
    const double baseline_mm = calib_.baseline_m * 1000.0;
    if (!(fx_proc > 0.0) || !(baseline_mm > 0.0)) return;

    // 统一成"像素"单位的浮点视差：CV_16S 是定点(像素x16)，除以 16；
    // 万一上层给的是 CV_32F（像素），直接用。
    cv::Mat d;
    if (disp16.type() == CV_16SC1) {
        disp16.convertTo(d, CV_32F, 1.0 / 16.0);
    } else {
        disp16.convertTo(d, CV_32F);
    }

    const int rows = d.rows, cols = d.cols;
    const double minD = static_cast<double>(cfg_.min_depth_mm);
    const double maxD = static_cast<double>(cfg_.max_depth_mm);

    // 输出 CV_32FC1，单位毫米，无效 = 0。
    // 为什么用 float 而不是 16U：毫米量级 0~12000 塞得下 16U，但深度后续要参与
    // 中位数/统计，float 免掉整数转换；且 32F 与 OpenCV 其它函数衔接更顺。
    depth_mm.create(rows, cols, CV_32F);
    depth_mm.setTo(0);

    for (int y = 0; y < rows; ++y) {
        const float* dp = d.ptr<float>(y);
        float* zp = depth_mm.ptr<float>(y);
        for (int x = 0; x < cols; ++x) {
            const float dd = dp[x];
            // dd <= 0 或 NaN 一律无效：SGBM 未匹配/遮挡区就是这些值，
            // 硬算会得到 Z=inf 或负数，污染后面的人脸深度中位数。
            if (!(dd > 0.0f)) continue;
            const double z = fx_proc * baseline_mm / static_cast<double>(dd);
            // 越界裁剪：太近(<250mm)多半是误匹配；太远(>12m)是 1 像素视差抖动
            // 放大后的噪声，留着只会让画面"鬼影"。裁剪后置 0 = 无效。
            if (z < minD || z > maxD) continue;
            zp[x] = static_cast<float>(z);
        }
    }
}

// ---------------------------------------------------------------------------
//  处理尺度像素 + 视差 -> 相机系 3D 点(米)
// ---------------------------------------------------------------------------
bool StereoPipeline::backprojectProc(const cv::Point2f& pt_proc, float disp_px,
                                     cv::Point3f& out_m) const {
    if (!calib_.valid) return false;
    if (!(disp_px > 0.0f)) return false;

    const double s = procIntrinsicScale(calib_, cfg_, prepared_size_);
    const double fx = calib_.fx * s;
    const double fy = calib_.fy * s;
    const double cx = calib_.cx * s;
    const double cy = calib_.cy * s;
    if (!(fx > 0.0) || !(fy > 0.0) || !(calib_.baseline_m > 0.0)) return false;

    // 先由视差得深度，再按针孔模型把 (u,v) 摊到 X/Y。
    // 反投影本质是"把归一化像平面上的方向 (u-cx)/fx 乘上深度"，
    // 所以 X/Z = (u-cx)/fx，移项就得到下面两行。
    const double Z = fx * calib_.baseline_m / static_cast<double>(disp_px);
    if (!std::isfinite(Z) || Z <= 0.0) return false;

    out_m.x = static_cast<float>((static_cast<double>(pt_proc.x) - cx) * Z / fx);
    out_m.y = static_cast<float>((static_cast<double>(pt_proc.y) - cy) * Z / fy);
    out_m.z = static_cast<float>(Z);
    return true;
}

// ===========================================================================
//  6) 可视化
// ===========================================================================
// 视差伪彩：固定量程（0 ~ num_disp 像素映射到蓝->红），不做"本帧最大最小归一化"。
// 为什么不按帧内极值归一化：那样每一帧的色阶基准都在变，物体稍微动一下整幅
// 颜色就跟着跳，既看不出"远近变化"，也没法看时间稳定性；固定量程下
// "同一个颜色 = 同一个视差 = 同一个距离"，这才是可视化该有的语义。
cv::Mat StereoPipeline::colorizeDisparity(const cv::Mat& disp16, int num_disp) {
    if (disp16.empty()) return cv::Mat();
    const int nd = std::max(1, num_disp);

    cv::Mat d32;
    if (disp16.type() == CV_16SC1) disp16.convertTo(d32, CV_32F, 1.0 / 16.0);
    else disp16.convertTo(d32, CV_32F);

    cv::Mat g8(d32.size(), CV_8UC1, cv::Scalar(0));
    cv::Mat invalid = cv::Mat::zeros(d32.size(), CV_8UC1);
    const float scale = 255.0f / static_cast<float>(nd);
    for (int y = 0; y < d32.rows; ++y) {
        const float* dp = d32.ptr<float>(y);
        uchar* gp = g8.ptr<uchar>(y);
        uchar* ip = invalid.ptr<uchar>(y);
        for (int x = 0; x < d32.cols; ++x) {
            const float d = dp[x];
            if (!(d > 0.0f)) { ip[x] = 255; continue; }   // 无效先记账，最后统一涂黑
            gp[x] = cv::saturate_cast<uchar>(d * scale);
        }
    }

    cv::Mat bgr;
    cv::applyColorMap(g8, bgr, cv::COLORMAP_JET);
    bgr.setTo(cv::Scalar(0, 0, 0), invalid);  // 无效区域必须纯黑，和"深蓝=最远"区分开
    return bgr;
}

// 深度伪彩：近 = 暖色(红)，远 = 冷色(蓝)。映射用 255*(1 - z/zmax)。
// 同理固定量程，且 max_depth_mm 直接用配置里的有效上限，保证和裁剪口径一致。
cv::Mat StereoPipeline::colorizeDepth(const cv::Mat& depth_mm, int max_depth_mm) {
    if (depth_mm.empty()) return cv::Mat();

    cv::Mat z;
    depth_mm.convertTo(z, CV_32F);  // 兼容 32F(毫米) / 16U 等输入
    const float zmax = static_cast<float>(max_depth_mm > 0 ? max_depth_mm : 12000);

    cv::Mat g8(z.size(), CV_8UC1, cv::Scalar(0));
    cv::Mat invalid = cv::Mat::zeros(z.size(), CV_8UC1);
    for (int y = 0; y < z.rows; ++y) {
        const float* zp = z.ptr<float>(y);
        uchar* gp = g8.ptr<uchar>(y);
        uchar* ip = invalid.ptr<uchar>(y);
        for (int x = 0; x < z.cols; ++x) {
            const float v = zp[x];
            if (!(v > 0.0f)) { ip[x] = 255; continue; }
            const float t = 1.0f - v / zmax;           // 近处 t 大 -> 暖色
            gp[x] = cv::saturate_cast<uchar>(t * 255.0f);
        }
    }

    cv::Mat bgr;
    cv::applyColorMap(g8, bgr, cv::COLORMAP_JET);
    bgr.setTo(cv::Scalar(0, 0, 0), invalid);
    return bgr;
}

// ===========================================================================
//  7) 点云
// ===========================================================================
// 输入必须是"处理尺度"的深度图（毫米）。stride 抽稀：全图 64 万点塞给上层
// 既没人看得过来也白占内存，隔行隔列抽 1/stride 通常就够做可视化/调参。
cv::Mat StereoPipeline::pointCloud(const cv::Mat& depth_mm, int stride) const {
    if (depth_mm.empty() || !calib_.valid) return cv::Mat();
    stride = std::max(1, stride);

    const double s = procIntrinsicScale(calib_, cfg_, prepared_size_);
    const double fx = calib_.fx * s;
    const double fy = calib_.fy * s;
    const double cx = calib_.cx * s;
    const double cy = calib_.cy * s;
    if (!(fx > 0.0) || !(fy > 0.0)) return cv::Mat();

    cv::Mat z;
    depth_mm.convertTo(z, CV_32F);  // 统一到毫米浮点

    const int rows = z.rows, cols = z.cols;
    std::vector<cv::Point3f> pts;
    pts.reserve(static_cast<size_t>((rows / stride + 1) * (cols / stride + 1)));

    for (int y = 0; y < rows; y += stride) {
        const float* zp = z.ptr<float>(y);
        for (int x = 0; x < cols; x += stride) {
            const float mm = zp[x];
            if (!(mm > 0.0f)) continue;         // 无效点直接跳过，不补 (0,0,0) 假点
            const double Z = static_cast<double>(mm) / 1000.0;   // 米
            const double X = (static_cast<double>(x) - cx) * Z / fx;
            const double Y = (static_cast<double>(y) - cy) * Z / fy;
            pts.emplace_back(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
        }
    }

    cv::Mat out(static_cast<int>(pts.size()), 3, CV_32F);
    for (size_t i = 0; i < pts.size(); ++i) {
        float* p = out.ptr<float>(static_cast<int>(i));
        p[0] = pts[i].x;
        p[1] = pts[i].y;
        p[2] = pts[i].z;
    }
    return out;
}

}  // namespace s3d
