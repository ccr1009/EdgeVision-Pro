#ifndef EDGEVISION_POSTPROCESS_H
#define EDGEVISION_POSTPROCESS_H

#include <vector>
#include <algorithm>
#include <cmath>

struct Detection {
    float x1;
    float y1;
    float x2;
    float y2;
    float score;
    int class_id;
};

void nms_sorted_boxes(const std::vector<Detection>& in_boxes,
                      std::vector<Detection>& out_boxes,
                      float nms_threshold);

void decode_yolov8_outputs(const float* output_data,
                           int num_channels,
                           int num_anchors,
                           int img_w,
                           int img_h,
                           float conf_threshold,
                           float nms_threshold,
                           std::vector<Detection>& detections);

#endif // EDGEVISION_POSTPROCESS_H
