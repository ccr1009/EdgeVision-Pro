#ifndef EDGEVISION_RKNN_ENGINE_DFL_FREE_H
#define EDGEVISION_RKNN_ENGINE_DFL_FREE_H

// DFL-Free 版推理引擎：模型只输出 raw_box(64ch) + cls_sig(80ch)，
// DFL 解码与 NMS 全在本机 CPU 侧完成（见 postprocess_dfl_free.cpp）。
// 目的：干掉原模型里回退到 CPU 的 DFL 链（实测 18.44ms，占单帧 49%）。

#include <string>
#include <vector>
#include <memory>
#include "postprocess.h"

/**
 * NPU 核心选择。
 * 刻意不直接用 rknn_core_mask：rknn_api.h 只挂在 RK3588 分支的 include path 上，
 * x86 模拟构建根本拿不到它。用自定义枚举把 SDK 类型挡在 .cpp 内部，公共头文件保持平台无关。
 */
enum class NpuCore {
    Auto  = -1,   // 交给驱动调度（单 context / 跑 bench 时用）
    Core0 = 0,
    Core1 = 1,
    Core2 = 2,
};

class RKNNEngineDflFree {
public:
    RKNNEngineDflFree();
    ~RKNNEngineDflFree();

    /** @param core 多线程并行时必须各自绑一个物理核，否则全挤在默认核上、不报错只变慢 */
    bool init(const std::string& model_path, NpuCore core = NpuCore::Auto);

    /**
     * 从内存初始化。
     * 多个 worker 线程共享同一份模型数据时用这个，避免每个线程各读一次盘。
     * @note model_data 指向的内存需在引擎生命周期内保持有效（rknn_init 会引用它）。
     */
    bool init_from_memory(const void* model_data, size_t size,
                          NpuCore core = NpuCore::Auto);

    /** @param rgb_data RGB888, 尺寸必须等于模型输入边长（默认 640x640） */
    bool run_inference(const unsigned char* rgb_data, int width, int height,
                       std::vector<Detection>& detections);

    /** 纯推理耗时（不含后处理），用于和旧模型对比 */
    double last_infer_ms() const;
    double last_decode_ms() const;

    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

#endif  // EDGEVISION_RKNN_ENGINE_DFL_FREE_H
