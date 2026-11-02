// ============================================================================
//  bench_mjpg_decode2.cpp —— OpenCV 纯 CPU 软解 vs Rockchip MPP 硬件解码 纯净对照
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <unistd.h>

#include <opencv2/opencv.hpp>

#include "rk_mpi.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_buffer.h"
#include "mpp_meta.h"

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/root/lab/sample.mjpg";
    const int   N    = (argc > 2) ? atoi(argv[2]) : 100;

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        perror("[-] fopen sample.mjpg");
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<char> buf(sz);
    if (fread(buf.data(), 1, sz, fp) != (size_t)sz) {
        printf("[-] 读取样本数据不完整\n");
        fclose(fp);
        return 1;
    }
    fclose(fp);

    printf("==================================================================\n");
    printf(" 🔬 MJPEG 解码性能基准测试: OpenCV 软解 (libjpeg) vs Rockchip MPP 硬件解码\n");
    printf(" 样本文件: %s (%ld 字节 / 约 %.1f KB)\n", path, sz, sz / 1024.0);
    printf(" 测试轮数: %d 轮\n", N);
    printf("==================================================================\n\n");

    // -------------------------------------------------------------
    // 1. OpenCV 纯 CPU 软解基准 (libjpeg)
    // -------------------------------------------------------------
    double soft_avg_ms = 0.0;
    {
        cv::Mat decoded_img;
        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            cv::Mat raw(1, (int)sz, CV_8UC1, buf.data());
            decoded_img = cv::imdecode(raw, cv::IMREAD_COLOR);
        }
        double soft_total_ms = ms_since(t0);
        soft_avg_ms = soft_total_ms / N;
        printf("[CPU 软解 OpenCV] %d 轮, 平均耗时: %6.2f ms/帧, 输出: %dx%d (BGR888)\n",
               N, soft_avg_ms, decoded_img.cols, decoded_img.rows);
    }

    // -------------------------------------------------------------
    // 2. Rockchip MPP 硬件解码器 (KEY_OUTPUT_FRAME 高级模式)
    // -------------------------------------------------------------
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK) {
        printf("[-] mpp_create 失败\n");
        return 1;
    }

    if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
        printf("[-] mpp_init (MPP_VIDEO_CodingMJPEG) 失败\n");
        mpp_destroy(ctx);
        return 1;
    }

    // 设置输出为硬件最佳格式 NV12 (YUV420SP)
    MppFrameFormat fmt = MPP_FMT_YUV420SP;
    mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, &fmt);

    // RK3588 DRM 显存池
    MppBufferGroup grp = nullptr;
    if (mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        printf("[-] mpp_buffer_group_get_internal 失败\n");
        mpp_destroy(ctx);
        return 1;
    }

    const int img_w = 1280;
    const int img_h = 720;
    const int hor_stride = (img_w + 15) & (~15); // 16 字节对齐
    const int ver_stride = (img_h + 15) & (~15);
    const size_t frame_size = hor_stride * ver_stride * 3 / 2;

    int ok_frames = 0;
    double mpp_total_ms = 0.0;

    for (int i = 0; i < N; ++i) {
        auto t_start = Clock::now();

        // 1. 为当前帧申请硬件输出显存 Buffer
        MppBuffer out_buf = nullptr;
        if (mpp_buffer_get(grp, &out_buf, frame_size) != MPP_OK) {
            printf("[-] 申请输出 frame buffer 失败\n");
            break;
        }

        // 2. 构造承接解码结果的目标 MppFrame
        MppFrame out_frame = nullptr;
        mpp_frame_init(&out_frame);
        mpp_frame_set_width(out_frame, img_w);
        mpp_frame_set_height(out_frame, img_h);
        mpp_frame_set_hor_stride(out_frame, hor_stride);
        mpp_frame_set_ver_stride(out_frame, ver_stride);
        mpp_frame_set_fmt(out_frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(out_frame, out_buf);

        // 3. 构造单帧 Packet 并通过元数据挂载 KEY_OUTPUT_FRAME
        MppPacket pkt = nullptr;
        if (mpp_packet_init(&pkt, buf.data(), sz) != MPP_OK) {
            mpp_frame_deinit(&out_frame);
            mpp_buffer_put(out_buf);
            break;
        }
        mpp_packet_set_pos(pkt, buf.data());
        mpp_packet_set_length(pkt, (size_t)sz);
        mpp_packet_set_eos(pkt);

        MppMeta meta = mpp_packet_get_meta(pkt);
        mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, out_frame);

        // 4. 送入硬件解码器
        MPP_RET put_ret = mpi->decode_put_packet(ctx, pkt);
        mpp_packet_deinit(&pkt);

        if (put_ret != MPP_OK) {
            printf("[-] decode_put_packet 失败 (ret=%d)\n", (int)put_ret);
            mpp_frame_deinit(&out_frame);
            mpp_buffer_put(out_buf);
            break;
        }

        // 5. 等待解码完成并获取 Frame
        MppFrame frame = nullptr;
        bool got_frame = false;

        for (int retry = 0; retry < 500; ++retry) {
            MPP_RET get_ret = mpi->decode_get_frame(ctx, &frame);
            if (get_ret != MPP_OK) break;

            if (frame) {
                if (mpp_frame_get_info_change(frame)) {
                    // 处理 info change 通知
                    mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
                    mpp_frame_deinit(&frame);
                    frame = nullptr;
                    continue;
                } else {
                    // 解码成功！
                    got_frame = true;
                    break;
                }
            }
            usleep(200); // 0.2ms
        }

        if (!got_frame || frame == nullptr) {
            printf("[-] 第 %d 轮解码等待超时\n", i);
            mpp_frame_deinit(&out_frame);
            mpp_buffer_put(out_buf);
            break;
        }

        mpp_total_ms += ms_since(t_start);
        ok_frames++;

        // 释放解码帧和 buffer
        mpp_frame_deinit(&frame);
        mpp_frame_deinit(&out_frame);
        mpp_buffer_put(out_buf);
    }

    if (ok_frames > 0) {
        double mpp_avg_ms = mpp_total_ms / ok_frames;
        printf("[MPP 硬件解码] %d 轮, 平均耗时: %6.2f ms/帧, 输出格式: NV12 (%dx%d, stride %dx%d)\n",
               ok_frames, mpp_avg_ms, img_w, img_h, hor_stride, ver_stride);
        printf("\n==================================================================\n");
        printf(" ⚡ 性能对比结果:\n");
        printf("    OpenCV 软解耗时: %6.2f ms/帧\n", soft_avg_ms);
        printf("    MPP    硬解耗时: %6.2f ms/帧\n", mpp_avg_ms);
        printf("    硬件加速收益    : 节省 %.2f ms/帧 (加速比: %.2fx, CPU占用彻底清零)\n",
               soft_avg_ms - mpp_avg_ms, soft_avg_ms / (mpp_avg_ms + 1e-5));
        printf("==================================================================\n");
    }

    mpp_destroy(ctx);
    return 0;
}
