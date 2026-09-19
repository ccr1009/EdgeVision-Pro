#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include "bev_projector.hpp"
#include "zero_copy_pipeline.hpp"
#include "rknn_engine.h"
#include "postprocess.h"

using namespace edgevision;

int main(int argc, char** argv) {
    std::cout << "==================================================================" << std::endl;
    std::cout << "EdgeVision Pro: Multi-Channel Zero-Copy Gateway & 3D/BEV Engine" << std::endl;
    std::cout << "Platform Target: Rockchip RK3588 (Tri-Core NPU 6.0 TOPS)" << std::endl;
    std::cout << "==================================================================" << std::endl;

    // 1. Initialize Zero-Copy Memory Pipeline
    ZeroCopyPipelineEngine zc_engine(8);
    std::cout << "[+] Hardware Zero-Copy Memory Pool Initialized (MPP/RGA/NPU DMA-BUF)" << std::endl;

    // 2. Initialize Camera & BEV Projector
    BEVProjector projector(1280, 720, 65.0f, 1.6f, 8.0f);
    std::cout << "[+] BEV Coordinate Projector Initialized (Camera Height: 1.6m, Pitch: 8.0 deg)" << std::endl;

    // 3. Initialize RKNN Engine Stub / Native
    std::string model_path = "data/models/yolov8n_rk3588_i8.rknn";
    RKNNEngine npu_engine;
    npu_engine.init(model_path);

    // 4. Simulate Multi-Channel Pipeline across 3 NPU Cores
    const int num_channels = 3;
    RKNNCoreMaskCpp cores[num_channels] = {
        RKNN_CORE_0, // Cam 0 (Front) -> NPU Core 0
        RKNN_CORE_1, // Cam 1 (Left)  -> NPU Core 1
        RKNN_CORE_2  // Cam 2 (Right) -> NPU Core 2
    };

    std::cout << "[*] Starting Multi-Channel Zero-Copy Simulation Loop (100 frames)..." << std::endl;

    auto t_start = std::chrono::high_resolution_clock::now();
    int total_processed_frames = 0;

    for (int frame_idx = 0; frame_idx < 100; ++frame_idx) {
        for (int ch = 0; ch < num_channels; ++ch) {
            // Step A: Acquire MPP hardware-decoded buffer (Zero-Copy)
            DMABufDescriptor* mpp_buf = zc_engine.acquire_mpp_buffer();
            if (!mpp_buf) continue;

            // Step B: RGA 2D Hardware color-convert & resize
            DMABufDescriptor* rga_buf = zc_engine.rga_hardware_transform(mpp_buf);
            if (!rga_buf) continue;

            // Step C: NPU inference assigned to specific core
            // On RK3588 real board: rknn_set_core_mask(ctx, cores[ch]);
            std::vector<uint8_t> dummy_input(640 * 640 * 3, 128);
            std::vector<Detection> detections;
            npu_engine.run_inference(dummy_input.data(), 640, 640, detections);

            // Step D: Release RGA DMA-BUF buffer
            zc_engine.release_rga_buffer(rga_buf);

            // Step E: 3D Box & BEV Projection
            Box3D box3d;
            bool valid_3d = projector.estimate_3d_box(520.0f, 400.0f, 760.0f, 610.0f, "car", box3d);
            (void)valid_3d;

            total_processed_frames++;
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    double fps = (total_processed_frames / total_ms) * 1000.0;

    std::cout << "==================================================================" << std::endl;
    std::cout << "[SUCCESS] Multi-Channel BEV Gateway Simulation Finished!" << std::endl;
    std::cout << "  Processed Frames:       " << total_processed_frames << " frames" << std::endl;
    std::cout << "  Zero-Copy Cycles:       " << zc_engine.get_zero_copy_count() << std::endl;
    std::cout << "  Memory Bandwidth Saved: " << std::fixed << std::setprecision(2)
              << (zc_engine.get_bytes_saved() / (1024.0 * 1024.0)) << " MB" << std::endl;
    std::cout << "  System Throughput:      " << std::fixed << std::setprecision(1)
              << fps << " FPS (Aggregated Multi-Channel)" << std::endl;
    std::cout << "==================================================================" << std::endl;

    return 0;
}
