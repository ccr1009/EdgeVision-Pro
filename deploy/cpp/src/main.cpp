#include <iostream>
#include <chrono>
#include <vector>
#include "rknn_engine.h"
#include "postprocess.h"

int main(int argc, char** argv) {
    std::cout << "==================================================================" << std::endl;
    std::cout << "EdgeVision-RK3588: C++ Embedded Video Analytics Pipeline" << std::endl;
#ifdef ENABLE_RKNN
    std::cout << "Target Hardware: Rockchip RK3588 (NPU 6 TOPS / aarch64)" << std::endl;
#else
    std::cout << "Target Hardware: x86_64 Simulation Host (Stub Mode)" << std::endl;
#endif
    std::cout << "==================================================================" << std::endl;

    std::string model_path = (argc > 1) ? argv[1] : "data/models/yolov8n_rk3588_i8.rknn";

    RKNNEngine engine;
    if (!engine.init(model_path)) {
        std::cerr << "[-] Engine initialization failed!" << std::endl;
        return 1;
    }

    // Benchmark loop: 50 frames test
    const int TEST_FRAMES = 50;
    std::vector<unsigned char> dummy_frame(640 * 640 * 3, 128);

    std::cout << "[*] Running benchmark loop over " << TEST_FRAMES << " frames..." << std::endl;
    auto t_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < TEST_FRAMES; ++i) {
        std::vector<Detection> detections;
        auto t0 = std::chrono::high_resolution_clock::now();
        engine.run_inference(dummy_frame.data(), 640, 640, detections);
        auto t1 = std::chrono::high_resolution_clock::now();

        double frame_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (i % 10 == 0) {
            std::cout << "  Frame [" << i << "/" << TEST_FRAMES << "] "
                      << "Inference: " << frame_ms << " ms | Detected: " 
                      << detections.size() << " objects" << std::endl;
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    double avg_fps = (TEST_FRAMES * 1000.0) / total_ms;

    std::cout << "==================================================================" << std::endl;
    std::cout << "[SUCCESS] C++ Pipeline Execution Finished!" << std::endl;
    std::cout << "Average Latency: " << (total_ms / TEST_FRAMES) << " ms" << std::endl;
    std::cout << "Throughput:      " << avg_fps << " FPS" << std::endl;
    std::cout << "==================================================================" << std::endl;

    engine.release();
    return 0;
}
