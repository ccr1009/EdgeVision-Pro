// ============================================================================
//  edgevision_pro_main.cpp —— EdgeVision Pro 真实链路 Demo
//
//  2026-10-24 重写：彻底移除假 zero_copy_pipeline.hpp 仿真桩（fd 硬编码
//  1000+i，"Memory Bandwidth Saved" 是编造数字），换成真实链路：
//    1. MPP 硬件 JPEG 解码 (MppJpegDecoder, 实测 p50 1.03ms, 7.6x vs 软解)
//    2. NV12 -> RGB888 (CPU 定点 BT.601 full-range, ~1ms)
//    3. letterbox 640x640 (OpenCV, 灰边 114, 与 eval_dump.cpp 口径一致)
//    4. RKNN NPU 推理 (YOLOv8n DFL-Free INT8)
//    5. 检测框还原原图坐标 -> BEV 地面投影 (BEVProjector 纯几何)
//
//  用法: edgevision_pro <jpg路径>
//    无参数时打印用法并退出 1, 不跑任何合成/仿真数据。
//  模型: 环境变量 EV_MODEL 可覆盖, 默认 data/models/yolov8n_dflfree_i8.rknn
// ============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <algorithm>
#include <cmath>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "mpp_jpeg_decoder.h"
#include "bev_projector.hpp"
#include "rknn_engine_dfl_free.h"

using namespace edgevision;

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    if (sz <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return f.good();
}

// BEV 尺寸先验用的类别名 (person/car 等有专门先验, 其余走通用先验)
static std::string cls_name(int id) {
    static const char* names[] = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus",
        "train", "truck", "boat",
    };
    if (id >= 0 && id <= 8) return names[id];
    return "cls" + std::string(id < 10 ? "0" : "") + std::to_string(id);
}

int main(int argc, char** argv) {
    std::cout << "==================================================================" << std::endl;
    std::cout << "EdgeVision Pro: MPP HW-decode + YOLOv8n DFL-Free + BEV Ground Projection" << std::endl;
    std::cout << "Platform Target: Rockchip RK3588 (Tri-Core NPU 6.0 TOPS)" << std::endl;
    std::cout << "==================================================================" << std::endl;
    std::cout << "[NOTICE] 真实链路 demo (2026-10-24 重写), 旧版 zero-copy 仿真已删除"
              << " (违反 AGENTS.md 约定 3: 不许用假数据)" << std::endl;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <jpg_path>" << std::endl;
        std::cerr << "  本程序只处理真实图像输入, 不含任何合成/仿真数据。" << std::endl;
        return 1;
    }
    const std::string jpg_path = argv[1];

    // ------------------------------------------------------------------
    // 1. 读入真实 JPEG
    // ------------------------------------------------------------------
    std::vector<uint8_t> jpeg;
    if (!read_file(jpg_path, jpeg)) {
        std::cerr << "[FAIL] cannot read jpg: " << jpg_path << std::endl;
        return 1;
    }
    std::cout << "[+] Loaded JPEG: " << jpg_path << " (" << jpeg.size() << " bytes)" << std::endl;

    // ------------------------------------------------------------------
    // 2. MPP 硬件解码 (真实 DRM buffer, 实测 p50 1.03ms)
    // ------------------------------------------------------------------
    MppJpegDecoder dec;
    if (dec.init(1280, 720) != 0) {
        std::cerr << "[FAIL] MppJpegDecoder init: " << dec.last_error() << std::endl;
        return 1;
    }

    DecodedFrame frame;
    auto t0 = std::chrono::high_resolution_clock::now();
    int rc = dec.decode(jpeg.data(), jpeg.size(), &frame);
    auto t1 = std::chrono::high_resolution_clock::now();
    double decode_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (rc != 0 || !frame.valid) {
        std::cerr << "[FAIL] MPP decode failed rc=" << rc
                  << " valid=" << frame.valid
                  << " err=" << dec.last_error() << std::endl;
        return 1;
    }
    std::cout << "[+] MPP HW decode: " << frame.width << "x" << frame.height
              << " NV12, hor_stride=" << frame.hor_stride
              << ", decode_ms=" << std::fixed << std::setprecision(3) << decode_ms
              << " (drm_fd=" << dec.get_dma_fd() << ")" << std::endl;

    // ------------------------------------------------------------------
    // 3. NV12 -> RGB888 (CPU 定点 BT.601 full-range) + letterbox 640x640
    //    (run_inference 不做缩放, 必须外部做; 口径与 eval_dump.cpp 一致)
    // ------------------------------------------------------------------
    std::vector<uint8_t> rgb(static_cast<size_t>(frame.width) * frame.height * 3);
    auto t2 = std::chrono::high_resolution_clock::now();
    dec.to_rgb888(frame, rgb.data());
    auto t3 = std::chrono::high_resolution_clock::now();
    double csc_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::cout << "[+] NV12->RGB888 CSC: " << std::fixed << std::setprecision(3) << csc_ms << " ms" << std::endl;

    // cv::Mat 包装 (不拷贝) -> letterbox 到 640x640 灰边 114
    cv::Mat src(frame.height, frame.width, CV_8UC3, rgb.data());
    cv::Mat lb;
    float scale = 0.f;
    int pad_x = 0, pad_y = 0;
    auto t4 = std::chrono::high_resolution_clock::now();
    {
        float s = std::min((float)640 / src.cols, (float)640 / src.rows);
        int nw = (int)(src.cols * s), nh = (int)(src.rows * s);
        cv::Mat resized;
        cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
        lb = cv::Mat(640, 640, CV_8UC3, cv::Scalar(114, 114, 114));
        pad_x = (640 - nw) / 2;
        pad_y = (640 - nh) / 2;
        resized.copyTo(lb(cv::Rect(pad_x, pad_y, nw, nh)));
        scale = s;
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double lb_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();
    std::cout << "[+] Letterbox 640: scale=" << scale
              << " pad=(" << pad_x << "," << pad_y << ")"
              << " " << std::fixed << std::setprecision(3) << lb_ms << " ms" << std::endl;

    // ------------------------------------------------------------------
    // 4. NPU 推理 (真实模型)
    // ------------------------------------------------------------------
    const char* model_env = std::getenv("EV_MODEL");
    std::string model_path = model_env ? model_env : "data/models/yolov8n_dflfree_rk3588_i8.rknn";
    RKNNEngineDflFree npu;
    if (!npu.init(model_path)) {
        std::cerr << "[FAIL] RKNN init: " << model_path
                  << " (设置 EV_MODEL 环境变量指定模型路径)" << std::endl;
        return 1;
    }

    std::vector<Detection> dets;
    auto t6 = std::chrono::high_resolution_clock::now();
    bool ok = npu.run_inference(lb.data, 640, 640, dets);
    auto t7 = std::chrono::high_resolution_clock::now();
    double npu_ms = std::chrono::duration<double, std::milli>(t7 - t6).count();
    if (!ok) {
        std::cerr << "[FAIL] inference failed" << std::endl;
        return 1;
    }
    std::cout << "[+] NPU inference: " << dets.size() << " detections, "
              << std::fixed << std::setprecision(2) << npu_ms << " ms" << std::endl;

    // ------------------------------------------------------------------
    // 5. 检测框从 letterbox 坐标还原到原图 1280x720, 再 BEV 地面投影
    // ------------------------------------------------------------------
    BEVProjector bev(1280, 720, 65.0f, 1.6f, 8.0f);
    std::cout << "------------------------------------------------------------------" << std::endl;
    std::cout << "  idx  class          conf    bbox(orig 1280x720)         ground X(m)  Y(m)  dist(m)" << std::endl;
    int bev_count = 0;
    for (size_t i = 0; i < dets.size(); ++i) {
        const Detection& d = dets[i];
        float ox1 = (d.x1 - pad_x) / scale;
        float oy1 = (d.y1 - pad_y) / scale;
        float ox2 = (d.x2 - pad_x) / scale;
        float oy2 = (d.y2 - pad_y) / scale;
        Box3D box3d;
        bool ground = bev.estimate_3d_box(ox1, oy1, ox2, oy2, cls_name(d.class_id), box3d);
        std::cout << "  " << std::setw(3) << i << "  "
                  << std::setw(12) << std::left << cls_name(d.class_id) << std::right
                  << "  " << std::fixed << std::setprecision(3) << d.score
                  << "  (" << std::setw(4) << (int)ox1 << "," << std::setw(4) << (int)oy1
                  << "," << std::setw(4) << (int)ox2 << "," << std::setw(4) << (int)oy2 << ")";
        if (ground) {
            std::cout << "  " << std::fixed << std::setprecision(2)
                      << std::setw(9) << box3d.longitudinal_m
                      << std::setw(8) << box3d.lateral_m
                      << std::setw(8) << box3d.distance_m;
            ++bev_count;
        } else {
            std::cout << "         (no ground contact)";
        }
        std::cout << std::endl;
    }
    std::cout << "------------------------------------------------------------------" << std::endl;
    std::cout << "[DONE] " << dets.size() << " dets, " << bev_count
              << " with valid BEV projection. pipeline: decode "
              << std::fixed << std::setprecision(2) << decode_ms
              << " + csc " << csc_ms << " + letterbox " << lb_ms
              << " + npu " << npu_ms
              << " = " << (decode_ms + csc_ms + lb_ms + npu_ms) << " ms" << std::endl;

    return 0;
}
