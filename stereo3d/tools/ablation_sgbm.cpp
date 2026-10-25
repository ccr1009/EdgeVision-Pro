// ============================================================================
//  ablation_sgbm.cpp —— SGBM 参数消融实测工具 (task-5 第二阶段)
//
//  【目的】在真实双摄上实测 6 组参数 (num_disp 64/48/32 × block 5/7) 的
//          sgbm_ms / 全链路 FPS / 有效视差占比，找速度/质量拐点。
//
//  【为什么单独写工具而不是用 stereo3d_demo】
//    demo 的 FPS 是 EMA、sgbm 是瞬时值、且不输出全图有效视差占比。
//    消融要的是"每组 200 帧的稳态统计"，逐组重启进程最干净 ——
//    避免上一组的 OpenCV 分配器缓存/热态污染下一组测量。
//    本工具复用 StereoPipeline（与 demo 同一条代码路径），只省去 HTTP 发布
//    和人脸检测的干扰可选项；sgbm_ms 与 demo 的 46.1ms 口径可比。
//
//  【用法】ablation_sgbm --num-disp 64 --block-size 5 --frames 200 [--snapshot /path.png]
//          参数由外层 shell 脚本逐组传，环境变量 STEREO_NUM_DISP/STEREO_BLOCK
//          也可覆盖（stereo_pipeline.cpp 里 getenv 入口），命令行优先级更高。
//
//  【精度粗验口径】有效视差占比 = (disp > 0) 像素数 / 总像素。
//    disp16 是定点（像素 x16），SGBM 无匹配/遮挡像素被 toDisp16 归 0，
//    左右一致性检查 + 唯一性Ratio 不过的也会输出 0 或负值，所以 >0 即有效。
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <chrono>
#include <string>
#include <vector>

#include <unistd.h>
#include <opencv2/opencv.hpp>

#include "stereo_pipeline.h"

using namespace s3d;
using Clock = std::chrono::steady_clock;

static volatile sig_atomic_t g_running = 1;
static void onSignal(int) { g_running = 0; }

static inline double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
static inline double msBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// 统计视差图有效占比：disp16 CV_16S 定点（像素x16），>0 即有效
static double validRatio(const cv::Mat& disp16) {
    if (disp16.empty()) return 0.0;
    const int total = disp16.rows * disp16.cols;
    const int valid = cv::countNonZero(disp16 > 0);
    return (total > 0) ? (double)valid / (double)total : 0.0;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string devL = "/dev/video81", devR = "/dev/video83";
    int num_disp = 64, block_size = 5, n_frames = 200;
    int proc_w = 640;
    std::string snapshot_path;   // 非空则收尾时存一张伪彩视差图

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "参数 %s 缺少取值\n", name); exit(2); }
            return argv[++i];
        };
        if      (a == "--left")      devL = need("--left");
        else if (a == "--right")     devR = need("--right");
        else if (a == "--num-disp")  num_disp = atoi(need("--num-disp"));
        else if (a == "--block-size")block_size = atoi(need("--block-size"));
        else if (a == "--frames")    n_frames = atoi(need("--frames"));
        else if (a == "--proc-w")    proc_w = atoi(need("--proc-w"));
        else if (a == "--snapshot")  snapshot_path = need("--snapshot");
        else { fprintf(stderr, "未知参数: %s\n", a.c_str()); return 2; }
    }

    printf("==== ablation: num_disp=%d block=%d frames=%d proc_w=%d ====\n",
           num_disp, block_size, n_frames, proc_w);
    fflush(stdout);

    // ---- 打开双摄（与 main.cpp 完全同参：MJPG 1280x720@30, bufsz=1）----
    cv::VideoCapture capL(devL, cv::CAP_V4L2);
    cv::VideoCapture capR(devR, cv::CAP_V4L2);
    if (!capL.isOpened() || !capR.isOpened()) {
        fprintf(stderr, "[-] 打不开双摄 %s / %s\n", devL.c_str(), devR.c_str());
        return 1;
    }
    for (auto* c : {&capL, &capR}) {
        c->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        c->set(cv::CAP_PROP_FRAME_WIDTH, 1280);
        c->set(cv::CAP_PROP_FRAME_HEIGHT, 720);
        c->set(cv::CAP_PROP_FPS, 30);
        c->set(cv::CAP_PROP_BUFFERSIZE, 1);
    }

    // 预热 20 帧（AE 收敛，与 demo 相同口径）
    cv::Mat wL, wR;
    for (int i = 0; i < 20; ++i) { capL >> wL; capR >> wR; }
    if (wL.empty() || wR.empty()) { fprintf(stderr, "[-] 预热取帧失败\n"); return 1; }
    if (wL.cols != wR.cols || wL.rows != wR.rows) {
        fprintf(stderr, "[-] 左右分辨率不一致\n"); return 1;
    }

    // ---- 流水线初始化（未标定模式，与当前 demo 运行方式一致）----
    StereoPipeline pipe;
    std::string err;
    if (!pipe.initIdeal(cv::Size(wL.cols, wL.rows), 60.0, 0.06, &err)) {
        fprintf(stderr, "[-] initIdeal: %s\n", err.c_str()); return 1;
    }
    StereoConfig& cfg = pipe.config();
    cfg.proc_w = proc_w;
    cfg.proc_h = 360;           // 16:9 等比，与 demo 现状 640x360 一致
    cfg.num_disp = num_disp;
    cfg.block_size = block_size;
    if (!pipe.prepare(cv::Size(wL.cols, wL.rows), &err)) {
        fprintf(stderr, "[-] prepare: %s\n", err.c_str()); return 1;
    }
    printf("[i] proc %dx%d  nd=%d  block=%d\n",
           cfg.proc_w, cfg.proc_h, cfg.num_disp, cfg.block_size);
    fflush(stdout);

    // ---- 主循环：与 demo 同序（cap/rect/sgbm/depth），统计稳态 ----
    cv::Mat L, R, rectL, rectR, disp16, depth;
    double sgbm_acc = 0, rect_acc = 0, depth_acc = 0, e2e_acc = 0;
    double ratio_acc = 0;
    int done = 0;

    const auto t_all0 = Clock::now();
    while (g_running && done < n_frames) {
        const auto t0 = Clock::now();
        if (!capL.grab()) { usleep(10000); continue; }
        capR.grab();
        if (!capL.retrieve(L)) { usleep(10000); continue; }
        capR.retrieve(R);
        if (L.empty() || R.empty()) { usleep(10000); continue; }
        const auto t_cap = Clock::now();

        pipe.rectify(L, R, rectL, rectR);
        const auto t_rect = Clock::now();

        pipe.computeDisparity(pipe.procLeftGray(), pipe.procRightGray(), disp16);
        const auto t_sgbm = Clock::now();

        pipe.depthFromDisparity(disp16, depth);
        const auto t_depth = Clock::now();

        if (disp16.empty()) { fprintf(stderr, "[-] disp empty\n"); break; }

        rect_acc  += msBetween(t_cap, t_rect);
        sgbm_acc  += msBetween(t_rect, t_sgbm);
        depth_acc += msBetween(t_sgbm, t_depth);
        e2e_acc   += msBetween(t0, t_depth);
        ratio_acc += validRatio(disp16);
        ++done;

        // 最后一帧存 snapshot（伪彩视差图，供人工/视觉复核）
        if (done == n_frames && !snapshot_path.empty()) {
            cv::Mat viz = StereoPipeline::colorizeDisparity(disp16, cfg.num_disp);
            if (!viz.empty() && cv::imwrite(snapshot_path, viz))
                printf("[i] snapshot -> %s\n", snapshot_path.c_str());
        }
        if (done % 50 == 0) { printf("  ..%d\n", done); fflush(stdout); }
    }
    const double total_s = std::chrono::duration<double>(Clock::now() - t_all0).count();

    capL.release();
    capR.release();

    if (done == 0) { fprintf(stderr, "[-] 一帧都没跑完\n"); return 1; }

    // ---- 汇总输出（机器可解析的 RESULT 行 + 人类可读）----
    printf("\n");
    printf("RESULT nd=%d block=%d frames=%d ", num_disp, block_size, done);
    printf("sgbm_ms=%.2f rect_ms=%.2f depth_ms=%.2f e2e_ms=%.2f fps=%.2f valid_ratio=%.4f\n",
           sgbm_acc / done, rect_acc / done, depth_acc / done, e2e_acc / done,
           done / std::max(1e-6, total_s), ratio_acc / done);
    printf("    (sgbm_ms = 每帧 SGBM 平均; fps = 端到端含采集/矫正/深度, 不含 HTTP/人脸)\n");
    fflush(stdout);
    return 0;
}
