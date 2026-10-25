#ifndef EDGEVISION_POSTPROCESS_DFL_FREE_H
#define EDGEVISION_POSTPROCESS_DFL_FREE_H

// ==============================================================================
// YOLOv8 DFL-Free 后处理
// ------------------------------------------------------------------------------
// 配套模型: yolov8n_dflfree.onnx -> yolov8n_dflfree_rk3588_i8.rknn
//   输出 0: raw_box  (1, 64, 8400)  3 尺度 box 分布 concat（DFL 前，未 softmax）
//   输出 1: cls_sig  (1, 80, 8400)  cls 概率（模型里已 sigmoid）
//
// RKNN 转换后输出布局可能变成 (1, 8400, 64) / (1, 8400, 80)（NHWC 化），
// 解码前用 rknn_tensor_attr 的 dims 判断，传 box_nhwc / cls_nhwc。
//
// 解码要点（务必与 convert/export_dflfree_onnx.py 的 numpy 版保持一致）：
//   1. dist 是**格子单位**（0~15），必须乘 stride 才是像素 —— 最初差了 8 倍就是这里
//   2. DFL: 16 个 bin 做 softmax，与 [0..15] 加权求和 -> 4 个距离
//   3. x1y1 = (anchor_grid - dist_lt) * stride ; x2y2 = (anchor_grid + dist_rb) * stride
//   4. 先按 cls 置信度筛 anchor，再只对通过的做 softmax（8400 次 softmax 太浪费）
// ==============================================================================

#include <vector>
#include <cstdint>
#include "postprocess.h"   // 复用 Detection 结构

namespace dflfree {

constexpr int kRegMax = 16;       // DFL 分箱数
constexpr int kNumAnchors = 8400; // 80x80 + 40x40 + 20x20
constexpr int kNumClasses = 80;

// 预计算 anchor（格子单位）与 stride，进程内只需初始化一次
struct AnchorTable {
    float xy[kNumAnchors][2];   // (x+0.5, y+0.5)，格子单位
    float stride[kNumAnchors];
    AnchorTable();
};
const AnchorTable& anchors();

/**
 * @param raw_box   (64,8400) 或 (8400,64)
 * @param cls_sig   (80,8400) 或 (8400,80)
 * @param box_nhwc  raw_box 是否为 (8400,64) 布局
 * @param cls_nhwc  cls_sig 是否为 (8400,80) 布局
 * @param imgsz     模型输入边长（640），用于把像素坐标从 letterbox 还原到原图时使用
 */
void decode(const float* raw_box, const float* cls_sig,
            bool box_nhwc, bool cls_nhwc,
            int imgsz,
            float conf_threshold, float nms_threshold,
            std::vector<Detection>& detections,
            int* out_num_candidates = nullptr);

}  // namespace dflfree

#endif  // EDGEVISION_POSTPROCESS_DFL_FREE_H
