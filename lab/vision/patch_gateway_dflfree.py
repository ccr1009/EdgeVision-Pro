#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
积木 3：让 9000 网关吐出真实检测框

做三件事（幂等，重复跑会安全退出）：
  1. 模型换成 DFL-Free 版
  2. 裸调 rknn_init/rknn_run  ->  RKNNEngineDflFree（3 线程各绑一个物理核）
  3. 写死的假框               ->  真实的 Detection（适配原「两格」协议）

用法（在板子上）：
    python3 patch_gateway_dflfree.py
"""
import os
import shutil
import sys
import time

SRC   = "/root/lab/w4_edgevision_gateway.cpp"
BUILD = "/root/lab/build_w4_gateway.sh"

if not os.path.exists(SRC):
    print(f"[-] 找不到 {SRC}")
    sys.exit(1)

src = open(SRC, encoding="utf-8").read()

if "RKNNEngineDflFree" in src:
    print("[!] 文件里已经有 RKNNEngineDflFree，看起来已经改过了。")
    print("    想重来请先恢复备份： ls /root/lab/w4_edgevision_gateway.cpp.bak_*")
    sys.exit(0)

# ---------- 0. 备份 ----------
bak = SRC + ".bak_" + time.strftime("%Y%m%d_%H%M%S")
shutil.copy2(SRC, bak)
print(f"[+] 已备份 -> {bak}")

# ---------- 1. 加头文件 ----------
old = "#include <rknn_api.h>\n"
new = ('#include <rknn_api.h>\n'
       '#include "rknn_engine_dfl_free.h"   // 积木1: NpuCore 核绑定\n'
       '#include "postprocess.h"            // Detection 结构\n')
if old not in src:
    print("[-] 找不到 #include <rknn_api.h>，源码结构变了？")
    sys.exit(1)
src = src.replace(old, new, 1)

# ---------- 2. 模型路径改成 DFL-Free 版 ----------
old = '#define MODEL_PATH "/root/edgevision/data/models/yolov8n_rk3588_i8.rknn"'
new = '#define MODEL_PATH "/root/edgevision/data/models/yolov8n_dflfree_rk3588_i8.rknn"'
if old not in src:
    print("[-] 找不到旧的 MODEL_PATH 定义，源码结构变了？")
    sys.exit(1)
src = src.replace(old, new, 1)

# ---------- 3. 整体替换 npu_worker_thread ----------
START = "void* npu_worker_thread(void* arg) {"
END   = "// -------------------------------------------------------------\n// 7. TCP 网络推流服务线程"

i = src.find(START)
j = src.find(END)
if i < 0 or j < 0 or j <= i:
    print("[-] 找不到 npu_worker_thread 的边界，源码结构变了？")
    sys.exit(1)

NEW_WORKER = r'''void* npu_worker_thread(void* arg) {
    int core_id = *(int*)arg;
    const NpuCore core = (core_id == 0) ? NpuCore::Core0
                       : (core_id == 1) ? NpuCore::Core1 : NpuCore::Core2;

    // 3 个 worker 共享 g_model_data，不重复读盘（积木2 的成果）
    RKNNEngineDflFree engine;
    if (!engine.init_from_memory(g_model_data.data(), g_model_data.size(), core)) {
        std::cerr << "[-] [NPU Core #" << core_id << "] 引擎初始化失败!\n";
        return nullptr;
    }
    std::cout << "[NPU Core #" << core_id << "] 引擎就绪（物理核硬绑定）\n";

    while (g_running) {
        RawFrame frame;

        pthread_mutex_lock(&g_raw_lock);
        while (g_raw_queue.empty() && g_running) {
            pthread_cond_wait(&g_raw_cond, &g_raw_lock);
        }
        if (!g_running && g_raw_queue.empty()) {
            pthread_mutex_unlock(&g_raw_lock);
            break;
        }
        frame = std::move(g_raw_queue.front());
        g_raw_queue.pop();
        pthread_mutex_unlock(&g_raw_lock);

        // ★ 真推理：NPU + DFL-Free 解码 + NMS 一整套
        std::vector<Detection> dets;
        auto t0 = std::chrono::steady_clock::now();
        engine.run_inference(frame.rgb_data.data(), 640, 640, dets);
        auto t1 = std::chrono::steady_clock::now();
        double latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // 真检测结果 -> 原来的「固定两格」协议（协议暂不扩，超出部分丢弃）
        FrameResult res{};
        res.header.frame_id   = frame.frame_id;
        res.header.bbox_count = static_cast<uint32_t>(dets.size() < 2 ? dets.size() : 2);
        for (uint32_t i = 0; i < res.header.bbox_count; ++i) {
            const Detection& d = dets[i];
            res.boxes[i].class_id = d.class_id;
            res.boxes[i].score    = d.score;
            res.boxes[i].x = d.x1;
            res.boxes[i].y = d.y1;
            res.boxes[i].w = d.x2 - d.x1;   // x1y1x2y2 -> 左上角 + 宽高
            res.boxes[i].h = d.y2 - d.y1;
        }

        pthread_mutex_lock(&g_stream_lock);
        g_stream_queue.push(res);
        pthread_cond_signal(&g_stream_cond);
        pthread_mutex_unlock(&g_stream_lock);

        if (frame.frame_id % 15 == 0) {
            std::cout << "  [NPU Core #" << core_id << "] " << latency_ms
                      << " ms | 帧 #" << frame.frame_id
                      << " | 检出 " << dets.size()
                      << " 个，推送 " << res.header.bbox_count << "\n";
        }
    }

    std::cout << "[NPU Core #" << core_id << "] 引擎已释放。\n";
    return nullptr;
}

'''

src = src[:i] + NEW_WORKER + src[j:]
open(SRC, "w", encoding="utf-8").write(src)
print("[+] 网关源码已改写（假框 -> 真框）")

# ---------- 4. 编译脚本加新源文件 ----------
if os.path.exists(BUILD):
    b = open(BUILD, encoding="utf-8").read()
    if "rknn_engine_dfl_free" not in b:
        b = b.replace(
            "g++ -O2 /root/lab/w4_edgevision_gateway.cpp",
            "g++ -O2 -DENABLE_RKNN /root/lab/w4_edgevision_gateway.cpp \\\n"
            "    /root/edgevision/deploy/cpp/src/rknn_engine_dfl_free.cpp \\\n"
            "    /root/edgevision/deploy/cpp/src/postprocess_dfl_free.cpp")
        b = b.replace(
            "-I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include",
            "-I/root/edgevision/deploy/cpp/include \\\n"
            "    -I/root/edgevision/deploy/cpp/3rdparty/rknn_api/include")
        shutil.copy2(BUILD, BUILD + ".bak_" + time.strftime("%Y%m%d_%H%M%S"))
        open(BUILD, "w", encoding="utf-8").write(b)
        print("[+] build_w4_gateway.sh 已加入 DFL-Free 源文件与头文件路径")
    else:
        print("[=] build 脚本已包含 dflfree，跳过")

print("\n下一步： bash /root/lab/build_w4_gateway.sh")
