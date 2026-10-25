// ============================================================================
//  mpp_jpeg_decoder.h —— RK3588 MPP 硬件 MJPEG/JPEG 解码器封装
//
//  背景 / 为什么需要这个类:
//    RK3588 板子上系统自带的 librockchip_mpp.so.0 (v1.5.0, 2021-05) 与本板
//    MPP 内核驱动语义不匹配 —— mpp_put_packet() 内部的 mpp_poll(MPP_PORT_INPUT,
//    mInputTimeout) 会立即返回负值, 被翻译成 MPP_ERR_BUFFER_FULL(-1012),
//    导致 MJPEG 硬解永远失败。必须使用与源码同源编译的库
//    (third_party/mpp/, 来自 github.com/rockchip-linux/mpp)。
//
//  正确用法要点 (缺一个就报 -1012 或永远拿不到帧):
//    1. mpp_init(MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG)
//    2. MPP_DEC_GET_CFG -> "base:split_parse"=1 -> MPP_DEC_SET_CFG
//       (MJPEG 必须开 split_parse, 否则解析器不切帧)
//    3. MPP_DEC_SET_OUTPUT_FORMAT = MPP_FMT_YUV420SP (NV12)
//    4. **预分配输出 MppFrame + MppBuffer, 每帧 mpp_meta_set_frame(meta,
//       KEY_OUTPUT_FRAME, frame)** —— 这是 advanced 模式的硬性要求;
//       MJPEG 解码器不会自己分配输出帧, 缺这一步 mpp_poll 立刻返回失败
//    5. 输入 packet 必须用 mpp_packet_init_with_buffer() 走 DRM 零拷贝,
//       不能 mpp_packet_init() + set_pos(普通 malloc 指针)
//
//  实测性能 (EVM3588-B, 1280x720 MJPEG -> NV12, N=200):
//    put    avg 1.044 ms  p50 1.015  p95 1.165  p99 1.414
//    get    avg 0.017 ms  p50 0.012
//    total  avg 1.061 ms  p50 1.027  p95 1.181  p99 1.426
//    对比 OpenCV cv::imdecode 软解 7.761 ms/p50  =>  7.6x 加速
//    200/200 帧有效, 解码期间 CPU 占用 <2% (确认为真硬件解码)
//
//  正确性 (vs OpenCV full-range 软解, 9 张真实图):
//    Y 平面 PSNR 49.9~77.2 dB, 逐字节相同率 97~99.9%
//    注: MPP 输出 JPEG 原生 full-range YCbCr (Y 0~255), 与 cv::cvtColor
//        默认的 BT.601 limited-range (16~235) 不是同一口径, 比对时必须
//        两边都转 full-range, 否则会误判为 PSNR 30dB 的"解码错误"
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "rk_mpi.h"

namespace edgevision {

/// 一次解码的结果视图。数据指针指向 MPP 内部 DRM buffer, 生命周期由
/// MppJpegDecoder 持有 —— 在调用 decode() 之前一直有效, 调用后失效。
struct DecodedFrame {
    const uint8_t* data = nullptr;  ///< NV12 起始地址 (Y 平面)
    int width = 0;
    int height = 0;
    int hor_stride = 0;   ///< 行跨度 (字节), 可能 > width
    int ver_stride = 0;   ///< 行数, 可能 > height
    int format = 0;       ///< MppFrameFormat, 期望 MPP_FMT_YUV420SP(0)
    bool valid = false;   ///< 本帧是否 err==0 && discard==0
    double decode_ms = 0.0;
};

class MppJpegDecoder {
public:
    MppJpegDecoder();
    ~MppJpegDecoder();

    MppJpegDecoder(const MppJpegDecoder&) = delete;
    MppJpegDecoder& operator=(const MppJpegDecoder&) = delete;

    /// 初始化解码上下文。已自动完成 create/init/split_parse/output_format。
    /// @param max_width  预分配输出帧的最大宽度 (建议用摄像头分辨率)
    /// @param max_height 预分配输出帧的最大高度
    /// @return 0 成功, 负数见 mpp_err.h
    int init(int max_width, int max_height);

    /// 解码一帧 JPEG/MJPEG。
    /// @param jpeg_data  完整 JPEG 数据 (含 SOI/EOI)
    /// @param jpeg_size  字节数
    /// @param out        输出结果
    /// @return 0 成功且拿到有效帧; <0 失败; 1 拿到帧但被标记 discard
    int decode(const uint8_t* jpeg_data, size_t jpeg_size, DecodedFrame* out);

    /// 把 NV12 按 stride 裁剪并转成紧凑 RGB888 (供 RKNN 或 OpenCV 使用)。
    /// 这是 CPU 侧拷贝, 会带来 ~1ms 开销; 若要零拷贝, 应把 DRM fd 直接
    /// 交给 RGA 或 rknn_create_mem_from_fd()。
    /// @param out_rgb    调用方提供, 至少 width*height*3 字节
    void to_rgb888(const DecodedFrame& f, uint8_t* out_rgb) const;

    /// 获取 MPP 内部 DRM buffer 的 fd (用于下游零拷贝)。
    int get_dma_fd() const;

    /// 最近一次错误信息
    const std::string& last_error() const { return last_error_; }

    bool is_ready() const { return ctx_ != nullptr; }

private:
    void cleanup();

    MppCtx ctx_ = nullptr;
    MppApi* mpi_ = nullptr;
    MppFrame frame_ = nullptr;      ///< 预分配输出帧 (advanced 模式必须)
    MppBuffer frm_buf_ = nullptr;
    MppBuffer in_buf_ = nullptr;
    MppBufferGroup frm_grp_ = nullptr;
    MppBufferGroup in_grp_ = nullptr;

    int max_width_ = 0;
    int max_height_ = 0;
    size_t in_buf_size_ = 0;
    std::string last_error_;
};

}  // namespace edgevision
