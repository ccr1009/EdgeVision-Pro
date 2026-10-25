// ============================================================================
//  mpp_jpeg_encoder.cpp —— MPP 硬件 JPEG 编码器实现
//
//  设计原则: 与 mpp_jpeg_decoder.cpp 完全一致的错误处理范式 ——
//    每个 MPP 调用后 `if (ret) { last_error_ = "... ret=" + itoa(ret); cleanup(); return ret; }`
//    任何一个环节失败都把上下文彻底拆掉, 不留半初始化状态。
//
//  结论提醒: 本板硬编 JPEG 比软编慢 3%, 详见 mpp_jpeg_encoder.h 文件头。
//  保留完整实现是因为:
//    ① CPU 卸载 6.2% vs 99.7% 在多路高并发场景确实有意义;
//    ② "硬件编码器为什么没比 CPU 快"是能讲透的面试素材 (见头文件延迟模型);
//    ③ 后续若要做 4 路 4K 采集, 硬编池是唯一不把 CPU 打满的选择。
// ============================================================================
#include "mpp_jpeg_encoder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_log.h"
#include "mpp_packet.h"
#include "rk_venc_cfg.h"
#include "rk_venc_cmd.h"
#include "rk_venc_rc.h"

namespace edgevision {

namespace {

int64_t now_us() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::microseconds>(
               clk::now().time_since_epoch())
        .count();
}

inline int align(int x, int a) { return (x + a - 1) & ~(a - 1); }

inline int clamp_u8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

}  // namespace

// ===========================================================================
//  MppJpegEncoder
// ===========================================================================

MppJpegEncoder::MppJpegEncoder() = default;

MppJpegEncoder::~MppJpegEncoder() { cleanup(); }

void MppJpegEncoder::cleanup() {
    if (frame_) {
        mpp_frame_deinit(&frame_);
        frame_ = nullptr;
    }
    if (in_buf_) {
        mpp_buffer_put(in_buf_);
        in_buf_ = nullptr;
    }
    if (in_grp_) {
        mpp_buffer_group_put(in_grp_);
        in_grp_ = nullptr;
    }
    if (cfg_) {
        mpp_enc_cfg_deinit(cfg_);
        cfg_ = nullptr;
    }
    if (mpi_ && ctx_) {
        mpi_->reset(ctx_);
    }
    if (ctx_) {
        mpp_destroy(ctx_);
        ctx_ = nullptr;
    }
    mpi_ = nullptr;
    in_buf_size_ = 0;
}

int MppJpegEncoder::init(int width, int height, int q_factor) {
    if (width <= 0 || height <= 0) {
        last_error_ = "init: width/height must be > 0";
        return MPP_ERR_VALUE;
    }
    if (q_factor < 1 || q_factor > 99) {
        last_error_ = "init: q_factor out of range [1, 99]: " + std::to_string(q_factor);
        return MPP_ERR_VALUE;
    }
    if (ctx_) cleanup();

    width_ = width;
    height_ = height;
    hor_stride_ = align(width, 16);
    ver_stride_ = align(height, 16);
    q_factor_ = q_factor;
    in_buf_size_ = static_cast<size_t>(hor_stride_) * ver_stride_ * 3 / 2;

    // ---- 1. 创建并初始化为 MJPEG 编码器 ----
    // 注: mpp_init(MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG) 本身会成功, 但**不代表
    //     编码器可用** —— 下面若漏设 fps, encode_put_frame 会 segfault (坑2)。
    MPP_RET ret = mpp_create(&ctx_, &mpi_);
    if (ret) {
        last_error_ = "mpp_create ret=" + std::to_string(ret);
        cleanup();
        return ret;
    }
    ret = mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
    if (ret) {
        last_error_ = "mpp_init(ENC,MJPEG) ret=" + std::to_string(ret);
        cleanup();
        return ret;
    }

    // ---- 2. 配置 ----
    // 只能用 mpp_enc_cfg_init + GET_CFG + set_s32 + SET_CFG 这套新 API。
    // MPP_ENC_SET_RC_CFG / SET_CODEC_CFG / SET_PREP_CFG 在本库已废弃, 恒返回 -1。
    ret = mpp_enc_cfg_init(&cfg_);
    if (ret) {
        last_error_ = "mpp_enc_cfg_init ret=" + std::to_string(ret);
        cleanup();
        return ret;
    }
    ret = mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_);
    if (ret) {
        last_error_ = "MPP_ENC_GET_CFG ret=" + std::to_string(ret);
        cleanup();
        return ret;
    }

    auto set = [&](const char* name, int val) -> int {
        MPP_RET r = mpp_enc_cfg_set_s32(cfg_, name, val);
        if (r) {
            last_error_ = std::string("mpp_enc_cfg_set_s32(") + name +
                          ") ret=" + std::to_string(r);
            cleanup();
        }
        return r;
    };

    // 输入描述
    if (set("prep:width", width_)) return MPP_ERR_VALUE;
    if (set("prep:height", height_)) return MPP_ERR_VALUE;
    if (set("prep:hor_stride", hor_stride_)) return MPP_ERR_VALUE;
    if (set("prep:ver_stride", ver_stride_)) return MPP_ERR_VALUE;
    if (set("prep:format", MPP_FMT_YUV420SP)) return MPP_ERR_VALUE;  // 只接受 NV12

    // 码率控制: MJPEG 用 FIXQP, **fps 必填** (坑2: 不填会 segfault)
    if (set("rc:mode", MPP_ENC_RC_MODE_FIXQP)) return MPP_ERR_VALUE;
    if (set("rc:quality", MPP_ENC_RC_QUALITY_BEST)) return MPP_ERR_VALUE;
    if (set("rc:fps_in_num", 30)) return MPP_ERR_VALUE;
    if (set("rc:fps_in_denorm", 1)) return MPP_ERR_VALUE;  // 库内断言叫 denom, 键名两种都收
    if (set("rc:fps_out_num", 30)) return MPP_ERR_VALUE;
    if (set("rc:fps_out_denorm", 1)) return MPP_ERR_VALUE;
    if (set("rc:gop", 1)) return MPP_ERR_VALUE;  // MJPEG 全 I 帧

    // JPEG 量化
    if (set("jpeg:q_factor", q_factor_)) return MPP_ERR_VALUE;

    ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_);
    if (ret) {
        last_error_ = "MPP_ENC_SET_CFG ret=" + std::to_string(ret);
        cleanup();
        return ret;
    }

    // ---- 3. 预分配 DRM 输入 buffer (零拷贝: 下游可直接写) ----
    ret = mpp_buffer_group_get_internal(&in_grp_, MPP_BUFFER_TYPE_DRM);
    if (ret) {
        last_error_ = "mpp_buffer_group_get_internal ret=" + std::to_string(ret);
        cleanup();
        return ret;
    }
    ret = mpp_buffer_get(in_grp_, &in_buf_, in_buf_size_);
    if (ret) {
        last_error_ = "mpp_buffer_get(" + std::to_string(in_buf_size_) +
                      ") ret=" + std::to_string(ret);
        cleanup();
        return ret;
    }

    // ---- 4. 预分配输入 MppFrame (复用, 不每帧重建) ----
    ret = mpp_frame_init(&frame_);
    if (ret) {
        last_error_ = "mpp_frame_init ret=" + std::to_string(ret);
        cleanup();
        return ret;
    }
    mpp_frame_set_width(frame_, width_);
    mpp_frame_set_height(frame_, height_);
    mpp_frame_set_hor_stride(frame_, hor_stride_);
    mpp_frame_set_ver_stride(frame_, ver_stride_);
    mpp_frame_set_fmt(frame_, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame_, in_buf_);
    mpp_frame_set_eos(frame_, 0);
    // 注意: prep:range / mpp_frame_set_color_range 两个参数被编码器完全忽略,
    //       设了也没用 (实测 12 组组合输出逐字节相同), 故此处不设。

    return MPP_OK;
}

uint8_t* MppJpegEncoder::input_ptr() {
    if (!in_buf_) return nullptr;
    return static_cast<uint8_t*>(mpp_buffer_get_ptr(in_buf_));
}

int MppJpegEncoder::get_input_dma_fd() const {
    if (!in_buf_) return -1;
    return mpp_buffer_get_fd(in_buf_);
}

int MppJpegEncoder::encode(const uint8_t* nv12, size_t nv12_size,
                           std::vector<uint8_t>* out) {
    if (!out) {
        last_error_ = "encode: out is null";
        return MPP_ERR_NULL_PTR;
    }
    out->clear();
    size_t need = 0;
    // 先用一个足够大的临时区, 再 move 进 vector。JPEG 最大不超过原始 3/2 * 4
    // (qf=1 极端情况), 但正常 qf>=30 时远小于此; 这里给 2 倍 YUV 大小留余量。
    std::vector<uint8_t> tmp(in_buf_size_ * 2 + 4096);
    int r = encode(nv12, nv12_size, tmp.data(), tmp.size(), &need);
    if (r == MPP_ERR_NOMEM) {
        // 实在不够就按 need 扩一次
        tmp.resize(need);
        r = encode(nv12, nv12_size, tmp.data(), tmp.size(), &need);
    }
    if (r) return r;
    out->assign(tmp.begin(), tmp.begin() + need);
    return MPP_OK;
}

int MppJpegEncoder::encode(const uint8_t* nv12, size_t nv12_size,
                           uint8_t* out, size_t out_cap, size_t* out_size) {
    if (!ctx_) {
        last_error_ = "encode: not initialized";
        return MPP_ERR_INIT;
    }
    if (!nv12 || !out || !out_size) {
        last_error_ = "encode: null pointer argument";
        return MPP_ERR_NULL_PTR;
    }
    if (nv12_size < in_buf_size_) {
        last_error_ = "encode: nv12_size " + std::to_string(nv12_size) +
                      " < required " + std::to_string(in_buf_size_);
        return MPP_ERR_VALUE;
    }
    *out_size = 0;

    int64_t t0 = now_us();

    // ---- 1. 拷入 DRM buffer ----
    // 注: 这次 memcpy 实测 0.07ms (640x640), 不是瓶颈; 若上游是 RGA/VPU
    //     可直接写 input_ptr(), 或用 get_input_dma_fd() 做真零拷贝。
    std::memcpy(mpp_buffer_get_ptr(in_buf_), nv12, in_buf_size_);

    // ---- 2. 提交 ----
    MPP_RET ret = mpi_->encode_put_frame(ctx_, frame_);
    if (ret) {
        last_error_ = "encode_put_frame ret=" + std::to_string(ret);
        return ret;
    }

    // ---- 3. 轮询取包 ----
    // 实测 640x640 平均 3.1ms 全部耗在这一步 (put 内部立即返回, 硬件异步)。
    // 注意: 这是**阻塞等待硬件**, 不是 CPU 在算 —— 期间 CPU 占用仅 6.2%。
    // 流水线化 (put 多帧再 get 多包) 实测无改善 (3.38 -> 3.14ms), 因为单个
    // context 只能占 1 个 jpege 核, 硬件本身就没有并行余量。要提吞吐必须
    // 用多 context (见 MppJpegEncoderPool)。
    MppPacket pkt = nullptr;
    for (int i = 0; i < 5000; ++i) {
        ret = mpi_->encode_get_packet(ctx_, &pkt);
        if (ret) {
            last_error_ = "encode_get_packet ret=" + std::to_string(ret);
            return ret;
        }
        if (pkt) break;
    }
    if (!pkt) {
        last_error_ = "encode_get_packet: no packet after 5000 polls";
        return MPP_ERR_TIMEOUT;
    }

    uint8_t* pdata = static_cast<uint8_t*>(mpp_packet_get_pos(pkt));
    size_t plen = mpp_packet_get_length(pkt);

    last_encode_ms_ = (now_us() - t0) / 1000.0;

    if (plen > out_cap) {
        *out_size = plen;
        mpp_packet_deinit(&pkt);
        last_error_ = "encode: out buffer too small, need " + std::to_string(plen);
        return MPP_ERR_NOMEM;
    }
    std::memcpy(out, pdata, plen);
    *out_size = plen;
    mpp_packet_deinit(&pkt);

    return MPP_OK;
}

int MppJpegEncoder::set_q_factor(int q_factor) {
    if (!ctx_ || !cfg_) {
        last_error_ = "set_q_factor: not initialized";
        return MPP_ERR_INIT;
    }
    if (q_factor < 1 || q_factor > 99) {
        last_error_ = "set_q_factor: out of range [1, 99]";
        return MPP_ERR_VALUE;
    }
    MPP_RET ret = mpp_enc_cfg_set_s32(cfg_, "jpeg:q_factor", q_factor);
    if (ret) {
        last_error_ = "set_q_factor: set_s32 ret=" + std::to_string(ret);
        return ret;
    }
    ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_);
    if (ret) {
        last_error_ = "set_q_factor: MPP_ENC_SET_CFG ret=" + std::to_string(ret);
        return ret;
    }
    q_factor_ = q_factor;
    return MPP_OK;
}

// ===========================================================================
//  MppJpegEncoderPool
// ===========================================================================

MppJpegEncoderPool::MppJpegEncoderPool() = default;

MppJpegEncoderPool::~MppJpegEncoderPool() {
    encs_.clear();  // 每个 MppJpegEncoder 析构时自己 cleanup
}

int MppJpegEncoderPool::init(int width, int height, int channels, int q_factor) {
    if (channels < 1) {
        return MPP_ERR_VALUE;
    }
    encs_.clear();
    width_ = width;
    height_ = height;
    for (int i = 0; i < channels; ++i) {
        auto e = std::make_unique<MppJpegEncoder>();
        int r = e->init(width, height, q_factor);
        if (r) {
            encs_.clear();
            return r;
        }
        encs_.push_back(std::move(e));
    }
    return MPP_OK;
}

int MppJpegEncoderPool::encode_batch(const std::vector<const uint8_t*>& nv12s,
                                     size_t nv12_size,
                                     std::vector<std::vector<uint8_t>>* outs) {
    if (!is_ready() || !outs) return MPP_ERR_INIT;
    const int n = (int)encs_.size();
    if ((int)nv12s.size() != n || (int)outs->size() != n) return MPP_ERR_VALUE;

    // 每路一个线程跑自己的独立 MppCtx —— 这是吃满多个 jpege 核的唯一方式。
    // (流水线 put-K/get-K 在同一 context 内实测无效, 因为硬件无并行余量。)
    std::vector<int> rets(n, 0);
    std::vector<std::thread> ths;
    ths.reserve(n);
    for (int i = 0; i < n; ++i) {
        ths.emplace_back([&, i]() {
            rets[i] = encs_[i]->encode(nv12s[i], nv12_size, &(*outs)[i]);
        });
    }
    for (auto& t : ths) t.join();

    for (int i = 0; i < n; ++i) {
        if (rets[i]) return rets[i];
    }
    return MPP_OK;
}

std::string MppJpegEncoderPool::last_error() const {
    for (const auto& e : encs_) {
        if (!e->last_error().empty()) return e->last_error();
    }
    return {};
}

// ===========================================================================
//  BGR <-> NV12 (full-range, JPEG 口径)
// ===========================================================================

int bgr_to_nv12_full_range(const uint8_t* bgr, int width, int height, int bgr_stride,
                           uint8_t* nv12, int hor_stride, int ver_stride) {
    if (!bgr || !nv12 || width <= 0 || height <= 0) return 0;
    if (bgr_stride <= 0) bgr_stride = width * 3;
    if (hor_stride <= 0) hor_stride = align(width, 16);
    if (ver_stride <= 0) ver_stride = align(height, 16);

    // 两条 LUT: limited->full 展开。cv::cvtColor(BGR2YUV_I420) 给出的就是
    // limited-range (Y 16~235, C 16~240), 而 MPP 原样写进 JPEG 要求 full-range。
    // 直接喂会偏暗, Y-PSNR 只有 ~31dB / BGR-PSNR ~35dB (实测)。
    static uint8_t lut_y[256], lut_c[256];
    static bool inited = false;
    if (!inited) {
        for (int i = 0; i < 256; ++i) {
            lut_y[i] = static_cast<uint8_t>(clamp_u8(
                static_cast<int>((i - 16) * 255.0 / 219.0 + 0.5)));
            lut_c[i] = static_cast<uint8_t>(clamp_u8(
                static_cast<int>((i - 128) * 255.0 / 224.0 + 128.0 + 0.5)));
        }
        inited = true;
    }

    // Y 平面
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = bgr + static_cast<size_t>(y) * bgr_stride;
        uint8_t* dst = nv12 + static_cast<size_t>(y) * hor_stride;
        for (int x = 0; x < width; ++x) {
            int B = src[x * 3 + 0];
            int G = src[x * 3 + 1];
            int R = src[x * 3 + 2];
            // BT.601 limited Y, 然后展开到 full
            int y_lim = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
            dst[x] = lut_y[clamp_u8(y_lim)];
        }
    }

    // UV 平面 (NV12: U V U V 交错, 每 2x2 取一个)
    uint8_t* uv = nv12 + static_cast<size_t>(hor_stride) * ver_stride;
    for (int y = 0; y < height / 2; ++y) {
        const uint8_t* s0 = bgr + static_cast<size_t>(y * 2) * bgr_stride;
        const uint8_t* s1 = bgr + static_cast<size_t>(y * 2 + 1) * bgr_stride;
        uint8_t* dst = uv + static_cast<size_t>(y) * hor_stride;
        for (int x = 0; x < width / 2; ++x) {
            // 2x2 四点平均
            int R = 0, G = 0, B = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const uint8_t* s = dy ? s1 : s0;
                B += s[(x * 2) * 3 + 0] + s[(x * 2 + 1) * 3 + 0];
                G += s[(x * 2) * 3 + 1] + s[(x * 2 + 1) * 3 + 1];
                R += s[(x * 2) * 3 + 2] + s[(x * 2 + 1) * 3 + 2];
            }
            B >>= 2; G >>= 2; R >>= 2;
            int u_lim = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
            int v_lim = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
            dst[x * 2 + 0] = lut_c[clamp_u8(u_lim)];
            dst[x * 2 + 1] = lut_c[clamp_u8(v_lim)];
        }
    }

    // 补齐 stride 边缘 (仅当 width/height 不是 16 对齐时存在)
    for (int y = height; y < ver_stride; ++y) {
        std::memset(nv12 + static_cast<size_t>(y) * hor_stride, 0,
                    static_cast<size_t>(hor_stride) * 3 / 2);
    }
    for (int y = 0; y < height; ++y) {
        std::memset(nv12 + static_cast<size_t>(y) * hor_stride + width, 16,
                    hor_stride - width);
    }

    return hor_stride;
}

void nv12_full_to_bgr(const uint8_t* nv12, int width, int height, int hor_stride,
                      uint8_t* bgr, int bgr_stride) {
    if (!nv12 || !bgr || width <= 0 || height <= 0) return;
    if (bgr_stride <= 0) bgr_stride = width * 3;
    const uint8_t* uv = nv12 + static_cast<size_t>(hor_stride) * height;

    for (int y = 0; y < height; ++y) {
        const uint8_t* yrow = nv12 + static_cast<size_t>(y) * hor_stride;
        const uint8_t* uvrow = uv + static_cast<size_t>(y / 2) * hor_stride;
        uint8_t* dst = bgr + static_cast<size_t>(y) * bgr_stride;
        for (int x = 0; x < width; ++x) {
            int Y = yrow[x];
            int U = uvrow[(x / 2) * 2 + 0] - 128;
            int V = uvrow[(x / 2) * 2 + 1] - 128;
            // BT.601 full-range YCbCr -> RGB
            int R = Y + ((918 * V) >> 10);
            int G = Y - ((225 * U + 468 * V) >> 10);
            int B = Y + ((1164 * U) >> 10);
            dst[x * 3 + 0] = static_cast<uint8_t>(clamp_u8(B));
            dst[x * 3 + 1] = static_cast<uint8_t>(clamp_u8(G));
            dst[x * 3 + 2] = static_cast<uint8_t>(clamp_u8(R));
        }
    }
}

}  // namespace edgevision
