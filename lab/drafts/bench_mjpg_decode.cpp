// 最小验证：同一帧 MJPG，OpenCV 软解 vs MPP 硬解 的耗时对比
// 编译：g++ -O2 bench_mjpg_decode.cpp -o bench_mjpg_decode \
//         `pkg-config --cflags --libs opencv4` -I/usr/include/rockchip \
//         -lrockchip_mpp -lpthread
// 运行：./bench_mjpg_decode /root/lab/sample.mjpg 100
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
#include "mpp_log.h"

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/root/lab/sample.mjpg";
    const int   N    = (argc > 2) ? atoi(argv[2]) : 100;

    FILE* fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<char> buf(sz);
    if (fread(buf.data(), 1, sz, fp) != (size_t)sz) { printf("读取不完整\n"); return 1; }
    fclose(fp);
    printf("样本: %s (%ld 字节)\n\n", path, sz);

    // ---------------- OpenCV 软解 ----------------
    {
        auto t = Clock::now();
        cv::Mat img;
        for (int i = 0; i < N; ++i) {
            cv::Mat raw(1, (int)sz, CV_8UC1, buf.data());
            img = cv::imdecode(raw, cv::IMREAD_COLOR);
        }
        double total = ms_since(t);
        printf("[OpenCV 软解] %3d 次, 平均 %6.2f ms/帧, 输出 %dx%d BGR\n",
               N, total / N, img.cols, img.rows);
    }

    // ---------------- MPP 硬解 ----------------
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK) { printf("[-] mpp_create 失败\n"); return 1; }
    if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
        printf("[-] mpp_init 失败\n"); return 1;
    }
    MppFrameFormat fmt = MPP_FMT_YUV420SP;   // NV12
    mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, &fmt);

    // 内部 buffer group：多数 MPP 版本不显式给会拿不到帧
    MppBufferGroup grp = nullptr;
    if (mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION) == MPP_OK) {
        mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
    } else {
        printf("[!] buffer group 申请失败，继续尝试\n");
    }

    // 整帧输入，不要流式分割（我们每次喂一整张 JPEG）
    RK_U32 need_split = 0;
    mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &need_split);

    int ok = 0;
    double total = 0;
    int w = 0, h = 0, hs = 0, vs = 0;
    for (int i = 0; i < N; ++i) {
        auto t = Clock::now();

        MppPacket pkt = nullptr;
        if (mpp_packet_init(&pkt, buf.data(), sz) != MPP_OK) break;
        mpp_packet_set_pos(pkt, buf.data());
        mpp_packet_set_length(pkt, (size_t)sz);   // 显式设长度：init 只设 pos，length 可能为 0
        mpp_packet_set_eos(pkt);                  // 单帧完整 JPEG
        MPP_RET ret = mpi->decode_put_packet(ctx, pkt);
        mpp_packet_deinit(&pkt);
        if (ret != MPP_OK) { printf("[-] put_packet 失败 i=%d ret=%d\n", i, (int)ret); break; }

        MppFrame frame = nullptr;
        for (int k = 0; k < 5000 && frame == nullptr; ++k) {
            mpi->decode_get_frame(ctx, &frame);
            if (frame == nullptr) usleep(200);
        }
        if (frame == nullptr) { printf("[-] get_frame 超时 i=%d\n", i); break; }

        total += ms_since(t);
        ok++;
        if (i == 0) {
            w  = (int)mpp_frame_get_width(frame);
            h  = (int)mpp_frame_get_height(frame);
            hs = (int)mpp_frame_get_hor_stride(frame);
            vs = (int)mpp_frame_get_ver_stride(frame);
        }
        mpp_frame_deinit(&frame);
    }
    if (ok > 0) {
        printf("[MPP  硬解] %3d 次, 平均 %6.2f ms/帧, 输出 %dx%d (stride %dx%d, NV12)\n",
               ok, total / ok, w, h, hs, vs);
    }
    mpp_destroy(ctx);
    return 0;
}
