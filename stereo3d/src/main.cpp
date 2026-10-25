// ============================================================================
//  main.cpp —— 双目 3D / 人脸 3D 定位主程序
//
//  数据流（单线程主循环 + 一个 HTTP 发布线程）：
//    双摄 grab/drop 取一对图
//      -> 立体矫正 remap（拉成共面行对齐）
//      -> 缩到处理分辨率 640x480 + 灰度
//      -> SGBM 半全局匹配出视差
//      -> Z = fx*B/d 出深度图（毫米）
//      -> Haar 人脸检测（处理尺度灰度图）
//      -> 框内深度中位数 -> 反投影成 3D 坐标
//      -> 四宫格拼图（左|右 / 视差|深度 + 人脸框 + HUD）
//      -> JPEG 编码 -> HTTP 线程发布 MJPEG
//
//  编译（板端原生）：
//    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
//  运行：
//    ./build/stereo3d_demo --calib calib/stereo_calib.yml
//    ./build/stereo3d_demo --no-calib      # 未标定兜底，只能看相对深度
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cmath>

#include <unistd.h>
#include <opencv2/opencv.hpp>

#include "stereo_types.h"
#include "stereo_pipeline.h"
#include "face_3d.h"
#include "http_server.h"
#include "app_options.h"

using namespace s3d;
using Clock = std::chrono::steady_clock;

static volatile sig_atomic_t g_running = 1;
static void onSignal(int) { g_running = 0; }

static inline double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ===========================================================================
//  命令行
// ===========================================================================
namespace s3d {

void printHelp(const char* prog) {
    printf(
        "双目 3D / 人脸 3D 定位 demo (RK3588)\n"
        "\n"
        "用法: %s [选项]\n"
        "\n"
        "摄像头:\n"
        "  --left  DEV        左目设备          (默认 /dev/video81)\n"
        "  --right DEV        右目设备          (默认 /dev/video83)\n"
        "  --cap-w N          采集宽            (默认 1280)\n"
        "  --cap-h N          采集高            (默认 720)\n"
        "  --cap-fps N        采集帧率          (默认 30)\n"
        "\n"
        "标定:\n"
        "  --calib FILE       标定文件 stereo_calib.yml\n"
        "  --no-calib         未标定模式：理想平行双目兜底（只能看相对深度）\n"
        "  --hfov DEG         未标定模式下的水平视场角 (默认 60)\n"
        "  --baseline M       未标定模式下的基线米数 (默认 0.06)\n"
        "\n"
        "立体匹配:\n"
        "  --proc-w N         处理分辨率宽      (默认 640)\n"
        "  --proc-h N         处理分辨率高      (默认 480)\n"
        "  --num-disp N       视差搜索范围，须为16倍数 (默认 64)\n"
        "  --block-size N     SGBM 块大小，奇数 (默认 5)\n"
        "  --min-depth MM     有效深度下限毫米   (默认 250)\n"
        "  --max-depth MM     有效深度上限毫米   (默认 12000)\n"
        "\n"
        "人脸:\n"
        "  --cascade FILE     Haar 级联 xml\n"
        "  --no-face          关闭人脸检测（纯深度图模式）\n"
        "\n"
        "服务:\n"
        "  --port N           HTTP 端口         (默认 8081)\n"
        "  --save-dir DIR     存图目录（配合 --save）\n"
        "  --save             存图调试\n"
        "  --help             显示本帮助\n",
        prog);
}

bool parseArgs(int argc, char** argv, AppOptions& opt, std::string& err, std::string& help) {
    bool no_calib = false, no_face = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { err = std::string("参数 ") + name + " 缺少取值"; return nullptr; }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { help = a; printHelp(argv[0]); return false; }
        else if (a == "--left")    { auto v = need("--left");    if (!v) return false; opt.dev_left = v; }
        else if (a == "--right")   { auto v = need("--right");   if (!v) return false; opt.dev_right = v; }
        else if (a == "--cap-w")   { auto v = need("--cap-w");   if (!v) return false; opt.cap_w = atoi(v); }
        else if (a == "--cap-h")   { auto v = need("--cap-h");   if (!v) return false; opt.cap_h = atoi(v); }
        else if (a == "--cap-fps") { auto v = need("--cap-fps"); if (!v) return false; opt.cap_fps = atoi(v); }
        else if (a == "--calib")   { auto v = need("--calib");   if (!v) return false; opt.calib_yml = v; }
        else if (a == "--no-calib"){ no_calib = true; }
        else if (a == "--hfov")    { auto v = need("--hfov");    if (!v) return false; opt.hfov_deg = atof(v); }
        else if (a == "--baseline"){ auto v = need("--baseline"); if (!v) return false; opt.baseline_m = atof(v); }
        else if (a == "--proc-w")  { auto v = need("--proc-w");  if (!v) return false; opt.stereo.proc_w = atoi(v); }
        else if (a == "--proc-h")  { auto v = need("--proc-h");  if (!v) return false; opt.stereo.proc_h = atoi(v); }
        else if (a == "--num-disp"){ auto v = need("--num-disp"); if (!v) return false; opt.stereo.num_disp = atoi(v); }
        else if (a == "--block-size"){ auto v = need("--block-size"); if (!v) return false; opt.stereo.block_size = atoi(v); }
        else if (a == "--min-depth"){ auto v = need("--min-depth"); if (!v) return false; opt.stereo.min_depth_mm = atoi(v); }
        else if (a == "--max-depth"){ auto v = need("--max-depth"); if (!v) return false; opt.stereo.max_depth_mm = atoi(v); }
        else if (a == "--cascade") { auto v = need("--cascade"); if (!v) return false; opt.cascade_xml = v; }
        else if (a == "--no-face") { no_face = true; }
        else if (a == "--port")    { auto v = need("--port");    if (!v) return false; opt.port = atoi(v); }
        else if (a == "--save")    { opt.save_frames = true; }
        else if (a == "--save-dir"){ auto v = need("--save-dir"); if (!v) return false; opt.save_dir = v; }
        else { err = "未知参数: " + a; return false; }
    }

    // --no-calib：强制走未标定分支（忽略 --calib）
    if (no_calib) opt.calib_yml.clear();
    // --no-face：给个空 cascade 路径
    if (no_face) opt.cascade_xml.clear();

    // 参数合法性
    if (opt.stereo.num_disp % 16 != 0) {
        err = "--num-disp 必须是 16 的倍数（SGBM 要求）";
        return false;
    }
    if (opt.stereo.block_size % 2 == 0) {
        err = "--block-size 必须是奇数（SGBM 要求）";
        return false;
    }
    if (opt.port <= 0 || opt.port > 65535) { err = "--port 非法"; return false; }
    if (opt.cap_w <= 0 || opt.cap_h <= 0) { err = "采集分辨率非法"; return false; }
    if (opt.stereo.proc_w <= 0 || opt.stereo.proc_h <= 0) { err = "处理分辨率非法"; return false; }
    return true;
}

}  // namespace s3d

// ===========================================================================
//  四宫格拼图
// ===========================================================================
// 左上 = 左目（画人脸框），右上 = 右目（画同样的框，直观展示极线对齐后 y 一致），
// 左下 = 伪彩视差图，右下 = 伪彩深度图。
// 每个格子加角标，方便截图/面试时讲。
static void putTag(cv::Mat& img, const std::string& text, const cv::Scalar& color) {
    const int pad = 8, h = 26;
    cv::rectangle(img, cv::Rect(0, 0, img.cols, h), cv::Scalar(20, 20, 20), cv::FILLED);
    cv::putText(img, text, cv::Point(pad, h - 8), cv::FONT_HERSHEY_SIMPLEX, 0.55, color, 1, cv::LINE_AA);
}

static cv::Mat composeGrid(const cv::Mat& left, const cv::Mat& right,
                           const cv::Mat& dispViz, const cv::Mat& depthViz,
                           const cv::Mat& hud) {
    cv::Mat tl, tr, bl, br;
    cv::resize(left,     tl, cv::Size(640, 360));
    cv::resize(right,    tr, cv::Size(640, 360));
    cv::resize(dispViz,  bl, cv::Size(640, 360));
    cv::resize(depthViz, br, cv::Size(640, 360));
    putTag(tl, "LEFT (rectified)", cv::Scalar(120, 230, 120));
    putTag(tr, "RIGHT (rectified)", cv::Scalar(120, 200, 255));
    putTag(bl, "DISPARITY (SGBM)", cv::Scalar(200, 220, 120));
    putTag(br, "DEPTH (Z = fx*B/d)", cv::Scalar(255, 180, 120));

    cv::Mat top, bot, grid;
    cv::hconcat(tl, tr, top);
    cv::hconcat(bl, br, bot);
    cv::vconcat(top, bot, grid);

    // HUD 叠在左上格下方（避开角标）
    if (!hud.empty()) {
        cv::Mat roi = grid(cv::Rect(8, 30, hud.cols, hud.rows));
        hud.copyTo(roi);
    }
    return grid;
}

// ===========================================================================
//  HUD：把关键数字画到图左上角（面试截图就靠它）
// ===========================================================================
static cv::Mat buildHud(const FrameStats& st, const StereoCalib& cal,
                        const StereoConfig& cfg, int port) {
    const int W = 430;
    const int lineH = 20;
    std::vector<std::string> lines;

    char buf[256];
    snprintf(buf, sizeof(buf), "FPS %.1f   frame %d   faces %d",
             st.fps, st.frame_id, st.n_faces);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "cap %.1f  rect %.1f  sgbm %.1f  depth %.1f",
             st.cap_ms, st.rect_ms, st.sgbm_ms, st.depth_ms);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "face %.1f  draw %.1f  enc %.1f",
             st.face_ms, st.draw_ms, st.encode_ms);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "fx %.1f  B %.1fmm  proc %dx%d nd%d",
             cal.fx, cal.baseline_m * 1000.0, cfg.proc_w, cfg.proc_h, cfg.num_disp);
    lines.push_back(buf);
    if (cal.source.find("SYNTHETIC") != std::string::npos) {
        lines.push_back("!! UNCALIBRATED - relative depth only !!");
    } else {
        snprintf(buf, sizeof(buf), "calib RMS %.3f px  (absolute depth valid)", cal.rms_reproj);
        lines.push_back(buf);
    }
    snprintf(buf, sizeof(buf), "http://<board-ip>:%d", port);
    lines.push_back(buf);

    cv::Mat hud(lines.size() * lineH + 10, W, CV_8UC3, cv::Scalar(15, 18, 26));
    for (size_t i = 0; i < lines.size(); ++i) {
        cv::Scalar c(235, 235, 235);
        if (lines[i].find("UNCALIBRATED") != std::string::npos)
            c = cv::Scalar(80, 80, 255);                 // BGR 红
        else if (i == 0)
            c = cv::Scalar(120, 230, 120);               // 首行绿
        cv::putText(hud, lines[i], cv::Point(10, 20 + (int)i * lineH),
                    cv::FONT_HERSHEY_SIMPLEX, 0.48, c, 1, cv::LINE_AA);
    }
    return hud;
}

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char** argv) {
    AppOptions opt;
    std::string err, help;
    if (!parseArgs(argc, argv, opt, err, help)) {
        if (!err.empty()) { fprintf(stderr, "参数错误: %s\n\n", err.c_str()); printHelp(argv[0]); return 2; }
        return 0;   // --help
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGPIPE, SIG_IGN);          // 关键：客户端断连不要崩

    printf("==================== stereo3d-rk3588 ====================\n");
    printf("左目 %s   右目 %s   %dx%d @%d\n",
           opt.dev_left.c_str(), opt.dev_right.c_str(), opt.cap_w, opt.cap_h, opt.cap_fps);

    // -------------------------------------------------------------------
    // 1. 打开双摄
    // -------------------------------------------------------------------
    cv::VideoCapture capL(opt.dev_left, cv::CAP_V4L2);
    cv::VideoCapture capR(opt.dev_right, cv::CAP_V4L2);
    if (!capL.isOpened()) { fprintf(stderr, "[-] 打不开左目 %s\n", opt.dev_left.c_str()); return 1; }
    if (!capR.isOpened()) { fprintf(stderr, "[-] 打不开右目 %s\n", opt.dev_right.c_str()); return 1; }
    for (auto* c : {&capL, &capR}) {
        c->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        c->set(cv::CAP_PROP_FRAME_WIDTH, opt.cap_w);
        c->set(cv::CAP_PROP_FRAME_HEIGHT, opt.cap_h);
        c->set(cv::CAP_PROP_FPS, opt.cap_fps);
        c->set(cv::CAP_PROP_BUFFERSIZE, 1);
    }

    // 预热：AE/AWB 需要约 15 帧才收敛（之前项目踩过的坑）
    cv::Mat warmL, warmR;
    for (int i = 0; i < 20; ++i) { capL >> warmL; capR >> warmR; }
    if (warmL.empty() || warmR.empty()) { fprintf(stderr, "[-] 预热取帧失败\n"); return 1; }
    const cv::Size actual(warmL.cols, warmL.rows);
    printf("[+] 实际采集分辨率 %dx%d（右目 %dx%d）\n",
           warmL.cols, warmL.rows, warmR.cols, warmR.rows);
    if (warmR.cols != warmL.cols || warmR.rows != warmL.rows) {
        fprintf(stderr, "[-] 左右分辨率不一致，无法做立体匹配\n");
        return 1;
    }

    // -------------------------------------------------------------------
    // 2. 初始化立体流水线（标定 or 未标定兜底）
    // -------------------------------------------------------------------
    StereoPipeline pipe;
    if (!opt.calib_yml.empty()) {
        printf("[i] 载入标定 %s\n", opt.calib_yml.c_str());
        if (!pipe.loadCalib(opt.calib_yml, &err)) {
            fprintf(stderr, "[-] 标定文件载入失败: %s\n", err.c_str());
            fprintf(stderr, "    先用 --no-calib 跑，或按 docs/CALIB_FORMAT.md 检查文件\n");
            return 1;
        }
        printf("[+] 标定 RMS=%.3f px  fx=%.2f  B=%.4f m  size=%dx%d\n",
               pipe.calib().rms_reproj, pipe.calib().fx,
               pipe.calib().baseline_m, pipe.calib().image_size.width,
               pipe.calib().image_size.height);
    } else {
        printf("[!] 未标定模式：用理想平行双目假设（hfov=%.1f°, B=%.3f m）\n",
               opt.hfov_deg, opt.baseline_m);
        printf("    -> 只能看相对深度，绝对距离不可信\n");
        if (!pipe.initIdeal(actual, opt.hfov_deg, opt.baseline_m, &err)) {
            fprintf(stderr, "[-] initIdeal 失败: %s\n", err.c_str());
            return 1;
        }
    }
    pipe.config() = opt.stereo;
    if (!pipe.prepare(actual, &err)) {
        fprintf(stderr, "[-] prepare 失败: %s\n", err.c_str());
        return 1;
    }
    printf("[i] 处理尺度 %dx%d  scale=%.4f  fx_proc=%.2f  nd=%d\n\n",
           pipe.config().proc_w, pipe.config().proc_h, pipe.config().scale,
           pipe.calib().fx * pipe.config().scale, pipe.config().num_disp);

    // -------------------------------------------------------------------
    // 3. 人脸检测器
    // -------------------------------------------------------------------
    Face3DLocator face;
    if (!opt.cascade_xml.empty()) {
        if (!face.loadCascade(opt.cascade_xml, &err))
            printf("[!] Haar 级联载入失败(%s)，降级为纯深度模式\n", err.c_str());
        else
            printf("[+] Haar 人脸检测已启用: %s\n", opt.cascade_xml.c_str());
    } else {
        printf("[i] 人脸检测已关闭（--no-face）\n");
    }

    // -------------------------------------------------------------------
    // 4. HTTP 预览
    // -------------------------------------------------------------------
    HttpPreview http(opt.port, "stereo3d-rk3588");
    if (!http.start()) {
        fprintf(stderr, "[-] HTTP 监听 %d 失败（端口被占用？）\n", opt.port);
        return 1;
    }

    // -------------------------------------------------------------------
    // 5. 主循环
    // -------------------------------------------------------------------
    FrameStats st;
    std::vector<Face3D> faces;
    std::vector<cv::Rect> faceRects;
    cv::Mat L, R, rectL, rectR, disp16, depth;
    cv::Mat viewL, viewR, dispViz, depthViz, grid, hud;
    std::vector<uint8_t> jpeg;
    std::vector<uchar> buf;

    auto tStart = Clock::now();
    int nFrames = 0;
    double fpsAccum = 0.0;

    while (g_running) {
        // ---- 采集：先各 grab 再各 retrieve，把两颗摄像头的时差压到最小 ----
        auto t0 = Clock::now();
        if (!capL.grab()) { usleep(10000); continue; }
        capR.grab();
        if (!capL.retrieve(L)) { usleep(10000); continue; }
        capR.retrieve(R);
        if (L.empty() || R.empty()) { usleep(10000); continue; }
        st.cap_ms = msSince(t0);

        // ---- 矫正 ----
        t0 = Clock::now();
        pipe.rectify(L, R, rectL, rectR);
        st.rect_ms = msSince(t0);

        // ---- 视差 ----
        t0 = Clock::now();
        pipe.computeDisparity(pipe.procLeftGray(), pipe.procRightGray(), disp16);
        st.sgbm_ms = msSince(t0);

        // ---- 深度 ----
        t0 = Clock::now();
        pipe.depthFromDisparity(disp16, depth);
        st.depth_ms = msSince(t0);

        // ---- 人脸检测 + 3D 定位 ----
        t0 = Clock::now();
        faces.clear();
        if (face.ready()) {
            face.detectAndLocate(pipe.procLeftGray(), disp16, depth,
                                 pipe.calib(), pipe.config(), faces);
        }
        st.face_ms = msSince(t0);
        st.n_faces = (int)faces.size();

        // ---- 画图 ----
        t0 = Clock::now();
        // ★ rectify() 返回的是【单通道灰度】矫正图（SGBM 只要灰度，省 3 倍访存）。
        //   画框/拼图需要 BGR，这里统一转一次，否则 hconcat 拼 1 通道和 3 通道会抛
        //   "src[i].type() == src[0].type()" 断言。
        cv::cvtColor(rectL, viewL, cv::COLOR_GRAY2BGR);
        cv::cvtColor(rectR, viewR, cv::COLOR_GRAY2BGR);
        // 右图上用「左图检测框 - 视差」画同样的框：矫正成功的话 y 应该完全对齐，
        // 这是肉眼验证极线校正最直观的办法
        for (const auto& f : faces) {
            cv::Rect rb = f.box_full;
            const float d = (f.depth_median_mm > 0)
                ? (float)(pipe.calib().fx * pipe.calib().baseline_m * 1000.0 / f.depth_median_mm)
                : 0.0f;
            rb.x -= (int)std::lround(d);
            if (rb.x >= 0 && rb.x + rb.width < viewR.cols)
                cv::rectangle(viewR, rb, cv::Scalar(0, 200, 255), 2);
        }
        // 传上一帧的 EMA FPS（本帧还没算完，用历史值是诚实的，不用 0 占位）
        Face3DLocator::draw(viewL, faces, pipe.config(), st.fps);
        dispViz  = StereoPipeline::colorizeDisparity(disp16, pipe.config().num_disp);
        depthViz = StereoPipeline::colorizeDepth(depth, pipe.config().max_depth_mm);

        st.draw_ms = msSince(t0);

        // FPS 用指数滑动平均，别用瞬时值（闪烁难看）
        const double inst = 1000.0 / std::max(1e-3, st.cap_ms + st.rect_ms +
                                                     st.sgbm_ms + st.depth_ms +
                                                     st.face_ms + st.draw_ms);
        st.fps = (nFrames == 0) ? inst : (st.fps * 0.9 + inst * 0.1);
        fpsAccum += inst;

        hud = buildHud(st, pipe.calib(), pipe.config(), opt.port);
        grid = composeGrid(viewL, viewR, dispViz, depthViz, hud);

        // ---- 编码 + 发布 ----
        t0 = Clock::now();
        cv::imencode(".jpg", grid, buf, {cv::IMWRITE_JPEG_QUALITY, 80});
        jpeg.assign(buf.begin(), buf.end());
        st.encode_ms = msSince(t0);

        char stats[512];
        snprintf(stats, sizeof(stats),
                 "{\"fps\":%.1f,\"frame\":%d,\"faces\":%d,"
                 "\"cap_ms\":%.2f,\"rect_ms\":%.2f,\"sgbm_ms\":%.2f,"
                 "\"depth_ms\":%.2f,\"face_ms\":%.2f,\"draw_ms\":%.2f,\"encode_ms\":%.2f,"
                 "\"fx\":%.2f,\"baseline_m\":%.4f,\"calibrated\":%s}",
                 st.fps, st.frame_id, st.n_faces, st.cap_ms, st.rect_ms, st.sgbm_ms,
                 st.depth_ms, st.face_ms, st.draw_ms, st.encode_ms,
                 pipe.calib().fx, pipe.calib().baseline_m,
                 (pipe.calib().source.find("SYNTHETIC") == std::string::npos) ? "true" : "false");
        http.publish(jpeg, stats);

        // 视差图单独发布（GET /depth），低频一点也行，这里每 5 帧一次省 CPU
        if (st.frame_id % 5 == 0) {
            std::vector<uchar> dbuf;
            cv::imencode(".jpg", depthViz, dbuf, {cv::IMWRITE_JPEG_QUALITY, 75});
            http.publishDepth(std::vector<uint8_t>(dbuf.begin(), dbuf.end()));
        }

        // ---- 控制台 + 存图 ----
        st.frame_id++;
        nFrames++;
        if (st.frame_id % 30 == 0) {
            printf("[%4d] FPS %5.1f | cap %4.1f rect %4.1f sgbm %5.1f depth %4.1f "
                   "face %4.1f enc %4.1f | faces %d\n",
                   st.frame_id, st.fps, st.cap_ms, st.rect_ms, st.sgbm_ms,
                   st.depth_ms, st.face_ms, st.encode_ms, st.n_faces);
            for (const auto& f : faces)
                printf("        face @(%d,%d %dx%d)  Z=%.3f m  XYZ=(%.3f, %.3f, %.3f) m  "
                       "valid %.0f%% (%d px)\n",
                       f.box_full.x, f.box_full.y, f.box_full.width, f.box_full.height,
                       f.distance_m, f.center_m.x, f.center_m.y, f.center_m.z,
                       f.valid_ratio * 100.0f, f.valid_pixels);
            fflush(stdout);
        }

        if (opt.save_frames && st.frame_id % 60 == 0) {
            char fn[512];
            snprintf(fn, sizeof(fn), "%s/grid_%04d.jpg", opt.save_dir.c_str(), st.frame_id);
            cv::imwrite(fn, grid);
            snprintf(fn, sizeof(fn), "%s/depth_%04d.png", opt.save_dir.c_str(), st.frame_id);
            cv::imwrite(fn, depth);
        }
    }

    // -------------------------------------------------------------------
    // 6. 收尾
    // -------------------------------------------------------------------
    printf("\n[i] 收到退出信号，正在关闭...\n");
    http.stop();
    capL.release();
    capR.release();

    const double total_s = std::chrono::duration<double>(Clock::now() - tStart).count();
    printf("[+] 共处理 %d 帧，用时 %.1f s，平均 %.2f FPS\n",
           nFrames, total_s, nFrames / std::max(1e-6, total_s));
    (void)fpsAccum;
    return 0;
}
