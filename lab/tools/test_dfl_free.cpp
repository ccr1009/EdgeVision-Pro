// ==============================================================================
// test_dfl_free.cpp —— 验证 C++ DFL 解码与 numpy 参考实现逐位一致
// 用法（板端）:
//   g++ -O2 -std=c++17 test_dfl_free.cpp ../src/postprocess_dfl_free.cpp \
//       -I../include -o test_dfl_free
//   ./test_dfl_free raw_box.bin cls_sig.bin expected.txt
// ==============================================================================
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <chrono>

#include "postprocess_dfl_free.h"

static std::vector<float> load_bin(const char* path, size_t expect_elems) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "[-] 打不开 " << path << "\n"; std::exit(1); }
    size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<float> buf(n / sizeof(float));
    f.read(reinterpret_cast<char*>(buf.data()), n);
    std::cout << "[*] " << path << " : " << buf.size() << " floats (期望 " << expect_elems << ")\n";
    if (buf.size() != expect_elems) { std::cerr << "[-] 元素数不符\n"; std::exit(1); }
    return buf;
}

int main(int argc, char** argv) {
    const char* box_path = argc > 1 ? argv[1] : "raw_box.bin";
    const char* cls_path = argc > 2 ? argv[2] : "cls_sig.bin";
    const char* exp_path = argc > 3 ? argv[3] : "expected.txt";

    auto box = load_bin(box_path, 64u * 8400u);
    auto cls = load_bin(cls_path, 80u * 8400u);

    std::vector<Detection> dets;
    int cand = 0;

    // 跑 20 次取平均，看解码本身的耗时（这是砍头方案要付的 CPU 代价）
    const int iter = 20;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iter; ++i) {
        dflfree::decode(box.data(), cls.data(), false, false, 640, 0.25f, 0.45f, dets, &cand);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iter;

    std::cout << "[+] 候选框 " << cand << "  NMS 后 " << dets.size() << "\n";
    std::cout << "[+] 解码耗时 " << ms << " ms/帧（含 NMS，单线程）\n";
    for (size_t i = 0; i < dets.size(); ++i) {
        printf("    #%zu  (%.4f, %.4f, %.4f, %.4f)  score=%.4f  cls=%d\n",
               i, dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2, dets[i].score, dets[i].class_id);
    }

    // 与 numpy 参考结果对比
    std::ifstream ef(exp_path);
    if (!ef) { std::cout << "[!] 没有参考文件，跳过对比\n"; return 0; }
    std::string header;
    std::getline(ef, header);
    std::vector<Detection> ref;
    double a, b, c, d, e; int f;
    while (ef >> a >> b >> c >> d >> e >> f) ref.push_back({(float)a,(float)b,(float)c,(float)d,(float)e,f});

    std::cout << "\n=== 与 numpy 参考对比（参考 " << ref.size() << " 框） ===\n";
    if (ref.size() != dets.size()) {
        std::cout << "[FAILED] 框数不一致: cpp=" << dets.size() << " numpy=" << ref.size() << "\n";
        return 1;
    }
    double max_err = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        double err = std::max({std::fabs(dets[i].x1 - ref[i].x1), std::fabs(dets[i].y1 - ref[i].y1),
                               std::fabs(dets[i].x2 - ref[i].x2), std::fabs(dets[i].y2 - ref[i].y2)});
        max_err = std::max(max_err, err);
        if (dets[i].class_id != ref[i].class_id) {
            std::cout << "[FAILED] #" << i << " 类别不一致\n"; return 1;
        }
    }
    std::cout << "[+] 坐标最大偏差 = " << max_err << " px\n";
    if (max_err < 1e-2) std::cout << "[SUCCESS] C++ 解码与 numpy 参考一致\n";
    else { std::cout << "[FAILED] 偏差过大\n"; return 1; }
    return 0;
}
