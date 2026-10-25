// ============================================================================
//  mpp_jpeg_encoder.h —— RK3588 MPP 硬件 MJPEG/JPEG 编码器封装
//
//  与 mpp_jpeg_decoder.h 同风格; 但**先说结论: 本板硬编 JPEG 不划算**。
//  本文件保留完整实现 + 实测数据, 供"为什么这条路走不通"复用与面试讲解。
//
//  ── 一句话结论 (EVM3588-B, Debian11, 内核 5.10.110, MPP 源码编译版) ──
//    640x640 单帧: 硬编 3.16ms  vs  软编 cv::imencode(q80) 3.06ms   → 硬编更慢 3%
//    质量:        硬编 Y-PSNR 44.1dB / BGR-PSNR 46.2dB  vs 软编 44.6 / 47.0dB
//    唯一优势:    CPU 占用 6.2%  vs  99.7%  (每帧 CPU 时间 0.197ms vs 3.05ms)
//    4 路并行:    硬编 4 个 context 吃满 4 个 jpege 核, 1.06ms/帧 (941 FPS), CPU 18.5%
//                 软编 4 线程 0.78ms/帧 (1279 FPS), CPU 392.6%
//    => 纯延迟和纯吞吐都不占优, 只有 CPU 卸载有意义。本项目单帧流水线场景不用硬编。
//
//  ── 实测数据 (N=200, p50/p95/p99 单位 ms) ──
//    分辨率     硬编 mean   p50    p95    p99    软编 mean  硬/软
//    640x640      3.16     3.14   3.22   3.81     3.06      0.97x
//   1280x720      6.83     6.69   7.04  10.82     6.72      0.98x
//
//  ── 硬件路径铁证 (/proc/interrupts) ──
//    RK3588 有 4 个 JPEG 编码核: fdba0000/fdba4000/fdba8000/fdbac000 .jpege-core
//    单 context 只用第一个核: 500 帧后 fdba0000 中断 +500 (每帧 1 次), 其余 3 核 0 变化
//    => 硬编确实在 A 硬件上做, 不是软件回退。慢是"硬件本身慢", 不是"没走硬件"。
//
//  ── 为什么会慢: 实测延迟模型 ──
//    t(ms) ≈ 0.35 (固定往返开销) + 7.0e-6 * 像素数
//    校验: 64x64→0.41  256x256→0.77  640x640→3.25  960x540→4.18
//    软编 libjpeg-turbo 单核: 7.5e-6 * 像素数, 无固定开销
//    => jpege 单核的每像素吞吐与 A76 单核跑 libjpeg-turbo **几乎相同**,
//       而硬编还要多付 0.35ms 的 ioctl+中断往返延迟。
//       JPEG 编码是 DCT+Huffman, libjpeg-turbo 的 NEON SIMD 已经极度优化;
//       Rockchip jpege 的定位是"相机抓拍时不要占 CPU", 不是"比 CPU 算得快"。
//
//  ── 踩过的坑 (按顺序, 每个都卡过一次) ──
//    坑1  系统自带 /usr/lib/librockchip_mpp.so (v1.5.0, 2021-05) 与内核驱动版本
//         倒挂 → 必现 MPP_ERR_BUFFER_FULL(-1012)。必须用 third_party/mpp 源码编译版。
//         (与硬解同坑, 见 TROUBLESHOOTING 第 17 条)
//    坑2  **不设 fps 会段错误**: mpp_init(MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG) 成功、
//         MPP_ENC_SET_CFG 也返回 0, 但 encode_put_frame() 直接 Segfault。
//         崩溃前日志: "rc: Assertion cfg->fps_in_denom >= 1 failed at
//         rc_frm_check_drop:142" → fps 分母为 0, rc 除零后 rc_base 空指针。
//         修法: 必须设 rc:fps_in_num/rc:fps_in_denorm/rc:fps_out_num/rc:fps_out_denorm。
//         注意头文件字段名是 fps_in_denorm(denorm), 但**配置键名两种拼写都可用**。
//    坑3  MPP_ENC_SET_RC_CFG / SET_CODEC_CFG / SET_PREP_CFG 全部已废弃, 返回 -1,
//         日志 "deprecated ... use MPP_ENC_GET_CFG / MPP_ENC_SET_CFG instead"。
//         只能走 mpp_enc_cfg_init + GET_CFG + set_s32(短键名) + SET_CFG。
//         且键名清单必须实测: 网上抄的会漏, strings 挖的也不全。
//         可用: prep:width/height/hor_stride/ver_stride/format/range
//               rc:mode/quality/bps_target/fps_in_num/fps_in_denorm/fps_in_denom/
//               fps_out_num/fps_out_denorm/fps_out_flex/gop/qp_init
//               jpeg:q_mode/q_factor/quant/qf_min/qf_max/qtable_y
//         不可用: prep:color / prep:denoise / prep:sharpen / jpeg:qfactor
//    坑4  **输入 YUV 必须是 full-range**, 否则画面偏暗、PSNR 只有 35dB。
//         cv::cvtColor(BGR2YUV_I420) 输出的是 limited-range (Y 16~235),
//         而 MPP 把输入 YUV **原样**写进 JPEG, 不做任何 range 变换
//         (实测 Y_JPEG = 1.0012 * Y_in - 0.15, a≈1.0 证明没做变换);
//         JPEG 规范要求 full-range → 必须输入侧预补偿 (Y-16)*255/219。
//         补偿后 BGR-PSNR 从 35.4dB → 46.2dB。
//         另: prep:range 和 mpp_frame_set_color_range 两个参数被编码器**完全忽略**
//         (12 组组合输出逐字节相同), 别指望它们。
//    坑5  prep:format 只接受 YUV420SP(NV12)。传 RGB888/BGR888 让 MPP 自己做 CSC
//         会输出**不可解码的垃圾**(imdecode 返回空), 此路不通。
//    坑6  jpeg:q_factor 的 1~99 与 cv::imencode 的 quality 80 **不是同一口径**,
//         但换算近似 1:1 —— qf=80 时码率最接近软编 q80 (27650B vs 26888B)。
//         奇怪的是 qf 从 30 到 95, 单帧耗时几乎不变 (3.46→3.08ms, 甚至 qf 越高越快),
//         说明瓶颈不在 Huffman 熵编码, 而在固定管线。
//
//  ── 正确性 (640x640 实拍图 real_000.jpg, 硬编后用 cv::imdecode 解回) ──
//    file(1) 认作 "JPEG image data, JFIF standard 1.02, baseline, precision 8,
//                    640x640, components 3"
//    Y-PSNR 44.09 dB, BGR-PSNR 46.19 dB  (软编 q80 对照: 44.61 / 47.02 dB)
//    => 质量达标, 硬编本身没有问题。问题纯粹是速度。
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "rk_mpi.h"

namespace edgevision {

/// 默认 JPEG 量化因子。与 cv::imencode 的 quality=80 码率基本对齐
/// (实测 27650 B vs 26888 B, 差 2.8%)。
constexpr int kDefaultJpegQF = 80;

/// 单帧编码结果视图。data 指向 MppJpegEncoder 内部 DRM buffer,
/// 生命周期到下一次 encode() 之前; 需要长期持有请自行 copy。
struct EncodedJpeg {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
    double encode_ms = 0.0;   ///< 本次 encode_put_frame + get_packet 的 wall time
};

/// MPP 硬件 JPEG 编码器 (单 context)。
///
/// 线程安全: 否。一个实例只能被一个线程使用。需要多路并行请用
/// MppJpegEncoderPool (每个线程一个独立 context, 才能吃满 4 个 jpege 核)。
class MppJpegEncoder {
public:
    MppJpegEncoder();
    ~MppJpegEncoder();

    MppJpegEncoder(const MppJpegEncoder&) = delete;
    MppJpegEncoder& operator=(const MppJpegEncoder&) = delete;

    /// 初始化编码上下文。内部完成 create/init/cfg/buffer/frame 全部准备工作。
    /// 已自动设置 fps=30/1 与 gop=1 (不设 fps 会 segfault, 见文件头坑2)。
    /// @param width    图像宽 (内部按 16 对齐算 hor_stride)
    /// @param height   图像高 (内部按 16 对齐算 ver_stride)
    /// @param q_factor JPEG 量化因子 1~99, 越大越清晰/文件越大; 80 约等于 cv q80
    /// @return 0 成功, 负数见 mpp_err.h
    int init(int width, int height, int q_factor = kDefaultJpegQF);

    /// 编码一帧 NV12。
    /// @param nv12      NV12 数据, 必须是 **full-range** (见同名工具的
    ///                  bgr_to_nv12_full_range(); 直接用 cv::cvtColor 会偏暗)
    /// @param nv12_size 字节数, 至少 hor_stride * ver_stride * 3 / 2
    /// @param out      输出 JPEG 字节; 内部会 clear() 后 resize
    /// @return 0 成功; 负数失败 (见 last_error())
    int encode(const uint8_t* nv12, size_t nv12_size, std::vector<uint8_t>* out);

    /// 编码一帧 NV12, 输出到调用方缓冲区 (避免 vector 重新分配)。
    /// @param out_cap   缓冲区容量
    /// @param out_size  实际写出字节数
    /// @return 0 成功; <0 失败; 若返回 -1006 (NOMEM) 表示 out_cap 不够,
    ///         此时 *out_size 给出所需大小, 扩容重试即可
    int encode(const uint8_t* nv12, size_t nv12_size,
               uint8_t* out, size_t out_cap, size_t* out_size);

    /// 直接拿到内部 DRM 输入 buffer 的写指针, 用于"下游直接写这里"免一次 memcpy。
    /// 注意: 每次 encode() 返回后内容才保证已被硬件消费完。
    uint8_t* input_ptr();
    /// 内部 DRM 输入 buffer 的字节数 (hor_stride * ver_stride * 3 / 2)
    size_t input_size() const { return in_buf_size_; }
    /// 内部 DRM 输入 buffer 的 fd, 用于 RGA 零拷贝写入
    int get_input_dma_fd() const;

    int hor_stride() const { return hor_stride_; }
    int ver_stride() const { return ver_stride_; }
    int width() const { return width_; }
    int height() const { return height_; }

    /// 运行中修改量化因子 (会重发 MPP_ENC_SET_CFG, 开销 ~0.1ms)。
    int set_q_factor(int q_factor);
    int q_factor() const { return q_factor_; }

    /// 最近一次错误信息
    const std::string& last_error() const { return last_error_; }
    bool is_ready() const { return ctx_ != nullptr; }
    /// 最近一次 encode 的耗时 (ms), 方便调用方统计
    double last_encode_ms() const { return last_encode_ms_; }

private:
    void cleanup();

    MppCtx ctx_ = nullptr;
    MppApi* mpi_ = nullptr;
    MppEncCfg cfg_ = nullptr;
    MppFrame frame_ = nullptr;
    MppBuffer in_buf_ = nullptr;
    MppBufferGroup in_grp_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int hor_stride_ = 0;
    int ver_stride_ = 0;
    int q_factor_ = kDefaultJpegQF;
    size_t in_buf_size_ = 0;
    double last_encode_ms_ = 0.0;
    std::string last_error_;
};

/// 多 context 并行编码池 —— 唯一能让硬编"有用"的用法。
///
/// 为什么需要: 单 context 只占用 1 个 jpege 核 (实测 /proc/interrupts),
/// RK3588 有 4 个。开 N 个独立 MppCtx + N 个线程才能吃满 N 个核。
/// 实测 640x640: NT=1→3.27ms/帧, NT=2→2.03, NT=3→1.38, NT=4→1.08 (近线性)。
///
/// 但注意: 4 路软编 (cv::imencode 4 线程) 是 0.78ms/帧, 仍然更快;
/// 硬编池的价值只在"CPU 占用 18.5% vs 392.6%"这一条。
class MppJpegEncoderPool {
public:
    MppJpegEncoderPool();
    ~MppJpegEncoderPool();

    MppJpegEncoderPool(const MppJpegEncoderPool&) = delete;
    MppJpegEncoderPool& operator=(const MppJpegEncoderPool&) = delete;

    /// @param width,height  所有实例统一分辨率
    /// @param channels      并行路数 (= 想占用的 jpege 核数, 本板最多 4)
    /// @param q_factor      JPEG 量化因子
    int init(int width, int height, int channels, int q_factor = kDefaultJpegQF);

    /// 把 nv12s[i] 编码到 outs[i], 用 channels 个线程并行。
    /// nv12s/outs 长度都必须 == channels。nv12 需为 full-range。
    /// 用满 channels 条流后, wall / (channels 帧) 才是"吞吐口径每帧成本"。
    /// @return 0 全部成功; 否则返回第一个失败的 ret (其余线程仍会跑完)
    int encode_batch(const std::vector<const uint8_t*>& nv12s,
                     size_t nv12_size,
                     std::vector<std::vector<uint8_t>>* outs);

    int channels() const { return (int)encs_.size(); }
    int width() const { return width_; }
    int height() const { return height_; }
    bool is_ready() const { return !encs_.empty(); }
    /// 任意一个子实例的最近错误
    std::string last_error() const;

private:
    std::vector<std::unique_ptr<MppJpegEncoder>> encs_;
    int width_ = 0;
    int height_ = 0;
};

// ---------------------------------------------------------------------------
//  BGR -> NV12 转换 (full-range, JPEG 编码专用口径)
//
//  为什么不直接用 cv::cvtColor(BGR2YUV_I420): 它输出 limited-range (Y 16~235),
//  而 MPP 硬编按 JPEG 规范要求 full-range, 直接喂会偏暗、PSNR 掉到 35dB。
//  本函数做 BT.601 full-range:
//      Y = ((66R + 129G + 25B + 128) >> 8) + 16   然后展开 (Y-16)*255/219
//      U = ((-38R - 74G + 112B + 128) >> 8) + 128 然后展开 (U-128)*255/224+128
//      V = ((112R - 94G - 18B + 128) >> 8) + 128  同理
//  实测补偿后 BGR-PSNR 35.4 -> 46.2 dB (软编 47.0)。
//
//  @param bgr         BGR888 输入, 行跨度 bgr_stride 字节 (传 0 表示 = width*3)
//  @param nv12        输出, 大小至少 hor_stride*ver_stride*3/2
//  @param hor_stride  输出行跨度, 必须 16 对齐且 >= width; 传 0 表示自动 align16(width)
//  @param ver_stride  输出行数, 必须 16 对齐且 >= height; 传 0 表示自动 align16(height)
//  @return 实际使用的 hor_stride (需要时可用来算 buffer 大小)
// ---------------------------------------------------------------------------
int bgr_to_nv12_full_range(const uint8_t* bgr, int width, int height, int bgr_stride,
                           uint8_t* nv12, int hor_stride = 0, int ver_stride = 0);

/// NV12 -> BGR (用于验证/调试; 走 BT.601 full-range, 与 bgr_to_nv12_full_range 对称)
void nv12_full_to_bgr(const uint8_t* nv12, int width, int height, int hor_stride,
                      uint8_t* bgr, int bgr_stride = 0);

}  // namespace edgevision
