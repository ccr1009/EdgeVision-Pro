// ============================================================================
//  voice_gateway.cpp —— 语音编排网关
// ============================================================================
//  把四个独立命令行工具串成一个常驻进程：
//
//    麦克风(hw:0,0) → [ASR 子进程: VAD+识别] → 文本
//                                              ↓
//                                        [Agent 子进程]
//                                              ↓
//    耳机(hw:2,0) ← [aplay] ← 重采样48k ← [TTS 子进程]
//
//  为什么用 fork/exec 而不是链接 C++ API：
//    sherpa-onnx 只发布了静态二进制包（38 个自包含可执行文件，共 684 MB），
//    没有 .a/.so 和头文件，要接库就得自己编 onnxruntime —— 不划算。
//    子进程方案顺带把"模型只加载一次"这件事交给工具自己常驻，反而更省内存。
//
//  系统编程要点（这也是本项目对 Linux 应用岗最有价值的部分）：
//    1. pipe + dup2 重定向子进程 stdio，父进程只读 stdout
//    2. 双向管道控制常驻 Agent：写 stdin 提问，读 stdout 等提示符
//    3. SIGSTOP / SIGCONT 实现半双工：播报时冻结 ASR 子进程，避免自激
//    4. SIGINT/SIGTERM 下有序回收所有子进程（waitpid，避免僵尸）
//    5. 读子进程输出必须带超时（poll），否则一个卡死的子进程会拖住整个网关
//
//  已知限制（诚实记录）：
//    - 没有唤醒词门控，目前用"最短文本长度 + 关键词"做粗过滤，目的是先跑通链路
//    - 重采样在这里手写（3 倍线性插值），因为板上 ALSA 的 plughw 插件失效
//    - 播放走 aplay 子进程，没有直接调 ALSA API
// ============================================================================

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <pty.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

// ---------------------------------------------------------------- 路径配置
static const char* kAsrBin   = "/root/asr/sherpa-onnx-v1.12.12-linux-aarch64-static/bin/sherpa-onnx-vad-alsa-offline-asr";
static const char* kTtsBin   = "/root/asr/sherpa-onnx-v1.12.12-linux-aarch64-static/bin/sherpa-onnx-offline-tts";
static const char* kAsrModel = "/root/asr/sherpa-onnx-paraformer-zh-small-2024-03-09";
static const char* kVadModel = "/root/asr/silero_vad.onnx";
static const char* kTtsModel = "/root/asr/tts-fanchen";
static const char* kAgentBin = "/root/rkllm_deploy/rkllm_embodied_agent";

static const char* kMicDev  = "hw:0,0";   // 摄像头麦克风
static const char* kSpkDev  = "hw:2,0";   // ES8388 耳机口
static const int   kSpkRate = 48000;      // ES8388 不支持 16 kHz，必须 48k

// ---------------------------------------------------------------- 运行状态
static volatile sig_atomic_t g_running = 1;
static pid_t g_asr_pid   = -1;
static pid_t g_agent_pid = -1;
static int   g_asr_fd    = -1;   // 读 ASR 子进程 stdout
static int   g_agent_in  = -1;   // 写 Agent stdin
static int   g_agent_out = -1;   // 读 Agent stdout

static void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf(const char* fmt, ...) {
    char buf[1024];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int n = snprintf(buf, sizeof(buf), "[%ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void on_signal(int) { g_running = 0; }

// ---------------------------------------------------------------- 子进程工具
// fork+exec，子进程的 stdout 接到伪终端（PTY），父进程从 master 端读。
//
// 为什么不用普通 pipe：
//   sherpa-onnx 是**静态链接**的二进制，stdout 接 pipe 时是全缓冲（4 KB），
//   不攒满就不 flush，父进程会一直读到空。
//   试过 stdbuf -oL，但它靠 LD_PRELOAD 注入，对静态程序无效（实测确认）。
//   PTY 是终端设备，内核按行缓冲处理 —— 这是唯一不依赖子进程配合的办法。
static pid_t spawn_capture(const std::vector<std::string>& argv, int* out_fd) {
    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(master); close(slave); return -1; }

    if (pid == 0) {
        close(master);
        setsid();                       // 独立会话
        ioctl(slave, TIOCSCTTY, 0);     // 显式把 PTY 设为控制终端
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > 2) close(slave);

        // 关键：setsid 之后子进程不是终端的前台进程组，一往 stdout 写就会收到
        // SIGTTOU 被挂起 —— 表现为"子进程起来了但一行输出都没有，随后消失"（实测踩过）。
        tcsetpgrp(STDIN_FILENO, getpid());

        struct termios tio;
        if (tcgetattr(STDIN_FILENO, &tio) == 0) {   // 关回显，免得输入被打回来
            tio.c_lflag &= ~(ECHO | ECHONL);
            tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }

    close(slave);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    *out_fd = master;
    return pid;
}

// fork+exec 但不抓输出（播放、TTS 这类只关心退出码的）
static pid_t spawn_wait(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    return pid;
}

// 带超时读一行；返回 false 表示超时或 EOF。已读到的半行存在 line 里。
static bool read_line_timeout(int fd, std::string& line, int timeout_ms) {
    struct pollfd pfd{fd, POLLIN, 0};
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return false;

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) return false;
    line.append(buf, (size_t)n);
    return true;
}

// 从缓冲区里切出一行（\n 结尾）
static bool pop_line(std::string& buf, std::string& out) {
    size_t pos = buf.find('\n');
    if (pos == std::string::npos) return false;
    out = buf.substr(0, pos);
    buf.erase(0, pos + 1);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return true;
}

// ---------------------------------------------------------------- WAV 处理
struct WavInfo { int rate; int channels; int bits; size_t data_off; size_t data_len; };

static bool wav_read_info(const char* path, WavInfo& wi) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    unsigned char hdr[44];
    if (fread(hdr, 1, 44, f) != 44) { fclose(f); return false; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { fclose(f); return false; }

    wi.channels = hdr[22] | (hdr[23] << 8);
    wi.rate     = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | ((unsigned)hdr[27] << 24);
    wi.bits     = hdr[34] | (hdr[35] << 8);
    wi.data_off = 44;

    fseek(f, 0, SEEK_END);
    wi.data_len = (size_t)ftell(f) - 44;
    fclose(f);
    return wi.channels == 1 && wi.bits == 16 && wi.rate > 0;
}

// 16k 单声道 → 48k 双声道（3 倍线性插值）。
// ES8388 只收 48k/44.1k 且只收双声道，而 TTS 固定输出 16k 单声道 —— 这一步躲不掉。
static bool wav_convert_to_spk(const char* in, const char* out) {
    WavInfo wi{};
    if (!wav_read_info(in, wi)) return false;
    if (wi.rate != 16000) {
        logf("  [!] TTS 输出 %d Hz，预期 16000；按倍率换算", wi.rate);
    }
    const int factor = kSpkRate / wi.rate;
    if (factor <= 0) return false;

    FILE* fi = fopen(in, "rb");
    if (!fi) return false;
    fseek(fi, (long)wi.data_off, SEEK_SET);
    std::vector<int16_t> src(wi.data_len / 2);
    size_t got = fread(src.data(), 2, src.size(), fi);
    fclose(fi);
    src.resize(got);
    if (src.size() < 2) return false;

    std::vector<int16_t> mono;
    mono.reserve(src.size() * factor);
    for (size_t i = 0; i + 1 < src.size(); ++i) {
        int a = src[i], b = src[i + 1];
        mono.push_back((int16_t)a);
        for (int k = 1; k < factor; ++k) {
            mono.push_back((int16_t)((a * (factor - k) + b * k) / factor));
        }
    }
    for (int k = 0; k < factor; ++k) mono.push_back(src.back());

    std::vector<int16_t> st;
    st.reserve(mono.size() * 2);
    for (int16_t v : mono) { st.push_back(v); st.push_back(v); }

    FILE* fo = fopen(out, "wb");
    if (!fo) return false;
    uint32_t data_bytes = (uint32_t)(st.size() * 2);
    uint32_t byte_rate = kSpkRate * 2 * 2;
    uint32_t chunk = 36 + data_bytes;
    unsigned char hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &chunk, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16; memcpy(hdr + 16, &fmtlen, 4);
    uint16_t pcm = 1;     memcpy(hdr + 20, &pcm, 2);
    uint16_t ch = 2;      memcpy(hdr + 22, &ch, 2);
    uint32_t rate = kSpkRate; memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    uint16_t align = 4;   memcpy(hdr + 32, &align, 2);
    uint16_t bits = 16;   memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_bytes, 4);
    fwrite(hdr, 1, 44, fo);
    fwrite(st.data(), 2, st.size(), fo);
    fclose(fo);
    return true;
}

// ---------------------------------------------------------------- TTS + 播放
// 优先走常驻 tts_daemon（Unix socket）：省掉每次 3.75 秒的 ONNX session 构建。
// 实测那 3.75 秒与文本长度无关，是纯 CPU（session 构建 + jieba/lexicon/FST 解析），
// 冷热 page cache 只差 0.02 秒，所以只能靠"别重复初始化"来解决。
static const char* kTtsSockPath = "/tmp/tts_daemon.sock";   // 与 tts_daemon.cpp 保持一致

// 把 16 kHz 单声道 PCM 写成 wav（后续 wav_convert_to_spk 负责重采样成 48k 双声道）
static bool write_wav_16k_mono(const char* path, const std::vector<int16_t>& pcm, int rate) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t data_bytes = (uint32_t)(pcm.size() * 2);
    uint32_t byte_rate = (uint32_t)(rate * 2);
    uint32_t chunk = 36 + data_bytes;
    unsigned char hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &chunk, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16; memcpy(hdr + 16, &fmtlen, 4);
    uint16_t pcm_fmt = 1; memcpy(hdr + 20, &pcm_fmt, 2);
    uint16_t ch = 1;      memcpy(hdr + 22, &ch, 2);
    uint32_t r = (uint32_t)rate; memcpy(hdr + 24, &r, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    uint16_t align = 2;   memcpy(hdr + 32, &align, 2);
    uint16_t bits = 16;   memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_bytes, 4);
    fwrite(hdr, 1, 44, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    return true;
}

static bool tts_via_daemon(const std::string& text, const char* out_wav) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, kTtsSockPath, sizeof(addr.sun_path) - 1);

    // 短超时：daemon 不在就赶紧降级，别把整轮拖住
    struct timeval tv{60, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return false; }

    uint32_t len_be = htonl((uint32_t)text.size());
    if (write(fd, &len_be, 4) != 4 ||
        write(fd, text.data(), text.size()) != (ssize_t)text.size()) {
        close(fd); return false;
    }

    int32_t hdr[2] = {0, 0};
    ssize_t hn = read(fd, hdr, 8);
    if (hn != 8) { close(fd); return false; }
    int32_t rate = (int32_t)ntohl((uint32_t)hdr[0]);
    int32_t n    = (int32_t)ntohl((uint32_t)hdr[1]);
    if (rate <= 0 || n <= 0) { close(fd); return false; }

    std::vector<int16_t> pcm((size_t)n);
    size_t want = (size_t)n * 2, got = 0;
    while (got < want) {
        ssize_t r = read(fd, (char*)pcm.data() + got, want - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got < want) return false;

    return write_wav_16k_mono(out_wav, pcm, rate);
}

static bool tts_to_wav(const std::string& text, const char* out_wav) {
    if (tts_via_daemon(text, out_wav)) return true;

    logf("  [!] TTS daemon 不可用，降级为 fork/exec（每次多花约 3.7s 加载模型）");
    std::vector<std::string> argv = {
        kTtsBin,
        std::string("--vits-model=") + kTtsModel + "/vits-zh-hf-fanchen-C.onnx",
        std::string("--vits-tokens=") + kTtsModel + "/tokens.txt",
        std::string("--vits-lexicon=") + kTtsModel + "/lexicon.txt",
        std::string("--vits-dict-dir=") + kTtsModel + "/dict",
        std::string("--output-filename=") + out_wav,
        "--num-threads=4",
        text,
    };
    pid_t pid = spawn_wait(argv);
    if (pid < 0) return false;
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static bool play_wav(const char* path) {
    std::vector<std::string> argv = {"aplay", "-D", kSpkDev, path};
    pid_t pid = spawn_wait(argv);
    if (pid < 0) return false;
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// 去掉 Markdown 标记再送 TTS。
// 模型输出的是给人读的 Markdown（**加粗**、`代码`、- 列表），
// 直接交给 TTS 会被逐字念成"星号星号…反引号…"（实测）。
static std::string strip_markdown(const std::string& s) {
    const std::string comma = "\xEF\xBC\x8C";   // 全角逗号，作为换行处的停顿
    std::string out;
    out.reserve(s.size());
    bool line_start = true;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '*' || c == '`' || c == '#' || c == '_' || c == '~') continue;
        if (c == '\n') {
            if (!out.empty() && (out.size() < 3 || out.compare(out.size() - 3, 3, comma) != 0))
                out += comma;
            line_start = true;
            continue;
        }
        if (line_start && (c == '-' || c == '+' || c == ' ')) continue;      // 列表符号
        if (line_start && c >= '0' && c <= '9') {                           // 有序列表 "1."
            size_t j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
            if (j < s.size() && s[j] == '.') { i = j; continue; }
        }
        out += c;
        line_start = false;
    }
    std::string clean;                          // 压缩连续逗号
    clean.reserve(out.size());
    for (size_t i = 0; i < out.size(); ) {
        if (i + 3 <= out.size() && out.compare(i, 3, comma) == 0) {
            if (clean.size() >= 3 && clean.compare(clean.size() - 3, 3, comma) == 0) { i += 3; continue; }
            clean += comma; i += 3; continue;
        }
        clean += out[i]; ++i;
    }
    return clean;
}

// 按句切分，用于分段播报。
// 动机：TTS 整段合成完才出声，80 字要等 13 秒才听到第一个字。
//
// 两步走，直白可靠：
//   1. 先按句末标点（。！？；）切
//   2. 若首段仍然过长，在第一个逗号处再切一刀
// 首段长度直接决定"用户等多久才听到第一个字"，所以单独处理它。
static std::vector<std::string> split_sentences(const std::string& s) {
    static const char* kEnd[] = {
        "\xE3\x80\x82",  // 。
        "\xEF\xBC\x81",  // ！
        "\xEF\xBC\x9F",  // ？
        "\xEF\xBC\x9B",  // ；
    };
    const std::string comma = "\xEF\xBC\x8C";   // ，

    std::vector<std::string> sents;
    std::string cur;
    for (size_t i = 0; i < s.size(); ) {
        bool cut = false;
        for (size_t m = 0; m < sizeof(kEnd) / sizeof(kEnd[0]); ++m) {
            size_t len = strlen(kEnd[m]);
            if (i + len <= s.size() && s.compare(i, len, kEnd[m]) == 0) {
                cur.append(kEnd[m]);
                i += len;
                cut = true;
                break;
            }
        }
        if (cut) { sents.push_back(cur); cur.clear(); continue; }
        cur += s[i];
        ++i;
        if (cur.size() > 240) { sents.push_back(cur); cur.clear(); }   // 兜底：80 字强切
    }
    if (!cur.empty()) sents.push_back(cur);

    // 首段过长 → 在第一个逗号处切开（45 字节 ≈ 15 字；切点至少 12 字节 ≈ 4 字）
    if (!sents.empty() && sents[0].size() > 45) {
        size_t pos = sents[0].find(comma);
        if (pos != std::string::npos && pos >= 12) {
            std::string rest = sents[0].substr(pos + comma.size());
            sents[0] = sents[0].substr(0, pos + comma.size());
            if (!rest.empty()) sents.insert(sents.begin() + 1, rest);
        }
    }
    return sents;
}
// 半双工 + 分段播报。
// 每段独立合成再播放：第一段 3~4 秒就能出声，不必等整段合成完（实测 80 字要 13 秒）。
static void speak(const std::string& raw) {
    std::string text = strip_markdown(raw);
    logf("🔊 播报: %s", text.c_str());

    static const char* kRaw = "/tmp/vgw_tts.wav";
    static const char* kSpk = "/tmp/vgw_spk.wav";

    // 整个播报期间冻结采集，避免麦克风把自己的声音再识别一遍（外放时必现自激）
    if (g_asr_pid > 0) kill(g_asr_pid, SIGSTOP);

    std::vector<std::string> sents = split_sentences(text);
    if (sents.empty()) {
        if (g_asr_pid > 0) kill(g_asr_pid, SIGCONT);
        return;
    }

    time_t t_all = time(nullptr);
    size_t spoken = 0;
    for (size_t k = 0; k < sents.size(); ++k) {
        time_t a = time(nullptr);
        if (!tts_to_wav(sents[k], kRaw)) { logf("  [!] 第 %zu 段 TTS 失败", k + 1); break; }
        time_t b = time(nullptr);
        if (!wav_convert_to_spk(kRaw, kSpk)) { logf("  [!] 第 %zu 段重采样失败", k + 1); break; }
        time_t c = time(nullptr);
        if (!play_wav(kSpk)) { logf("  [!] 第 %zu 段播放失败", k + 1); break; }
        time_t d = time(nullptr);
        ++spoken;
        logf("  [段 %zu/%zu] 合成 %lds / 播放 %lds", k + 1, sents.size(),
             (long)(b - a), (long)(d - c));
    }
    logf("  [耗时] 共 %zu 段，播报总时段 %lds", spoken, (long)(time(nullptr) - t_all));

    if (g_asr_pid > 0) kill(g_asr_pid, SIGCONT);
}

// ---------------------------------------------------------------- ASR 子进程
static bool asr_start() {
    std::vector<std::string> argv = {
        kAsrBin,
        std::string("--silero-vad-model=") + kVadModel,
        std::string("--tokens=") + kAsrModel + "/tokens.txt",
        std::string("--paraformer=") + kAsrModel + "/model.int8.onnx",
        "--num-threads=2",
        kMicDev,
    };
    g_asr_pid = spawn_capture(argv, &g_asr_fd);
    if (g_asr_pid < 0) { logf("✗ ASR 子进程启动失败: %s", strerror(errno)); return false; }
    logf("✓ ASR 子进程已启动 pid=%d（模型加载中，约 4~10s）", g_asr_pid);
    return true;
}

// 解析 " 12: 识别文本" 这种行
static bool parse_asr_line(const std::string& s, std::string& text) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
    size_t j = i;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    if (j + 2 >= s.size() || s[j] != ':' || s[j + 1] != ' ') return false;
    text = s.substr(j + 2);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.pop_back();
    return !text.empty();
}

// ---------------------------------------------------------------- Agent 子进程
static bool agent_start() {
    int in[2], out[2];
    if (pipe(in) != 0) return false;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return false; }

    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return false; }

    if (pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        // stderr 落盘而不是丢弃：rkllm 的真实失败原因（errno 14 / 模型路径 / 库缺失）
        // 都只在 stderr 里，丢掉等于放弃唯一线索
        int ef = open("/tmp/vgw_agent.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ef >= 0) { dup2(ef, STDERR_FILENO); close(ef); }
        for (int fd : {in[0], in[1], out[0], out[1]}) if (fd > 2) close(fd);
        // 两个都必须：
        //   1) chdir —— agent 用相对路径 "qwen3.5-2b_w8a8_rk3588.rkllm" 加载模型
        //   2) LD_LIBRARY_PATH —— 它编译时带 -Wl,-rpath=. ，rpath 也是相对 cwd 解析
        // 缺任一条就会得到 "E rkllm: load model file error!"（实测踩过）
        if (chdir("/root/rkllm_deploy") != 0) _exit(126);
        setenv("LD_LIBRARY_PATH", "/root/rkllm_deploy", 1);
        execl(kAgentBin, kAgentBin, (char*)nullptr);
        _exit(127);
    }

    close(in[0]);
    close(out[1]);
    fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
    g_agent_pid = pid;
    g_agent_in = in[1];
    g_agent_out = out[0];

    // 等首个 "EdgeAgent> " 提示符，确认模型就绪。
    // 注意：提示符是 `std::cout << "\nEdgeAgent> "` —— 后面**没有换行**，
    // 所以不能按行解析（永远等不到 '\n'），必须在缓冲区里做子串匹配。
    std::string buf;
    time_t t0 = time(nullptr);
    while (g_running && time(nullptr) - t0 < 240) {   // 冷启动实测 >120s，给 240s
        if (read_line_timeout(g_agent_out, buf, 500)) {
            if (buf.find("EdgeAgent>") != std::string::npos) {
                logf("✓ Agent 子进程就绪 pid=%d（耗时 %lds）", g_agent_pid, (long)(time(nullptr) - t0));
                // 把管道里剩余数据排空。否则下次 agent_ask 会读到残留的提示符，
                // 误判"本轮已结束"而立刻返回空结果（实测踩过）。
                char tmp[4096];
                while (read(g_agent_out, tmp, sizeof(tmp)) > 0) { /* drain */ }
                return true;
            }
            if (buf.size() > 65536) buf.erase(0, 32768);
        }
    }
    logf("✗ Agent 就绪超时");
    return false;
}

// 向 Agent 提一个问，等它回到下一个提示符，返回本轮的终局汇报。
// Agent 的输出是流式的，靠 "EdgeAgent> " 判定一轮结束。
static std::string agent_ask(const std::string& q, int timeout_s = 180) {
    if (g_agent_in < 0) return "";
    std::string l = q + "\n";
    if (write(g_agent_in, l.c_str(), l.size()) < 0) return "";

    std::string buf;
    std::string sec_think;   // 「思考与决策」之后的内容（路径 B：直接回答）
    std::string sec_final;   // 「终局汇报」之后的内容（路径 A：工具链跑完的最终回答）
    bool in_final = false;

    // 把 chunk 里已完整的行分类吸收，未完成的半行留在 chunk 里由调用者继续拼。
    //
    // 三个血泪教训：
    //  1. 必须保留半行。管道每次只读 512 字节，长行（如 get_patrol_events 的 JSON）
    //     会被切成多段；直接 clear() 会让后续片段绕过所有过滤规则。
    //  2. Agent 有两条输出路径，都要接：
    //        A 需要工具：思考与决策 → <tool_call> → 观测 → 终局汇报 → 回答
    //        B 不需要工具：思考与决策 → 回答（**没有**终局汇报）
    //     只认 A 会让闲聊类提问全部降级成"我听到你说…"。
    //  3. 但路径 A 的「思考段」里混着工具观测垃圾（长行被截断后认不出前缀），
    //     所以最终优先返回 sec_final，只有它为空（说明走的是路径 B）才用 sec_think。
    static FILE* dbg = nullptr;
    auto absorb = [&](std::string& chunk) {
        if (!dbg) dbg = fopen("/tmp/vgw_absorb.log", "w");
        size_t p = 0;
        while (true) {
            size_t e = chunk.find('\n', p);
            if (e == std::string::npos) break;
            std::string s = chunk.substr(p, e - p);
            p = e + 1;
            if (!s.empty() && s.back() == '\r') s.pop_back();
            if (dbg) fprintf(dbg, "%s| %zu | %s\n", in_final ? "F" : "-", s.size(), s.c_str());

            if (s.find("终局汇报") != std::string::npos) { sec_final.clear(); in_final = true; continue; }
            if (s.find("思考与决策") != std::string::npos) { sec_think.clear(); in_final = false; continue; }
            if (s.find("👤 [User]") != std::string::npos) { sec_think.clear(); sec_final.clear(); continue; }
            if (s.rfind("---", 0) == 0 || s.rfind("===", 0) == 0) continue;

            // 框架行与工具调用，始终丢弃
            if (s.find("[Agent 决策]") != std::string::npos) continue;
            if (s.find("[硬件感知") != std::string::npos) continue;
            if (s.find("-> 设定参数") != std::string::npos) continue;
            if (s.find("<tool_call>") != std::string::npos) continue;
            if (s.find("</tool_call>") != std::string::npos) continue;
            if (s.find("<function=") != std::string::npos) continue;
            if (s.find("</function>") != std::string::npos) continue;
            if (s.find("<parameter=") != std::string::npos) continue;
            if (s.find("</parameter>") != std::string::npos) continue;

            if (!s.empty()) {
                if (in_final) sec_final += s + "\n";
                else          sec_think += s + "\n";
            }
        }
        chunk.erase(0, p);          // 保留未完成的半行
        if (dbg) {
            fprintf(dbg, "--- think=%zu final=%zu ---\n", sec_think.size(), sec_final.size());
            fflush(dbg);
        }
    };

    time_t t0 = time(nullptr);
    size_t total_read = 0;
    int absorb_calls = 0;
    while (g_running && time(nullptr) - t0 < timeout_s) {
        if (!read_line_timeout(g_agent_out, buf, 500)) {
            if (buf.size() > 65536) buf.erase(0, 32768);
            continue;
        }
        total_read += buf.size();
        ++absorb_calls;
        // 提示符没有换行，必须做子串匹配
        size_t ph = buf.find("EdgeAgent>");
        if (ph != std::string::npos) {
            std::string pre = buf.substr(0, ph);
            absorb(pre);
            buf.erase(0, ph + 10);
            std::string res = sec_final.empty() ? sec_think : sec_final;
            logf("  [dbg] 读到 %zu 字节 / absorb %d 次 / think=%zu final=%zu → 采用 %s",
                 total_read, absorb_calls, sec_think.size(), sec_final.size(),
                 sec_final.empty() ? "think段" : "final段");
            return res;
        }
        absorb(buf);        // 内部已 erase 完整行，半行留在 buf 里继续拼
    }
    std::string res = sec_final.empty() ? sec_think : sec_final;
    logf("  [dbg] 超时退出：读到 %zu 字节 / absorb %d 次 / think=%zu final=%zu",
         total_read, absorb_calls, sec_think.size(), sec_final.size());
    return res;
}

// ---------------------------------------------------------------- 清理
// 子进程必须死透。SIGTERM 不响应就 SIGKILL —— 否则会留下孤儿继续霸占麦克风，
// 下次启动全部 exit 255（实测踩过：一个孤儿 ASR 进程让新实例无限重启）。
static void reap(pid_t pid, const char* name) {
    if (pid <= 0) return;
    kill(pid, SIGCONT);          // 先解冻：可能被半双工的 SIGSTOP 冻着
    kill(pid, SIGTERM);
    int st = 0;
    for (int i = 0; i < 20; ++i) {
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        usleep(100000);
    }
    logf("  [!] %s(pid=%d) 未响应 SIGTERM，强制 SIGKILL", name, pid);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
}

static void cleanup() {
    if (g_asr_pid > 0)   reap(g_asr_pid, "ASR");
    if (g_agent_pid > 0) reap(g_agent_pid, "Agent");
    g_asr_pid = -1;
    g_agent_pid = -1;

    if (g_asr_fd >= 0)    close(g_asr_fd);
    if (g_agent_in >= 0)  close(g_agent_in);
    if (g_agent_out >= 0) close(g_agent_out);
    g_asr_fd = g_agent_in = g_agent_out = -1;

    unlink("/tmp/vgw_tts.wav");
    unlink("/tmp/vgw_spk.wav");
    logf("已回收全部子进程");
}

// ---------------------------------------------------------------- 主流程
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    bool self_test = false;
    bool no_agent = false;
    std::vector<std::string> asks;
    std::string say_text;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--self-test")) self_test = true;
        if (!strcmp(argv[i], "--no-agent")) no_agent = true;
        if (!strcmp(argv[i], "--ask") && i + 1 < argc) asks.push_back(argv[++i]);
        if (!strcmp(argv[i], "--say") && i + 1 < argc) say_text = argv[++i];
    }

    logf("════ voice_gateway 启动 ════");

    // 直接播报一段文本，用于单独测 TTS 合成/播放耗时
    if (!say_text.empty()) {
        speak(say_text);
        return 0;
    }

    // 调试模式：直接问 Agent（可多个 --ask，用于观察多轮 KV cache 效果）
    if (!asks.empty()) {
        if (!agent_start()) { logf("✗ Agent 未就绪"); return 1; }
        int rc = 0;
        for (auto& q : asks) {
            time_t t0 = time(nullptr);
            std::string r = agent_ask(q);
            logf("【%s】耗时 %lds，%zu 字节", q.c_str(), (long)(time(nullptr) - t0), r.size());
            fputs(r.c_str(), stdout);
            fputc('\n', stdout);
            fflush(stdout);
            if (r.empty()) rc = 2;
        }
        cleanup();
        return rc;
    }

    if (self_test) {
        logf("自检模式：只验证 重采样 + 播放");
        if (!tts_to_wav("自检通过，语音链路正常。", "/tmp/vgw_tts.wav")) {
            logf("✗ TTS 失败"); return 1;
        }
        if (!wav_convert_to_spk("/tmp/vgw_tts.wav", "/tmp/vgw_spk.wav")) {
            logf("✗ 重采样失败"); return 1;
        }
        if (!play_wav("/tmp/vgw_spk.wav")) { logf("✗ 播放失败"); return 1; }
        logf("✓ 自检通过");
        return 0;
    }

    if (!asr_start()) return 1;
    if (!no_agent && !agent_start()) {
        logf("! Agent 未就绪，降级为「只识别 + 回显」模式");
    } else if (g_agent_in >= 0) {
        // 预热：首轮要 prefill 2000+ 字的 system prompt，实测 28s；之后 KV cache 命只需 9~10s。
        // 把这 28s 挪到启动阶段（用户还没开始说话），第一次提问就能直接享受 9s 的响应。
        logf("预热模型（首轮 prefill 约 30s，完成后首次提问可降到 10s 内）...");
        time_t tp = time(nullptr);
        agent_ask("你好");
        logf("✓ 预热完成，耗时 %lds", (long)(time(nullptr) - tp));
    }

    logf("════ 开始监听，说话即可（Ctrl+C 退出）════");

    std::string buf, line;
    int seg = 0;
    while (g_running) {
        // 守护 ASR 子进程：它退出后必须重启，否则父进程会一直空转
        // （实测踩过：子进程启动 5 秒就没了，父进程干等了 100 分钟毫无察觉）
        if (g_asr_pid > 0) {
            int st = 0;
            if (waitpid(g_asr_pid, &st, WNOHANG) == g_asr_pid) {
                logf("! ASR 子进程已退出（%s=%d），3 秒后重启",
                     WIFSIGNALED(st) ? "signal" : "code",
                     WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
                if (g_asr_fd >= 0) { close(g_asr_fd); g_asr_fd = -1; }
                g_asr_pid = -1;
                sleep(3);
                asr_start();
                buf.clear();
                continue;
            }
        }
        if (g_asr_fd < 0) { sleep(1); continue; }

        if (!read_line_timeout(g_asr_fd, buf, 500)) {
            if (buf.size() > 65536) buf.erase(0, 32768);
            continue;
        }

        while (pop_line(buf, line)) {
            std::string text;
            if (!parse_asr_line(line, text)) {
                if (line.find("Started") != std::string::npos) logf("麦克风已就绪，请说话");
                continue;
            }
            ++seg;
            logf("👤 [%d] %s", seg, text.c_str());

            // 粗过滤：先挡住极短的无意义片段（真正的唤醒词门控待做）
            if (text.size() < 3) { logf("    (过短，丢弃)"); continue; }

            std::string reply;
            if (g_agent_in >= 0) {
                reply = agent_ask(text);
            }
            if (reply.empty()) {
                reply = "我听到你说" + text;
            }
            logf("🤖 %s", reply.c_str());
            speak(reply);
        }
    }

    cleanup();
    return 0;
}
