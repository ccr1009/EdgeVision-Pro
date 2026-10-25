// ============================================================================
//  mpp_jpeg_decoder.cpp —— MPP 硬件 JPEG/MJPEG 解码器实现
//  详见 include/mpp_jpeg_decoder.h 里的用法要点与实测数据
// ============================================================================
#include "mpp_jpeg_decoder.h"

#include <cstring>
#include <cstdio>
#include <sys/time.h>

namespace edgevision {

namespace {
int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}
inline int align(int x, int a) { return (x + a - 1) & ~(a - 1); }
}  // namespace

MppJpegDecoder::MppJpegDecoder() = default;

MppJpegDecoder::~MppJpegDecoder() { cleanup(); }

int MppJpegDecoder::init(int max_width, int max_height) {
    cleanup();
    max_width_ = max_width;
    max_height_ = max_height;

    MPP_RET ret = mpp_create(&ctx_, &mpi_);
    if (ret) { last_error_ = "mpp_create failed ret=" + std::to_string(ret); return ret; }

    // MJPEG 必须使用 MPP_VIDEO_CodingMJPEG
    ret = mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret) { last_error_ = "mpp_init failed ret=" + std::to_string(ret); cleanup(); return ret; }

    // MJPEG 必须开 split_parse
    MppDecCfg cfg = nullptr;
    mpp_dec_cfg_init(&cfg);
    mpi_->control(ctx_, MPP_DEC_GET_CFG, cfg);
    mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
    ret = mpi_->control(ctx_, MPP_DEC_SET_CFG, cfg);
    if (ret) { last_error_ = "set split_parse failed ret=" + std::to_string(ret); cleanup(); return ret; }

    // 输出 NV12
    MppFrameFormat fmt = MPP_FMT_YUV420SP;
    ret = mpi_->control(ctx_, MPP_DEC_SET_OUTPUT_FORMAT, &fmt);
    if (ret) { last_error_ = "set output format failed ret=" + std::to_string(ret); cleanup(); return ret; }

    // ★ 预分配输出帧 —— advanced 模式的核心 (缺这一步 put_packet 会 -1012)
    ret = mpp_frame_init(&frame_);
    if (ret) { last_error_ = "mpp_frame_init failed ret=" + std::to_string(ret); cleanup(); return ret; }

    ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_DRM);
    if (ret) { last_error_ = "frm buffer group failed ret=" + std::to_string(ret); cleanup(); return ret; }

    const int hstr = align(max_width_, 16);
    const int vstr = align(max_height_, 16);
    ret = mpp_buffer_get(frm_grp_, &frm_buf_,
                         static_cast<size_t>(hstr) * vstr * 4);  // 4x 保守 (支持 4:2:2)
    if (ret) { last_error_ = "frame buffer get failed ret=" + std::to_string(ret); cleanup(); return ret; }
    mpp_frame_set_buffer(frame_, frm_buf_);

    // 输入缓冲组 (零拷贝路径必须用 DRM buffer)
    ret = mpp_buffer_group_get_internal(&in_grp_, MPP_BUFFER_TYPE_DRM);
    if (ret) { last_error_ = "in buffer group failed ret=" + std::to_string(ret); cleanup(); return ret; }

    // 单帧上限按 max_width/8 估算 (1280x720 JPEG 约 60KB, 留足余量)
    in_buf_size_ = static_cast<size_t>(hstr) * vstr / 4;
    ret = mpp_buffer_get(in_grp_, &in_buf_, in_buf_size_);
    if (ret) { last_error_ = "in buffer get failed ret=" + std::to_string(ret); cleanup(); return ret; }

    return 0;
}

int MppJpegDecoder::decode(const uint8_t* jpeg_data, size_t jpeg_size, DecodedFrame* out) {
    if (!ctx_ || !out) return -1;
    if (jpeg_size > in_buf_size_) {
        last_error_ = "jpeg too large: " + std::to_string(jpeg_size) +
                      " > " + std::to_string(in_buf_size_);
        return -2;
    }

    // 拷入 DRM 输入缓冲
    std::memcpy(mpp_buffer_get_ptr(in_buf_), jpeg_data, jpeg_size);

    MppPacket pkt = nullptr;
    MPP_RET ret = mpp_packet_init_with_buffer(&pkt, in_buf_);
    if (ret) { last_error_ = "packet init failed"; return ret; }
    mpp_packet_set_length(pkt, jpeg_size);
    mpp_packet_set_pos(pkt, mpp_buffer_get_ptr(in_buf_));

    // ★ 把预分配帧挂到 packet meta (advanced 模式硬性要求)
    MppMeta meta = mpp_packet_get_meta(pkt);
    if (meta) {
        mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, frame_);
    } else {
        last_error_ = "packet meta is NULL";
        mpp_packet_deinit(&pkt);
        return -3;
    }

    const int64_t t0 = now_us();
    ret = mpi_->decode_put_packet(ctx_, pkt);
    if (ret) {
        last_error_ = "decode_put_packet failed ret=" + std::to_string(ret) +
                      " (若为 -1012/MPP_ERR_BUFFER_FULL, 检查是否用了系统旧版 librockchip_mpp)";
        mpp_packet_deinit(&pkt);
        return ret;
    }

    // 轮询取帧
    int result = -4;
    MppFrame got = nullptr;
    for (int spin = 0; spin < 300; ++spin) {
        ret = mpi_->decode_get_frame(ctx_, &got);
        if (ret || !got) break;

        if (mpp_frame_get_info_change(got)) {
            mpi_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
            continue;
        }

        const int e = mpp_frame_get_errinfo(got);
        const int d = mpp_frame_get_discard(got);
        MppBuffer fb = mpp_frame_get_buffer(got);

        out->width = mpp_frame_get_width(got);
        out->height = mpp_frame_get_height(got);
        out->hor_stride = mpp_frame_get_hor_stride(got);
        out->ver_stride = mpp_frame_get_ver_stride(got);
        out->format = mpp_frame_get_fmt(got);
        out->decode_ms = (now_us() - t0) / 1000.0;
        out->data = fb ? static_cast<const uint8_t*>(mpp_buffer_get_ptr(fb)) : nullptr;
        out->valid = (e == 0 && d == 0 && out->data != nullptr);

        result = (e == 0) ? 0 : 1;

        mpi_->decode_get_frame(ctx_, nullptr);  // 释放
        break;
    }

    mpp_packet_deinit(&pkt);
    if (got == nullptr) last_error_ = "no frame produced (ret=" + std::to_string(result) + ")";
    return result;
}

void MppJpegDecoder::to_rgb888(const DecodedFrame& f, uint8_t* out_rgb) const {
    if (!f.data || !out_rgb) return;
    const int w = f.width, h = f.height;
    const int hs = f.hor_stride, vs = f.ver_stride;
    const uint8_t* y_plane = f.data;
    const uint8_t* uv_plane = f.data + static_cast<size_t>(hs) * vs;

    // BT.601 full-range: R = Y + 1.402*(V-128) ; G = Y - 0.344*(U-128) - 0.714*(V-128) ; B = Y + 1.772*(U-128)
    for (int j = 0; j < h; ++j) {
        const uint8_t* yrow = y_plane + static_cast<size_t>(j) * hs;
        const uint8_t* uvrow = uv_plane + static_cast<size_t>(j / 2) * hs;
        uint8_t* orow = out_rgb + static_cast<size_t>(j) * w * 3;
        for (int i = 0; i < w; ++i) {
            const int Y = yrow[i];
            const int U = uvrow[(i / 2) * 2] - 128;
            const int V = uvrow[(i / 2) * 2 + 1] - 128;
            int R = Y + ((1436 * V) >> 10);
            int G = Y - ((352 * U + 731 * V) >> 10);
            int B = Y + ((1815 * U) >> 10);
            orow[i * 3 + 0] = static_cast<uint8_t>(R < 0 ? 0 : (R > 255 ? 255 : R));
            orow[i * 3 + 1] = static_cast<uint8_t>(G < 0 ? 0 : (G > 255 ? 255 : G));
            orow[i * 3 + 2] = static_cast<uint8_t>(B < 0 ? 0 : (B > 255 ? 255 : B));
        }
    }
}

int MppJpegDecoder::get_dma_fd() const {
    return frm_buf_ ? mpp_buffer_get_fd(frm_buf_) : -1;
}

void MppJpegDecoder::cleanup() {
    if (mpi_ && ctx_) mpi_->reset(ctx_);
    if (frame_) mpp_frame_deinit(&frame_);
    if (frm_buf_) mpp_buffer_put(frm_buf_);
    if (frm_grp_) mpp_buffer_group_put(frm_grp_);
    if (in_buf_) mpp_buffer_put(in_buf_);
    if (in_grp_) mpp_buffer_group_put(in_grp_);
    if (ctx_) mpp_destroy(ctx_);
    frame_ = nullptr;
    frm_buf_ = nullptr;
    frm_grp_ = nullptr;
    in_buf_ = nullptr;
    in_grp_ = nullptr;
    ctx_ = nullptr;
    mpi_ = nullptr;
}

}  // namespace edgevision
