#include "rknn_engine_dfl_free.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <cstring>
#include <vector>

#ifdef ENABLE_RKNN
#include "rknn_api.h"
#endif

#include "postprocess_dfl_free.h"

struct RKNNEngineDflFree::Impl {
    bool is_initialized = false;
    int input_w = 640;
    int input_h = 640;

    // 输出布局（RKNN 转换后可能是 (1,64,8400) 也可能是 (1,8400,64)）
    bool box_nhwc = false;
    bool cls_nhwc = false;
    int box_out_idx = 0;
    int cls_out_idx = 1;
    double infer_ms = 0.0;
    double decode_ms = 0.0;

#ifdef ENABLE_RKNN
    rknn_context ctx = 0;
    rknn_input_output_num io_num{};
#endif
};

#ifdef ENABLE_RKNN
namespace {
// 只在 RK3588 构建里存在：把平台无关的 NpuCore 映射成 RKNN 的核掩码
rknn_core_mask to_rknn_mask(NpuCore core) {
    switch (core) {
        case NpuCore::Core0: return RKNN_NPU_CORE_0;
        case NpuCore::Core1: return RKNN_NPU_CORE_1;
        case NpuCore::Core2: return RKNN_NPU_CORE_2;
        default:             return RKNN_NPU_CORE_AUTO;
    }
}
}  // namespace
#endif

RKNNEngineDflFree::RKNNEngineDflFree() : pImpl(std::make_unique<Impl>()) {}
RKNNEngineDflFree::~RKNNEngineDflFree() { release(); }

double RKNNEngineDflFree::last_infer_ms() const { return pImpl->infer_ms; }
double RKNNEngineDflFree::last_decode_ms() const { return pImpl->decode_ms; }

bool RKNNEngineDflFree::init(const std::string& model_path, NpuCore core) {
    std::cout << "[RKNNEngineDflFree] Initializing: " << model_path << std::endl;

    std::ifstream file(model_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[-] Failed to open model: " << model_path << std::endl;
        return false;
    }
    const size_t model_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<char> buf(model_size);
    if (!file.read(buf.data(), static_cast<std::streamsize>(model_size))) {
        std::cerr << "[-] Failed to read model data" << std::endl;
        return false;
    }

    // 读文件只是「拿数据的手段」，真正的初始化在内存版本里。
    // 这样 3 个 worker 可以共享同一份 g_model_data，不必各读一次盘。
    return init_from_memory(buf.data(), buf.size(), core);
}

bool RKNNEngineDflFree::init_from_memory(const void* model_data, size_t size, NpuCore core) {
#ifdef ENABLE_RKNN
    // rknn_init 要的是非 const 指针，但它不会改写这块内存
    int ret = rknn_init(&pImpl->ctx, const_cast<void*>(model_data),
                        static_cast<uint32_t>(size), 0, nullptr);
    if (ret < 0) {
        std::cerr << "[-] rknn_init failed: " << ret << std::endl;
        return false;
    }
    rknn_set_core_mask(pImpl->ctx, to_rknn_mask(core));
    std::cout << "[+] core_mask=" << static_cast<int>(core)
              << (core == NpuCore::Auto ? " (AUTO，驱动调度)" : " (物理核硬绑定)") << std::endl;

    ret = rknn_query(pImpl->ctx, RKNN_QUERY_IN_OUT_NUM, &pImpl->io_num, sizeof(pImpl->io_num));
    if (ret < 0) {
        std::cerr << "[-] rknn_query(IO_NUM) failed" << std::endl;
        return false;
    }
    std::cout << "[+] Inputs=" << pImpl->io_num.n_input << " Outputs=" << pImpl->io_num.n_output << std::endl;

    // 输入属性
    rknn_tensor_attr in_attr{};
    in_attr.index = 0;
    rknn_query(pImpl->ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    if (in_attr.fmt == RKNN_TENSOR_NHWC && in_attr.n_dims == 4) {
        pImpl->input_h = static_cast<int>(in_attr.dims[1]);
        pImpl->input_w = static_cast<int>(in_attr.dims[2]);
    }
    std::cout << "[+] Input " << pImpl->input_w << "x" << pImpl->input_h
              << " fmt=" << in_attr.fmt << " type=" << in_attr.type << std::endl;

    // 输出属性：按通道数识别哪个是 raw_box(64) / cls_sig(80)
    pImpl->box_out_idx = -1;
    pImpl->cls_out_idx = -1;
    for (uint32_t i = 0; i < pImpl->io_num.n_output; ++i) {
        rknn_tensor_attr a{};
        a.index = i;
        if (rknn_query(pImpl->ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a)) < 0) continue;

        // 找出「通道维」与「anchor 维」：anchor 维必须是 8400
        int ch = -1;
        bool nhwc = false;
        for (int d = 0; d < a.n_dims; ++d) {
            if (a.dims[d] == 8400) {
                // 另一维是通道；若最后一维是 8400 -> (1, C, 8400) 即 NCHW 风格
                nhwc = (d != a.n_dims - 1);
                ch = static_cast<int>(nhwc ? a.dims[a.n_dims - 1] : a.dims[a.n_dims - 2]);
                break;
            }
        }
        if (ch == 64) { pImpl->box_out_idx = static_cast<int>(i); pImpl->box_nhwc = nhwc; }
        else if (ch == 80) { pImpl->cls_out_idx = static_cast<int>(i); pImpl->cls_nhwc = nhwc; }

        std::cout << "    out[" << i << "] name=" << a.name << " dims=[";
        for (int d = 0; d < a.n_dims; ++d) std::cout << a.dims[d] << (d + 1 < a.n_dims ? "," : "");
        std::cout << "] ch=" << ch << " nhwc=" << nhwc << std::endl;
    }
    if (pImpl->box_out_idx < 0 || pImpl->cls_out_idx < 0) {
        std::cerr << "[-] 无法识别输出张量（需要 64ch 与 80ch 各一个）" << std::endl;
        return false;
    }

    pImpl->is_initialized = true;
    return true;
#else
    (void)model_data; (void)size; (void)core;  // x86 Stub：没有 NPU
    std::cout << "[!] x86 Stub Mode (no NPU)" << std::endl;
    pImpl->is_initialized = true;
    return true;
#endif
}

bool RKNNEngineDflFree::run_inference(const unsigned char* rgb_data, int width, int height,
                                      std::vector<Detection>& detections) {
    if (!pImpl->is_initialized) return false;

#ifdef ENABLE_RKNN
    rknn_input in{};
    in.index = 0;
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    in.size = static_cast<uint32_t>(width * height * 3);
    in.buf = const_cast<unsigned char*>(rgb_data);
    if (rknn_inputs_set(pImpl->ctx, 1, &in) < 0) return false;

    auto t0 = std::chrono::high_resolution_clock::now();
    if (rknn_run(pImpl->ctx, nullptr) < 0) return false;
    auto t1 = std::chrono::high_resolution_clock::now();
    pImpl->infer_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::vector<rknn_output> outs(pImpl->io_num.n_output);
    for (auto& o : outs) { memset(&o, 0, sizeof(o)); o.want_float = 1; }
    if (rknn_outputs_get(pImpl->ctx, pImpl->io_num.n_output, outs.data(), nullptr) < 0) return false;

    auto t2 = std::chrono::high_resolution_clock::now();
    dflfree::decode(static_cast<const float*>(outs[pImpl->box_out_idx].buf),
                    static_cast<const float*>(outs[pImpl->cls_out_idx].buf),
                    pImpl->box_nhwc, pImpl->cls_nhwc,
                    pImpl->input_w,
                    0.25f, 0.45f, detections);
    auto t3 = std::chrono::high_resolution_clock::now();
    pImpl->decode_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    rknn_outputs_release(pImpl->ctx, pImpl->io_num.n_output, outs.data());
    return true;
#else
    // x86 Stub：没有 NPU。按 AGENTS.md 约定 3, 不产生任何假检测框 ——
    // 直接返回 false, 调用方应明确知道本平台无真实推理能力。
    (void)rgb_data; (void)width; (void)height;
    return false;
#endif
}

void RKNNEngineDflFree::release() {
#ifdef ENABLE_RKNN
    if (pImpl->ctx != 0) {
        rknn_destroy(pImpl->ctx);
        pImpl->ctx = 0;
    }
#endif
    pImpl->is_initialized = false;
}
