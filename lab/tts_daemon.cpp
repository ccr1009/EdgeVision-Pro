// tts_daemon.cpp —— 常驻 TTS 服务（Unix socket）
//
// 为什么存在：
//   sherpa-onnx-offline-tts 是「一次性进程」，每次 fork/exec 都要重新
//   读 116 MB 的 VITS 模型、建 ONNX Runtime session、解析 2.4 MB lexicon。
//   实测固定开销 3.70 s（冷热页缓存都一样，是纯 CPU，不是磁盘 IO）。
//   常驻进程把这段加载做一次，之后每个请求只付合成时间。
//
// 实测（RK3588, num_threads=4）：
//   fork/exec 跑 15 字：3.70 s 加载 + 1.8 s 合成 ≈ 5.5 s
//   本服务同一句：≈ 1.8 s
//
// 说明：
//   - 只做 CPU 推理，不碰麦克风、不碰扬声器、不碰 NPU。
//   - 输出是 16 kHz 单声道 PCM16，重采样/播放仍由调用方负责。
//
// 协议（AF_UNIX SOCK_STREAM，大端）：
//   请求  : [u32 len][len 字节 UTF-8 文本]
//   响应  : [i32 sample_rate][i32 n_samples][n_samples * i16 PCM]
//   出错  : [i32 -1][i32 0]
//   sample_rate < 0 即表示失败。连接按请求-响应一问一答，可复用。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

// ------------------------------------------------------------------ 配置
static const char* kModelDir  = "/root/asr/tts-fanchen";
static const char* kSockPath  = "/tmp/tts_daemon.sock";
static const int   kNumThreads = 4;
static const int32_t kMaxTextBytes = 8192;

static std::atomic<bool> g_stop{false};

// ------------------------------------------------------------------ 日志
static void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf(const char* fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char head[32];
    snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d ",
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    fputs(head, stdout);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ------------------------------------------------------------------ 全局 TTS
static const SherpaOnnxOfflineTts* g_tts = nullptr;
static int32_t g_sample_rate = 0;
static std::mutex g_tts_mu;   // C API 的 tts 对象非线程安全，串行化

static bool tts_init() {
    int64_t t0 = now_ms();

    std::string model = std::string(kModelDir) + "/vits-zh-hf-fanchen-C.onnx";
    std::string tokens = std::string(kModelDir) + "/tokens.txt";
    std::string lexicon = std::string(kModelDir) + "/lexicon.txt";
    std::string dict = std::string(kModelDir) + "/dict";

    SherpaOnnxOfflineTtsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model.vits.model = model.c_str();
    cfg.model.vits.tokens = tokens.c_str();
    cfg.model.vits.lexicon = lexicon.c_str();
    cfg.model.vits.dict_dir = dict.c_str();
    cfg.model.vits.noise_scale = 0.667f;
    cfg.model.vits.noise_scale_w = 0.8f;
    cfg.model.vits.length_scale = 1.0f;
    cfg.model.num_threads = kNumThreads;
    cfg.model.provider = "cpu";
    cfg.model.debug = 0;
    // 实测：0 = 按标点内部切句，长文本更快（80 字 9.9s vs 11.5s）；
    //       1 = 整段一次生成，padding 更大。取 0 与 CLI 行为一致。
    cfg.max_num_sentences = 0;

    g_tts = SherpaOnnxCreateOfflineTts(&cfg);
    if (!g_tts) { logf("✗ 模型加载失败: %s", model.c_str()); return false; }

    g_sample_rate = SherpaOnnxOfflineTtsSampleRate(g_tts);
    logf("✓ 模型已加载 %.2fs, sample_rate=%d, speakers=%d",
         (now_ms() - t0) / 1000.0, g_sample_rate,
         SherpaOnnxOfflineTtsNumSpeakers(g_tts));

    // 预热：首次 Generate 会顺带分配 ORT 的推理 arena，
    // 不预热的话第一个真实请求会多花 ~0.5s。
    int64_t t1 = now_ms();
    const SherpaOnnxGeneratedAudio* a = SherpaOnnxOfflineTtsGenerate(g_tts, "预热", 0, 1.0f);
    if (a) SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
    logf("✓ 预热 %.2fs，服务就绪", (now_ms() - t1) / 1000.0);
    return true;
}

// ------------------------------------------------------------------ IO 辅助
static bool read_full(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r == 0) return false;              // 对端关闭
        if (r < 0) { if (errno == EINTR) continue; return false; }
        p += r; n -= (size_t)r;
    }
    return true;
}

static bool write_full(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w; n -= (size_t)w;
    }
    return true;
}

static bool send_error(int fd) {
    int32_t hdr[2] = { htonl((uint32_t)-1), 0 };
    return write_full(fd, hdr, sizeof(hdr));
}

// float [-1,1] → int16
static void to_pcm16(const float* src, int32_t n, std::vector<int16_t>& out) {
    out.resize((size_t)n);
    for (int32_t i = 0; i < n; ++i) {
        float v = src[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out[(size_t)i] = (int16_t)(v * 32767.0f);
    }
}

// ------------------------------------------------------------------ 连接处理
static void serve_conn(int fd) {
    for (;;) {
        uint32_t len_be = 0;
        if (!read_full(fd, &len_be, 4)) break;
        uint32_t len = ntohl(len_be);
        if (len == 0 || len > (uint32_t)kMaxTextBytes) { send_error(fd); break; }

        std::string text(len, '\0');
        if (!read_full(fd, &text[0], len)) break;
        if (text.find('\0') != std::string::npos) text.resize(text.find('\0'));

        int64_t t0 = now_ms();
        std::vector<int16_t> pcm;
        int32_t sr = 0, n = 0;
        {
            std::lock_guard<std::mutex> lk(g_tts_mu);
            const SherpaOnnxGeneratedAudio* audio =
                SherpaOnnxOfflineTtsGenerate(g_tts, text.c_str(), 0, 1.0f);
            if (audio) {
                to_pcm16(audio->samples, audio->n, pcm);
                sr = audio->sample_rate;
                n = audio->n;
                SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
            }
        }
        logf("[req] %zu 字节文本 → %d 采样点 %.2fs, 合成 %lldms",
             text.size(), n, n ? (double)n / sr : 0.0, (long long)(now_ms() - t0));

        if (n <= 0) { send_error(fd); break; }

        int32_t hdr[2] = { (int32_t)htonl((uint32_t)sr), (int32_t)htonl((uint32_t)n) };
        if (!write_full(fd, hdr, sizeof(hdr))) break;
        if (!write_full(fd, pcm.data(), (size_t)n * sizeof(int16_t))) break;
    }
    ::close(fd);
}

// ------------------------------------------------------------------ main
static void on_signal(int) { g_stop = true; }

int main(int argc, char** argv) {
    if (argc > 1) kSockPath = argv[1];

    ::signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (!tts_init()) return 1;

    ::unlink(kSockPath);
    int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(kSockPath) >= sizeof(addr.sun_path)) { logf("✗ socket 路径过长"); return 1; }
    strncpy(addr.sun_path, kSockPath, sizeof(addr.sun_path) - 1);

    if (::bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    ::chmod(kSockPath, 0666);
    if (::listen(lfd, 8) < 0) { perror("listen"); return 1; }
    logf("✓ 监听 %s", kSockPath);

    while (!g_stop) {
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (g_stop) break;
            perror("accept");
            break;
        }
        std::thread(serve_conn, cfd).detach();
    }

    ::close(lfd);
    ::unlink(kSockPath);
    if (g_tts) SherpaOnnxDestroyOfflineTts(g_tts);
    logf("已退出");
    return 0;
}
