// 一次性测量工具（非交付物）：验证「模型只加载一次」后每句的真实成本。
// 只写 /tmp 下的 wav，不碰音频设备、不碰 NPU。
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "sherpa-onnx/c-api/c-api.h"

static double now() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main() {
    const char* dir = "/root/asr/tts-fanchen";
    std::string model = std::string(dir) + "/vits-zh-hf-fanchen-C.onnx";
    std::string tokens = std::string(dir) + "/tokens.txt";
    std::string lex = std::string(dir) + "/lexicon.txt";
    std::string dict = std::string(dir) + "/dict";

    SherpaOnnxOfflineTtsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model.vits.model = model.c_str();
    cfg.model.vits.tokens = tokens.c_str();
    cfg.model.vits.lexicon = lex.c_str();
    cfg.model.vits.dict_dir = dict.c_str();
    cfg.model.vits.noise_scale = 0.667f;
    cfg.model.vits.noise_scale_w = 0.8f;
    cfg.model.vits.length_scale = 1.0f;
    cfg.model.num_threads = 4;
    cfg.model.provider = "cpu";
    cfg.max_num_sentences = 1;

    double t0 = now();
    const SherpaOnnxOfflineTts* tts = SherpaOnnxCreateOfflineTts(&cfg);
    printf("load_once=%.2fs sr=%d\n", now() - t0, SherpaOnnxOfflineTtsSampleRate(tts));

    int lens[] = {2, 5, 15, 27, 80};
    for (int k = 0; k < 5; ++k) {
        std::string text;
        for (int i = 0; i < lens[k]; ++i) text += "测";
        for (int r = 0; r < 3; ++r) {
            double a = now();
            const SherpaOnnxGeneratedAudio* au = SherpaOnnxOfflineTtsGenerate(tts, text.c_str(), 0, 1.0f);
            double b = now();
            printf("n=%-3d rep%d  call=%.3fs audio=%.2fs\n", lens[k], r + 1,
                   b - a, au ? (double)au->n / au->sample_rate : 0.0);
            if (au) SherpaOnnxDestroyOfflineTtsGeneratedAudio(au);
        }
    }
    SherpaOnnxDestroyOfflineTts(tts);
    return 0;
}
