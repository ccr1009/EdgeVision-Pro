// ============================================================================
//  gen_synthetic_stereo.cpp —— 合成双目图像 + 已知真值，用来验证整条数学链
//
//  为什么要有这个工具？
//    真实标定板还没做，摄像头基线也是猜的。如果不先做「已知答案」的自检，
//    出来的深度错了你根本分不清是标定错、公式错、还是 SGBM 参数错。
//    做法：按 pinhole 模型**正投影**生成一对左右图，真值深度 = 我写进去的那个数，
//    再让 StereoPipeline 反算回来，误差必须 ≈ 0（浮点极限）。
//
//  场景：三层平面（1.0 m / 2.0 m / 3.0 m 横条），每层用随机点阵纹理
//  用法：
//    ./gen_synthetic_stereo            # 生成 + 自检，打印每层深度误差
//    ./gen_synthetic_stereo --dump     # 额外把 left.png/right.png 存到当前目录
// ============================================================================
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <cmath>

#include "stereo_pipeline.h"

using namespace s3d;

// ---------------------------------------------------------------------------
// 生成一对合成双目图
//   W,H        : 图像尺寸
//   fx, cx, cy : 理想内参（左右相同）
//   baseline_m : 基线（米）
//   layers     : 每个横条(从上到下)对应的真实深度（米）
// 原理：真实场景点 P=(X,Y,Z) 在左图投到 uL = fx*X/Z + cx，右图投到 uR = fx*(X-B)/Z + cx，
//       于是视差 d = uL - uR = fx*B/Z。所以「给定左图 + 每个像素的深度」，
//       右图就是把左图按 d 重采样一次。
// ---------------------------------------------------------------------------
static void makeSynthetic(const cv::Size& S, double fx, double cx, double cy,
                          double baseline_m, const std::vector<float>& layers,
                          cv::Mat& left, cv::Mat& right)
{
    const int W = S.width, H = S.height;

    // 1) 一张足够「有纹理」的左图：随机点 + 平滑，避免 SGBM 在纯色区匹配失败
    cv::Mat tex(H, W, CV_8U);
    cv::RNG rng(12345);
    rng.fill(tex, cv::RNG::UNIFORM, 0, 256);
    cv::GaussianBlur(tex, tex, cv::Size(5, 5), 0);
    // 叠加规则网格（给 SGBM 稳定的低频结构）
    for (int y = 0; y < H; y += 24)
        for (int x = 0; x < W; x += 24)
            cv::circle(tex, cv::Point(x, y), 3, cv::Scalar(220), -1, cv::LINE_AA);

    left = cv::Mat(H, W, CV_8UC3);
    cv::cvtColor(tex, left, cv::COLOR_GRAY2BGR);

    // 2) 右图：逐像素按该行深度求视差，从左图重采样
    right = cv::Mat(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
    const int nLayers = (int)layers.size();
    std::vector<int> rowOfLayer(H, 0);
    for (int y = 0; y < H; ++y)
        rowOfLayer[y] = std::min(nLayers - 1, y * nLayers / H);   // 竖直方向等分横条

    for (int y = 0; y < H; ++y) {
        const float Z = layers[rowOfLayer[y]];
        const double d = fx * baseline_m / Z;                     // 该层的真实视差(像素)
        for (int x = 0; x < W; ++x) {
            // 右图 (x,y) 对应左图 (x + d, y)
            const double sx = x + d;
            if (sx < 0 || sx >= W - 1) continue;
            const int x0 = (int)sx;
            const double frac = sx - x0;
            for (int c = 0; c < 3; ++c) {
                right.at<cv::Vec3b>(y, x)[c] = (uchar)std::lround(
                    (1 - frac) * left.at<cv::Vec3b>(y, x0)[c] +
                    frac       * left.at<cv::Vec3b>(y, x0 + 1)[c]);
            }
        }
    }
    return;
}

static std::vector<float> parseLayers(int argc, char** argv) {
    std::vector<float> v;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--layers" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t p = 0;
            while (p <= s.size()) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                if (q > p) v.push_back((float)atof(s.substr(p, q - p).c_str()));
                p = q + 1;
            }
        }
    }
    if (v.empty()) v = {1.0f, 2.0f, 3.0f};      // 默认三层
    return v;
}

int main(int argc, char** argv) {
    bool dump = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--dump") dump = true;

    const cv::Size S(1280, 720);
    const double H_FOV_DEG = 60.0;
    const double baseline_m = 0.06;                  // 6 cm，接近真实双摄间距
    const double fx = (S.width / 2.0) / std::tan(M_PI * H_FOV_DEG / 360.0);
    const double cx = S.width / 2.0, cy = S.height / 2.0;

    const std::vector<float> layers = parseLayers(argc, argv);

    printf("=== 合成双目自检 ===\n");
    printf("图像 %dx%d, hfov=%.1f deg -> fx=%.2f, 基线=%.0f mm\n",
           S.width, S.height, H_FOV_DEG, fx, baseline_m * 1000.0);
    printf("真值层深: ");
    for (float z : layers) printf("%.2f m ", z);
    printf("\n  对应真值视差: ");
    for (float z : layers) printf("%.2f px ", fx * baseline_m / z);
    printf("\n\n");

    cv::Mat left, right;
    makeSynthetic(S, fx, cx, cy, baseline_m, layers, left, right);
    if (dump) {
        cv::imwrite("synthetic_left.png", left);
        cv::imwrite("synthetic_right.png", right);
        printf("[+] 已存 synthetic_left.png / synthetic_right.png\n");
    }

    // ---- 用未标定的「理想平行双目」初始化流水线 ----
    StereoPipeline pipe;
    std::string err;
    if (!pipe.initIdeal(S, H_FOV_DEG, baseline_m, &err)) {
        printf("[-] initIdeal 失败: %s\n", err.c_str());
        return 1;
    }
    pipe.config().proc_w = 640;
    pipe.config().proc_h = 480;
    pipe.config().num_disp = 128;      // 3m 处视差约 fx*0.06/3 = 19.7px(全分辨率) -> 9.8px@half
    pipe.config().block_size = 5;
    pipe.config().min_depth_mm = 200;
    pipe.config().max_depth_mm = 20000;
    if (!pipe.prepare(S, &err)) { printf("[-] prepare 失败: %s\n", err.c_str()); return 1; }

    printf("[i] calib: fx=%.3f baseline=%.4f m source=%s\n",
           pipe.calib().fx, pipe.calib().baseline_m, pipe.calib().source.c_str());
    printf("[i] 处理尺度 %dx%d  scale=%.3f  fx_proc=%.3f\n\n",
           pipe.config().proc_w, pipe.config().proc_h, pipe.config().scale,
           pipe.calib().fx * pipe.config().scale);

    // ---- 跑一遍完整链路 ----
    cv::Mat rl, rr;
    pipe.rectify(left, right, rl, rr);
    cv::Mat disp16;
    pipe.computeDisparity(pipe.procLeftGray(), pipe.procRightGray(), disp16);
    cv::Mat depth;
    pipe.depthFromDisparity(disp16, depth);

    // ---- 按层核对误差 ----
    const int H = depth.rows, W = depth.cols;
    const int nL = (int)layers.size();
    // ★ 中文不能直接写进 printf 的格式串（会被当成 % 转换符解析），单独 puts
    puts("=== 每层深度误差（只看该横条中间 50% 宽度，避开边缘遮挡）===");
    printf("%-10s %-12s %-14s %-14s %-10s\n", "真值(m)", "中值深度(m)", "绝对误差(mm)", "相对误差", "有效像素%");
    bool all_ok = true;
    for (int i = 0; i < nL; ++i) {
        const int y0 = i * H / nL, y1 = (i + 1) * H / nL;
        std::vector<float> vals;
        int total = 0;
        for (int y = y0 + (y1 - y0) / 4; y < y1 - (y1 - y0) / 4; ++y) {
            for (int x = W / 4; x < W * 3 / 4; ++x) {
                ++total;
                float d = depth.at<float>(y, x);
                if (d > 0 && std::isfinite(d)) vals.push_back(d);
            }
        }
        if (vals.empty()) { printf(" %.2f      无有效像素 ❌\n", layers[i]); all_ok = false; continue; }
        std::sort(vals.begin(), vals.end());
        const float med = vals[vals.size() / 2] / 1000.0f;
        const float err_mm = std::fabs(med - layers[i]) * 1000.0f;
        const float rel = err_mm / (layers[i] * 1000.0f);
        const float ratio = 100.0f * vals.size() / total;
        printf("%-10.2f %-12.4f %-14.2f %-14.4f %-10.1f\n",
               layers[i], med, err_mm, rel, ratio);
        if (err_mm > 100.0f) all_ok = false;      // 10cm 以上算失败
    }

    // ---- 点云自检 ----
    cv::Mat pc = pipe.pointCloud(depth, 8);
    printf("\n点云点数: %d （stride=8 抽稀）\n", pc.rows);

    printf("\n%s\n", all_ok ? "[PASS] 合成数据深度误差全部 < 100mm —— 数学链正确"
                            : "[FAIL] 存在层误差 > 100mm，检查 fx/baseline/scale 换算");

    // ---- 存可视化 ----
    cv::imwrite("synthetic_disp.png", StereoPipeline::colorizeDisparity(disp16, pipe.config().num_disp));
    cv::imwrite("synthetic_depth.png", StereoPipeline::colorizeDepth(depth, pipe.config().max_depth_mm));
    printf("[+] 已存 synthetic_disp.png / synthetic_depth.png\n");
    return all_ok ? 0 : 2;
}
