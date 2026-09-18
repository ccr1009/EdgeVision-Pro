#include "postprocess.h"

static float compute_iou(const Detection& a, const Detection& b) {
    float xx1 = std::max(a.x1, b.x1);
    float yy1 = std::max(a.y1, b.y1);
    float xx2 = std::min(a.x2, b.x2);
    float yy2 = std::min(a.y2, b.y2);

    float w = std::max(0.0f, xx2 - xx1);
    float h = std::max(0.0f, yy2 - yy1);
    float inter = w * h;

    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float union_area = area_a + area_b - inter;

    return (union_area <= 0.0f) ? 0.0f : (inter / union_area);
}

void nms_sorted_boxes(const std::vector<Detection>& in_boxes,
                      std::vector<Detection>& out_boxes,
                      float nms_threshold) {
    std::vector<bool> suppressed(in_boxes.size(), false);

    for (size_t i = 0; i < in_boxes.size(); ++i) {
        if (suppressed[i]) continue;
        out_boxes.push_back(in_boxes[i]);

        for (size_t j = i + 1; j < in_boxes.size(); ++j) {
            if (suppressed[j]) continue;
            if (in_boxes[i].class_id != in_boxes[j].class_id) continue;

            if (compute_iou(in_boxes[i], in_boxes[j]) > nms_threshold) {
                suppressed[j] = true;
            }
        }
    }
}

void decode_yolov8_outputs(const float* output_data,
                           int num_channels,
                           int num_anchors,
                           int img_w,
                           int img_h,
                           float conf_threshold,
                           float nms_threshold,
                           std::vector<Detection>& detections) {
    // YOLOv8 output tensor format: [1, 84, 8400]
    // 84 channels: [cx, cy, w, h, score_cls0, ... score_cls79]
    int num_classes = num_channels - 4;
    std::vector<Detection> candidates;

    for (int i = 0; i < num_anchors; ++i) {
        float cx = output_data[0 * num_anchors + i];
        float cy = output_data[1 * num_anchors + i];
        float w  = output_data[2 * num_anchors + i];
        float h  = output_data[3 * num_anchors + i];

        // Find max class score
        float max_score = -1.0f;
        int max_cls = -1;
        for (int c = 0; c < num_classes; ++c) {
            float score = output_data[(4 + c) * num_anchors + i];
            if (score > max_score) {
                max_score = score;
                max_cls = c;
            }
        }

        if (max_score >= conf_threshold) {
            float x1 = std::max(0.0f, cx - w * 0.5f);
            float y1 = std::max(0.0f, cy - h * 0.5f);
            float x2 = std::min((float)img_w, cx + w * 0.5f);
            float y2 = std::min((float)img_h, cy + h * 0.5f);

            Detection det;
            det.x1 = x1;
            det.y1 = y1;
            det.x2 = x2;
            det.y2 = y2;
            det.score = max_score;
            det.class_id = max_cls;
            candidates.push_back(det);
        }
    }

    // Sort descending by score
    std::sort(candidates.begin(), candidates.end(), [](const Detection& a, const Detection& b) {
        return a.score > b.score;
    });

    // Run NMS
    nms_sorted_boxes(candidates, detections, nms_threshold);
}
