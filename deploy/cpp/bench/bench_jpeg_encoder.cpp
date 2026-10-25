// ============================================================================
//  bench_jpeg_encoder.cpp —— MPP 硬件 JPEG 编码器 vs OpenCV 软编 基准测试
//
//  测什么:
//    1. 正确性: 硬编 -> cv::imdecode 解回 -> Y/BGR PSNR。用真实实拍图
//       (data/calib_v2/real_*.jpg, 板端双摄实拍), 不用合成图 —— 合成图
//       PSNR 虚高 (项目里踩过, 见 AGENTS.md 工作约定第 3 条)。
//    2. 单 context 速度: 硬编 vs cv::imencode, N=200, 跑 2 轮看稳定性。
//       mean/p50/p95/p99 四指标, 与 bench_dflfree.cpp 同口径。
//    3. 多 context 并行: 单 context 只占 1 个 jpege 核, 硬编要提吞吐必须开多个。
//       同时用 /proc/interrupts 验证中断确实分摊到 4 个核。
//    4. CPU 占用: 硬编的真正优势 (卸载 CPU), 用 getrusage 量 wall vs cpu。
//    5. 分辨率-延迟曲线: 区分"算力"与"固定往返延迟"。
//
//  编译 (板上):
//    g++ -O2 -std=c++17 -pthread bench_jpeg_encoder.cpp ../src/mpp_jpeg_encoder.cpp
//        -I../include -I/root/edgevision/third_party/mpp/include
//        -L/root/edgevision/third_party/mpp/lib -lrockchip_mpp
//        -Wl,-rpath,/root/edgevision/third_party/mpp/lib
//        -I/usr/include/opencv4 -lopencv_core -lopencv_imgcodecs -lopencv_imgproc
//        -o bench_jpeg_encoder
//    (上面一条命令行, 这里为可读性折行; 实际执行见 _run_bench_jpeg_encoder.sh)
//
//  运行:
//    ./bench_jpeg_encoder /root/edgevision/data/calib_v2 200
//
//  不要跑这个 bench 时开 preview/gateway —— 会抢摄像头与 CPU, 数字不稳。
// ============================================================================
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "mpp_jpeg_encoder.h"
#include "mpp_log.h"

namespace {

constexpr int kSoftQuality = 80;  // 软编基线口径

double now_ms() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

// 进程 CPU 时间 (ms), 含所有线程
double cpu_ms() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
           (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

// 与 bench_dflfree.cpp 一致的百分位口径
double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::ceil(p / 100.0 * v.size())) - 1;
    return v[std::min(i, v.size() - 1)];
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / v.size();
}

// 与 bench_dflfree.cpp 同款输出行
void print_stats(const char* tag, std::vector<double>& v) {
    if (v.empty()) return;
    double m = mean(v);
    printf("  %-24s mean=%7.2f  p50=%7.2f  p95=%7.2f  p99=%7.2f  FPS=%6.1f\n",
           tag, m, pct(v, 50), pct(v, 95), pct(v, 99), m > 0 ? 1000.0 / m : 0.0);
}

double psnr_1ch(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat d;
    cv::absdiff(a, b, d);
    d.convertTo(d, CV_32F);
    double mse = cv::sum(d.mul(d))[0] / static_cast<double>(a.total());
    if (mse <= 1e-12) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

double psnr_bgr(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat d;
    cv::absdiff(a, b, d);
    d.convertTo(d, CV_32F);
    double mse = cv::sum(d.mul(d))[0] /
                 static_cast<double>(a.total() * a.channels());
    if (mse <= 1e-12) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// 读 /proc/interrupts 里 jpeg 相关中断计数
// 行形如: " 42:  1270  0  0 ... GICv3  44 Level  fdba0000.jpege-core"
std::vector<std::pair<std::string, long long>> read_jpeg_irq() {
    std::vector<std::pair<std::string, long long>> out;
    std::ifstream f("/proc/interrupts");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("jpeg") == std::string::npos) continue;
        std::vector<std::string> tk;
        std::istringstream iss(line);
        std::string t;
        while (iss >> t) tk.push_back(t);
        if (tk.size() < 3) continue;
        long long sum = 0;
        for (size_t k = 1; k < tk.size(); ++k) {
            bool alldigit = !tk[k].empty();
            for (char c : tk[k]) {
                if (c < '0' || c > '9') { alldigit = false; break; }
            }
            if (!alldigit) break;  // 遇到 GICv3 之类就停, 否则死循环
            sum += atoll(tk[k].c_str());
        }
        out.emplace_back(tk.back(), sum);
    }
    return out;
}

void print_irq_diff(const std::vector<std::pair<std::string, long long>>& a,
                    const std::vector<std::pair<std::string, long long>>& b,
                    int frames) {
    for (size_t i = 0; i < a.size(); ++i) {
        long long v0 = a[i].second;
        long long v1 = (i < b.size() && b[i].first == a[i].first) ? b[i].second : 0;
        long long d = v1 - v0;
        if (d == 0) continue;
        printf("       %-24s +%-6lld (%.2f 次/帧)\n", a[i].first.c_str(), d,
               frames > 0 ? static_cast<double>(d) / frames : 0.0);
    }
}

// 从目录里取真实实拍图 real_*.jpg (按文件名排序, 最多 max_n 张)
std::vector<std::string> collect_real_images(const std::string& dir, int max_n) {
    std::vector<std::string> out;
    std::string cmd = "ls " + dir + "/real_*.jpg 2>/dev/null | sort";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) out.push_back(s);
        if (static_cast<int>(out.size()) >= max_n) break;
    }
    pclose(p);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const std::string img_dir =
        argc > 1 ? argv[1] : "/root/edgevision/data/calib_v2";
    const int N = argc > 2 ? atoi(argv[2]) : 200;

    mpp_set_log_level(MPP_LOG_FATAL);  // 关掉 "invalid qf_min 0" 刷屏

    printf("============================================================\n");
    printf(" MPP 硬件 JPEG 编码器基准测试\n");
    printf(" 板卡 EVM3588-B / Debian11 / 内核 5.10.110 / MPP 源码编译版\n");
    printf(" 图目录 %s\n", img_dir.c_str());
    printf(" N=%d  软编基线 cv::imencode(quality=%d)\n", N, kSoftQuality);
    printf("============================================================\n\n");

    // ================= 1. 正确性: 硬编 -> 软解 -> PSNR =================
    printf("=== 1. 正确性 (硬编 qf80 -> cv::imdecode 解回 -> PSNR) ===\n");
    {
        auto files = collect_real_images(img_dir, 9);
        if (files.empty()) {
            printf("  [-] 找不到 %s/real_*.jpg\n", img_dir.c_str());
            return 1;
        }
        printf("  %-22s %-10s %-9s %-9s %-9s\n",
               "图", "硬编字节", "Y-PSNR", "BGR-PSNR", "软编BGR");

        cv::Mat first_bgr;
        std::vector<uint8_t> first_jpg;
        double sum_y = 0.0, sum_bgr = 0.0, sum_soft = 0.0;
        int ok_cnt = 0, fail_cnt = 0;

        for (const auto& path : files) {
            cv::Mat bgr = cv::imread(path, cv::IMREAD_COLOR);
            if (bgr.empty()) {
                printf("  [-] 读不了: %s\n", path.c_str());
                continue;
            }
            if (first_bgr.empty()) first_bgr = bgr.clone();

            edgevision::MppJpegEncoder enc;
            int r = enc.init(bgr.cols, bgr.rows, edgevision::kDefaultJpegQF);
            if (r) {
                printf("  [-] init 失败 ret=%d (%s)\n", r, enc.last_error().c_str());
                continue;
            }

            // BGR -> full-range NV12
            std::vector<uint8_t> nv12(static_cast<size_t>(enc.hor_stride()) *
                                          enc.ver_stride() * 3 / 2,
                                      0x80);
            edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows,
                                               (int)bgr.step, nv12.data(),
                                               enc.hor_stride(), enc.ver_stride());

            std::vector<uint8_t> hard;
            r = enc.encode(nv12.data(), nv12.size(), &hard);
            if (r || hard.empty()) {
                printf("  [-] encode 失败 ret=%d (%s)\n", r, enc.last_error().c_str());
                ++fail_cnt;
                continue;
            }

            cv::Mat hard_dec = cv::imdecode(hard, cv::IMREAD_COLOR);
            if (hard_dec.empty()) {
                printf("  [-] imdecode 解不了硬编结果 (不是合法 JPEG!)\n");
                ++fail_cnt;
                continue;
            }
            if (first_jpg.empty()) first_jpg = hard;

            // 软编对照
            std::vector<uint8_t> soft;
            cv::imencode(".jpg", bgr, soft, {cv::IMWRITE_JPEG_QUALITY, kSoftQuality});
            cv::Mat soft_dec = cv::imdecode(soft, cv::IMREAD_COLOR);

            cv::Mat g_src, g_hard, g_soft;
            cv::cvtColor(bgr, g_src, cv::COLOR_BGR2GRAY);
            cv::cvtColor(hard_dec, g_hard, cv::COLOR_BGR2GRAY);
            cv::cvtColor(soft_dec, g_soft, cv::COLOR_BGR2GRAY);

            double py = psnr_1ch(g_src, g_hard);
            double pb = psnr_bgr(bgr, hard_dec);
            double ps = psnr_bgr(bgr, soft_dec);
            sum_y += py;
            sum_bgr += pb;
            sum_soft += ps;
            ++ok_cnt;

            std::string base = path.substr(path.find_last_of('/') + 1);
            printf("  %-22s %-10zu %8.2f  %8.2f  %8.2f\n",
                   base.c_str(), hard.size(), py, pb, ps);
        }

        if (ok_cnt > 0) {
            printf("  %-22s %-10s %8.2f  %8.2f  %8.2f   (%d 张平均)\n",
                   "平均", "-", sum_y / ok_cnt, sum_bgr / ok_cnt, sum_soft / ok_cnt,
                   ok_cnt);
        }
        printf("  结果: %d 成功 / %d 失败\n", ok_cnt, fail_cnt);

        // 存一张硬编 JPEG 供 file(1) 验证
        if (!first_jpg.empty()) {
            const char* outp = "/tmp/bench_jpeg_encoder_hard.jpg";
            FILE* fp = fopen(outp, "wb");
            if (fp) {
                fwrite(first_jpg.data(), 1, first_jpg.size(), fp);
                fclose(fp);
                printf("  样张已存 %s (%zu 字节), 可用 file(1) 验证\n",
                       outp, first_jpg.size());
            }
        }
        printf("\n");
    }

    // ================= 2. 单 context 速度: 硬编 vs 软编, 2 轮 =================
    printf("=== 2. 单 context 速度 (N=%d, 跑 2 轮) ===\n", N);
    int bench_w = 0, bench_h = 0;
    {
        auto files = collect_real_images(img_dir, 1);
        if (files.empty()) {
            printf("  [-] 没有测试图\n");
            return 1;
        }
        cv::Mat bgr = cv::imread(files[0], cv::IMREAD_COLOR);
        if (bgr.empty()) {
            printf("  [-] 读不了 %s\n", files[0].c_str());
            return 1;
        }
        bench_w = bgr.cols;
        bench_h = bgr.rows;
        printf("  测试图 %s  %dx%d\n",
               files[0].substr(files[0].find_last_of('/') + 1).c_str(), bench_w, bench_h);

        edgevision::MppJpegEncoder enc;
        int r = enc.init(bench_w, bench_h, edgevision::kDefaultJpegQF);
        if (r) {
            printf("  [-] init 失败 ret=%d (%s)\n", r, enc.last_error().c_str());
            return 1;
        }
        std::vector<uint8_t> nv12(static_cast<size_t>(enc.hor_stride()) *
                                      enc.ver_stride() * 3 / 2,
                                  0x80);
        edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows, (int)bgr.step,
                                           nv12.data(), enc.hor_stride(),
                                           enc.ver_stride());

        // 拆解每阶段开销
        {
            // BGR->NV12
            std::vector<double> v;
            for (int i = 0; i < 20; ++i)
                edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows,
                                                   (int)bgr.step, nv12.data(),
                                                   enc.hor_stride(), enc.ver_stride());
            for (int i = 0; i < N; ++i) {
                double t0 = now_ms();
                edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows,
                                                   (int)bgr.step, nv12.data(),
                                                   enc.hor_stride(), enc.ver_stride());
                v.push_back(now_ms() - t0);
            }
            print_stats("BGR->NV12(full-range)", v);
        }
        {
            std::vector<double> v;
            for (int i = 0; i < 20; ++i)
                memcpy(enc.input_ptr(), nv12.data(), enc.input_size());
            for (int i = 0; i < N; ++i) {
                double t0 = now_ms();
                memcpy(enc.input_ptr(), nv12.data(), enc.input_size());
                v.push_back(now_ms() - t0);
            }
            print_stats("memcpy -> DRM(uncached)", v);
        }

        for (int round = 1; round <= 2; ++round) {
            printf("\n  ---- 第 %d 轮 ----\n", round);

            std::vector<uint8_t> hard_out;
            for (int i = 0; i < 20; ++i) enc.encode(nv12.data(), nv12.size(), &hard_out);
            std::vector<double> vh;
            double c0 = cpu_ms();
            for (int i = 0; i < N; ++i) {
                double t0 = now_ms();
                enc.encode(nv12.data(), nv12.size(), &hard_out);
                vh.push_back(now_ms() - t0);
            }
            double c1 = cpu_ms();
            print_stats("MPP 硬编 (含NV12拷贝)", vh);
            double wall_h = mean(vh) * N;   // 总 wall (ms)
            printf("  %-24s CPU占用=%5.1f%%  (每帧 cpu %.3f ms)\n", "  硬编 CPU",
                   100.0 * (c1 - c0) / wall_h, (c1 - c0) / N);

            std::vector<uint8_t> soft_out;
            std::vector<int> pr = {cv::IMWRITE_JPEG_QUALITY, kSoftQuality};
            for (int i = 0; i < 20; ++i) cv::imencode(".jpg", bgr, soft_out, pr);
            std::vector<double> vs;
            double c2 = cpu_ms();
            for (int i = 0; i < N; ++i) {
                double t0 = now_ms();
                cv::imencode(".jpg", bgr, soft_out, pr);
                vs.push_back(now_ms() - t0);
            }
            double c3 = cpu_ms();
            print_stats("cv::imencode 软编 q80", vs);
            printf("  %-24s CPU占用=%5.1f%%  (每帧 cpu %.3f ms)\n", "  软编 CPU",
                   100.0 * (c3 - c2) / (mean(vs) * N), (c3 - c2) / N);
            printf("  %-24s %.2fx   (字节硬/软 = %zu/%zu = %.3f)\n", "  硬编/软编(越<1 越好)",
                   mean(vh) / mean(vs), hard_out.size(), soft_out.size(),
                   (double)hard_out.size() / soft_out.size());
        }
        printf("\n");
    }

    // ================= 3. 多 context 并行 + 中断验证 =================
    printf("=== 3. 多 context 并行 (单 context 只占 1 个 jpege 核) ===\n");
    {
        auto files = collect_real_images(img_dir, 1);
        cv::Mat bgr = cv::imread(files[0], cv::IMREAD_COLOR);
        std::vector<uint8_t> nv12_dummy;

        for (int NT : {1, 2, 3, 4}) {
            edgevision::MppJpegEncoderPool pool;
            int r = pool.init(bgr.cols, bgr.rows, NT, edgevision::kDefaultJpegQF);
            if (r) {
                printf("  [-] NT=%d init 失败 ret=%d\n", NT, r);
                continue;
            }
            // 每路一份 NV12 (真实场景每路画面不同, 这里复用同一张 —— 已单独验证
            // 不同图之间编码耗时差异在测量噪声内)
            int hs = (bgr.cols + 15) & ~15;
            int vs = (bgr.rows + 15) & ~15;
            size_t nv12_size = static_cast<size_t>(hs) * vs * 3 / 2;
            std::vector<std::vector<uint8_t>> nv12s(NT, std::vector<uint8_t>(nv12_size, 0x80));
            std::vector<const uint8_t*> ptrs(NT);
            std::vector<std::vector<uint8_t>> outs(NT);
            for (int t = 0; t < NT; ++t) {
                edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows,
                                                   (int)bgr.step, nv12s[t].data(), hs, vs);
                ptrs[t] = nv12s[t].data();
            }
            (void)nv12_dummy;

            // 预热
            for (int w = 0; w < 20; ++w) pool.encode_batch(ptrs, nv12_size, &outs);

            const int REP = std::max(1, N / NT);  // 总帧数仍约 N
            auto irq0 = read_jpeg_irq();
            double c0 = cpu_ms(), t0 = now_ms();
            int bad = 0;
            for (int rep = 0; rep < REP; ++rep) {
                int rr = pool.encode_batch(ptrs, nv12_size, &outs);
                if (rr) { bad = rr; break; }
            }
            double t1 = now_ms(), c1 = cpu_ms();
            auto irq1 = read_jpeg_irq();

            int total = REP * NT;
            if (bad) {
                printf("  NT=%d 失败 ret=%d\n", NT, bad);
            } else {
                double per_frame = (t1 - t0) / total;
                printf("  NT=%d  聚合 %6.3f ms/帧 (%6.1f FPS 总吞吐)  CPU %5.1f%%"
                       "  单流延迟 %6.3f ms/帧\n",
                       NT, per_frame, 1000.0 / per_frame, 100.0 * (c1 - c0) / (t1 - t0),
                       (t1 - t0) / REP);
                print_irq_diff(irq0, irq1, total);
            }
        }

        // 软编多线程对照 (RK3588 有 8 个 A7x 核, 软编也能线性扩)
        printf("  -- 软编多线程对照 --\n");
        std::vector<int> pr = {cv::IMWRITE_JPEG_QUALITY, kSoftQuality};
        for (int NT : {1, 2, 4}) {
            auto work = [&]() {
                std::vector<uint8_t> tmp;
                for (int i = 0; i < N / NT; ++i) {
                    tmp.clear();
                    cv::imencode(".jpg", bgr, tmp, pr);
                }
            };
            work();
            double c0 = cpu_ms(), t0 = now_ms();
            std::vector<std::thread> ths;
            for (int t = 0; t < NT; ++t) ths.emplace_back(work);
            for (auto& th : ths) th.join();
            double t1 = now_ms(), c1 = cpu_ms();
            int total = (N / NT) * NT;
            printf("  NT=%d  聚合 %6.3f ms/帧 (%6.1f FPS 总吞吐)  CPU %5.1f%%\n",
                   NT, (t1 - t0) / total, 1000.0 * total / (t1 - t0),
                   100.0 * (c1 - c0) / (t1 - t0));
        }

        // ---- 3b. 稳态多 context: 线程只创建一次 ----
        // 上面 3 节每调一次 encode_batch 就 pthread_create 4 个线程, 50 次调用
        // = 200 次线程创建, 在 640x640 这种 1ms 量级的活上会被放大约 0.7ms/帧。
        // 真实的持续视频流是一个线程长期跑自己的 context, 所以这一节才是
        // "峰值吞吐"的正确口径; 3 节的口径适合"一批帧同时到达"的场景。
        printf("  -- 3b. 稳态多 context (线程只创建一次, 峰值吞吐口径) --\n");
        for (int NT : {1, 2, 3, 4}) {
            int hs = (bgr.cols + 15) & ~15;
            int vs = (bgr.rows + 15) & ~15;
            size_t nv12_size = static_cast<size_t>(hs) * vs * 3 / 2;
            std::vector<std::unique_ptr<edgevision::MppJpegEncoder>> encs;
            std::vector<std::vector<uint8_t>> nv12s(NT, std::vector<uint8_t>(nv12_size, 0x80));
            std::vector<std::vector<uint8_t>> outs(NT);
            bool ok = true;
            for (int t = 0; t < NT; ++t) {
                auto e = std::unique_ptr<edgevision::MppJpegEncoder>(
                    new edgevision::MppJpegEncoder());
                if (e->init(bgr.cols, bgr.rows, edgevision::kDefaultJpegQF)) { ok = false; break; }
                edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows,
                                                   (int)bgr.step, nv12s[t].data(), hs, vs);
                encs.push_back(std::move(e));
            }
            if (!ok) { printf("  NT=%d init 失败\n", NT); continue; }

            // 预热 (每路 20 帧)
            for (int t = 0; t < NT; ++t)
                for (int i = 0; i < 20; ++i) encs[t]->encode(nv12s[t].data(), nv12_size, &outs[t]);

            const int PER = std::max(1, N / NT);
            std::vector<int> rets(NT, 0);
            auto irq0 = read_jpeg_irq();
            double c0 = cpu_ms(), t0 = now_ms();
            std::vector<std::thread> ths;
            for (int t = 0; t < NT; ++t) {
                ths.emplace_back([&, t]() {
                    for (int i = 0; i < PER; ++i) {
                        int r = encs[t]->encode(nv12s[t].data(), nv12_size, &outs[t]);
                        if (r) { rets[t] = r; break; }
                    }
                });
            }
            for (auto& th : ths) th.join();
            double t1 = now_ms(), c1 = cpu_ms();
            auto irq1 = read_jpeg_irq();
            int total = PER * NT;
            int bad = 0;
            for (int t = 0; t < NT; ++t) if (rets[t]) bad = rets[t];
            if (bad) { printf("  NT=%d 失败 ret=%d\n", NT, bad); continue; }
            double per_frame = (t1 - t0) / total;
            printf("  NT=%d  聚合 %6.3f ms/帧 (%6.1f FPS 总吞吐)  CPU %5.1f%%"
                   "  单流延迟 %6.3f ms/帧\n",
                   NT, per_frame, 1000.0 / per_frame, 100.0 * (c1 - c0) / (t1 - t0),
                   (t1 - t0) / PER);
            print_irq_diff(irq0, irq1, total);
        }
        printf("\n");
    }

    // ================= 4. 分辨率-延迟曲线 =================
    printf("=== 4. 分辨率-延迟曲线 (区分算力 vs 固定往返延迟) ===\n");
    {
        auto files = collect_real_images(img_dir, 1);
        cv::Mat src = cv::imread(files[0], cv::IMREAD_COLOR);
        struct Sz { int w, h; };
        std::vector<Sz> sizes = {{64, 64}, {128, 128}, {256, 256}, {320, 320},
                                 {640, 640}, {960, 540}};
        printf("  %-12s %-12s %-10s %-10s %-12s\n", "分辨率", "硬编ms/帧",
               "硬编FPS", "软编ms/帧", "每MPix硬/软");
        for (auto s : sizes) {
            cv::Mat img;
            cv::resize(src, img, cv::Size(s.w, s.h));
            edgevision::MppJpegEncoder enc;
            if (enc.init(s.w, s.h, edgevision::kDefaultJpegQF)) {
                printf("  %4dx%-4d  init 失败\n", s.w, s.h);
                continue;
            }
            int hs = enc.hor_stride(), vs = enc.ver_stride();
            std::vector<uint8_t> nv12(static_cast<size_t>(hs) * vs * 3 / 2, 0x80);
            edgevision::bgr_to_nv12_full_range(img.data, img.cols, img.rows,
                                               (int)img.step, nv12.data(), hs, vs);
            std::vector<uint8_t> out;
            const int NAVG = std::min(N, 100);
            for (int i = 0; i < 20; ++i) enc.encode(nv12.data(), nv12.size(), &out);
            double t0 = now_ms();
            for (int i = 0; i < NAVG; ++i) enc.encode(nv12.data(), nv12.size(), &out);
            double hard = (now_ms() - t0) / NAVG;

            std::vector<uint8_t> sout;
            std::vector<int> pr2 = {cv::IMWRITE_JPEG_QUALITY, kSoftQuality};
            for (int i = 0; i < 20; ++i) cv::imencode(".jpg", img, sout, pr2);
            t0 = now_ms();
            for (int i = 0; i < NAVG; ++i) cv::imencode(".jpg", img, sout, pr2);
            double soft = (now_ms() - t0) / NAVG;

            double mp = s.w * s.h / 1e6;
            printf("  %4dx%-7d %-12.3f %-10.1f %-10.3f %.2f ms/MPix vs %.2f\n",
                   s.w, s.h, hard, 1000.0 / hard, soft,
                   mp > 0 ? hard / mp : 0.0, mp > 0 ? soft / mp : 0.0);
        }
        printf("\n");
    }

    // ================= 5. 生产分辨率 1280x720 =================
    // 真实链路 (w4_edgevision_preview_real / gateway) 跑的是 1280x720 MJPG,
    // 而 calib_v2/real_*.jpg 是 640x640, 所以这一节把实拍图缩放到生产分辨率
    // 再测 —— 结论才有"要不要替换掉 cv::imencode"的直接意义。
    printf("=== 5. 生产分辨率 1280x720 (实拍图缩放, 2 轮) ===\n");
    {
        auto files = collect_real_images(img_dir, 1);
        cv::Mat src = cv::imread(files[0], cv::IMREAD_COLOR);
        cv::Mat bgr;
        cv::resize(src, bgr, cv::Size(1280, 720));
        printf("  %dx%d\n", bgr.cols, bgr.rows);

        edgevision::MppJpegEncoder enc;
        int r = enc.init(bgr.cols, bgr.rows, edgevision::kDefaultJpegQF);
        if (r) {
            printf("  [-] init 失败 ret=%d (%s)\n", r, enc.last_error().c_str());
        } else {
            std::vector<uint8_t> nv12(static_cast<size_t>(enc.hor_stride()) *
                                          enc.ver_stride() * 3 / 2, 0x80);
            edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows,
                                               (int)bgr.step, nv12.data(),
                                               enc.hor_stride(), enc.ver_stride());
            {
                std::vector<double> v;
                for (int i = 0; i < 20; ++i)
                    edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows,
                                                       (int)bgr.step, nv12.data(),
                                                       enc.hor_stride(), enc.ver_stride());
                for (int i = 0; i < N; ++i) {
                    double t0 = now_ms();
                    edgevision::bgr_to_nv12_full_range(bgr.data, bgr.cols, bgr.rows,
                                                       (int)bgr.step, nv12.data(),
                                                       enc.hor_stride(), enc.ver_stride());
                    v.push_back(now_ms() - t0);
                }
                print_stats("BGR->NV12(full-range)", v);
            }
            for (int round = 1; round <= 2; ++round) {
                printf("  ---- 第 %d 轮 ----\n", round);
                std::vector<uint8_t> hard_out;
                for (int i = 0; i < 20; ++i) enc.encode(nv12.data(), nv12.size(), &hard_out);
                std::vector<double> vh;
                double c0 = cpu_ms();
                for (int i = 0; i < N; ++i) {
                    double t0 = now_ms();
                    enc.encode(nv12.data(), nv12.size(), &hard_out);
                    vh.push_back(now_ms() - t0);
                }
                double c1 = cpu_ms();
                print_stats("MPP 硬编 (含NV12拷贝)", vh);
                double wall_h = mean(vh) * N;
                printf("  %-24s CPU占用=%5.1f%%  (每帧 cpu %.3f ms)\n", "  硬编 CPU",
                       100.0 * (c1 - c0) / wall_h, (c1 - c0) / N);

                std::vector<uint8_t> soft_out;
                std::vector<int> pr3 = {cv::IMWRITE_JPEG_QUALITY, kSoftQuality};
                for (int i = 0; i < 20; ++i) cv::imencode(".jpg", bgr, soft_out, pr3);
                std::vector<double> vs;
                double c2 = cpu_ms();
                for (int i = 0; i < N; ++i) {
                    double t0 = now_ms();
                    cv::imencode(".jpg", bgr, soft_out, pr3);
                    vs.push_back(now_ms() - t0);
                }
                double c3 = cpu_ms();
                print_stats("cv::imencode 软编 q80", vs);
                printf("  %-24s CPU占用=%5.1f%%  (每帧 cpu %.3f ms)\n", "  软编 CPU",
                       100.0 * (c3 - c2) / (mean(vs) * N), (c3 - c2) / N);
                printf("  %-24s %.2fx   字节硬/软 = %zu/%zu = %.3f\n",
                       "  硬编/软编", mean(vh) / mean(vs), hard_out.size(),
                       soft_out.size(), (double)hard_out.size() / soft_out.size());
            }
            std::vector<uint8_t> hard_out;
            enc.encode(nv12.data(), nv12.size(), &hard_out);
            cv::Mat dec = cv::imdecode(hard_out, cv::IMREAD_COLOR);
            if (!dec.empty()) {
                cv::Mat g1, g2;
                cv::cvtColor(bgr, g1, cv::COLOR_BGR2GRAY);
                cv::cvtColor(dec, g2, cv::COLOR_BGR2GRAY);
                printf("  正确性: Y-PSNR=%.2f dB  BGR-PSNR=%.2f dB (尺寸 %dx%d)\n",
                       psnr_1ch(g1, g2), psnr_bgr(bgr, dec), dec.cols, dec.rows);
            } else {
                printf("  正确性: imdecode 失败!\n");
            }
        }
        printf("\n");
    }

    printf("============================================================\n");
    printf(" 完成\n");
    printf("============================================================\n");
    return 0;
}
