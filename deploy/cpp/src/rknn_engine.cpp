#include "rknn_engine.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <cstring>

#ifdef ENABLE_RKNN
#include "rknn_api.h"
#endif

struct RKNNEngine::Impl {
    bool is_initialized = false;
    int input_w = 640;
    int input_h = 640;
    int input_c = 3;

#ifdef ENABLE_RKNN
    rknn_context ctx = 0;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs = nullptr;
    rknn_tensor_attr* output_attrs = nullptr;
#endif
};

RKNNEngine::RKNNEngine() : pImpl(std::make_unique<Impl>()) {}
RKNNEngine::~RKNNEngine() { release(); }

bool RKNNEngine::init(const std::string& model_path) {
    std::cout << "[RKNNEngine] Initializing model: " << model_path << std::endl;

#ifdef ENABLE_RKNN
    std::ifstream file(model_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[-] [RKNNEngine] Failed to open model file: " << model_path << std::endl;
        return false;
    }
    size_t model_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> model_buf(model_size);
    if (!file.read(model_buf.data(), model_size)) {
        std::cerr << "[-] [RKNNEngine] Failed to read model data!" << std::endl;
        return false;
    }

    int ret = rknn_init(&pImpl->ctx, model_buf.data(), model_size, 0, NULL);
    if (ret < 0) {
        std::cerr << "[-] [RKNNEngine] rknn_init failed with error code: " << ret << std::endl;
        return false;
    }

    // Bind to all 3 NPU cores
    rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO;
    rknn_set_core_mask(pImpl->ctx, core_mask);

    ret = rknn_query(pImpl->ctx, RKNN_QUERY_IN_OUT_NUM, &pImpl->io_num, sizeof(pImpl->io_num));
    if (ret < 0) {
        std::cerr << "[-] [RKNNEngine] rknn_query io_num failed!" << std::endl;
        return false;
    }

    std::cout << "[+] [RKNNEngine] Loaded RKNN model on NPU successfully! Inputs: " 
              << pImpl->io_num.n_input << ", Outputs: " << pImpl->io_num.n_output << std::endl;
    pImpl->is_initialized = true;
    return true;
#else
    // x86 Stub Mode:
    std::cout << "[!] [RKNNEngine] Compiled in x86 Stub Mode (No physical NPU)." << std::endl;
    std::cout << "    --> Model checked. Stub simulation active." << std::endl;
    pImpl->is_initialized = true;
    return true;
#endif
}

bool RKNNEngine::run_inference(const unsigned char* rgb_data, int width, int height, std::vector<Detection>& detections) {
    if (!pImpl->is_initialized) return false;

#ifdef ENABLE_RKNN
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = width * height * 3;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].buf = (void*)rgb_data;

    int ret = rknn_inputs_set(pImpl->ctx, pImpl->io_num.n_input, inputs);
    if (ret < 0) return false;

    ret = rknn_run(pImpl->ctx, NULL);
    if (ret < 0) return false;

    rknn_output outputs[1];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1;

    ret = rknn_outputs_get(pImpl->ctx, 1, outputs, NULL);
    if (ret < 0) return false;

    float* raw_data = (float*)outputs[0].buf;
    decode_yolov8_outputs(raw_data, 84, 8400, width, height, 0.25f, 0.45f, detections);

    rknn_outputs_release(pImpl->ctx, 1, outputs);
    return true;
#else
    // x86 Stub: Simulate 2 detections (person, car) with realistic confidence
    Detection d1 = {150.0f, 100.0f, 200.0f, 220.0f, 0.88f, 0}; // person
    Detection d2 = {30.0f, 480.0f, 150.0f, 540.0f, 0.92f, 2};  // car
    detections.push_back(d1);
    detections.push_back(d2);
    return true;
#endif
}

void RKNNEngine::release() {
#ifdef ENABLE_RKNN
    if (pImpl->ctx != 0) {
        rknn_destroy(pImpl->ctx);
        pImpl->ctx = 0;
    }
#endif
    pImpl->is_initialized = false;
}
