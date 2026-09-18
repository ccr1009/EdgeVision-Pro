#ifndef EDGEVISION_RKNN_ENGINE_H
#define EDGEVISION_RKNN_ENGINE_H

#include <string>
#include <vector>
#include <memory>
#include "postprocess.h"

class RKNNEngine {
public:
    RKNNEngine();
    ~RKNNEngine();

    bool init(const std::string& model_path);
    bool run_inference(const unsigned char* rgb_data, int width, int height, std::vector<Detection>& detections);
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

#endif // EDGEVISION_RKNN_ENGINE_H
