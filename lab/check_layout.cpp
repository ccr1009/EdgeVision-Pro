#include <iostream>
#include <vector>
#include <rknn_api.h>

int main() {
    FILE* fp = fopen("/root/edgevision/data/models/yolov8n_rk3588_i8.rknn", "rb");
    fseek(fp, 0, SEEK_END); size_t s = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::vector<char> b(s); fread(b.data(), 1, s, fp); fclose(fp);

    rknn_context ctx = 0;
    rknn_init(&ctx, b.data(), s, 0, nullptr);

    std::vector<uint8_t> img(640 * 640 * 3, 128);
    rknn_input in{};
    in.index = 0; in.buf = img.data(); in.size = img.size();
    in.type = RKNN_TENSOR_UINT8; in.fmt = RKNN_TENSOR_NHWC;
    rknn_inputs_set(ctx, 1, &in);
    rknn_run(ctx, nullptr);

    rknn_output out{}; out.want_float = 1;
    rknn_outputs_get(ctx, 1, &out, nullptr);
    float* data = (float*)out.buf;

    // 检查布局 A: data[c * 8400 + i]
    float max_score_A = 0;
    for (int c = 4; c < 84; ++c) {
        for (int i = 0; i < 8400; ++i) {
            max_score_A = std::max(max_score_A, data[c * 8400 + i]);
        }
    }

    // 检查布局 B: data[i * 84 + c]
    float max_score_B = 0;
    for (int i = 0; i < 8400; ++i) {
        for (int c = 4; c < 84; ++c) {
            max_score_B = std::max(max_score_B, data[i * 84 + c]);
        }
    }

    std::cout << "Max score under Layout A [c * 8400 + i]: " << max_score_A << std::endl;
    std::cout << "Max score under Layout B [i * 84 + c]: " << max_score_B << std::endl;

    rknn_outputs_release(ctx, 1, &out);
    rknn_destroy(ctx);
    return 0;
}
