#include <iostream>
#include <vector>
#include <cmath>
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

    std::cout << "Anchor 0: ";
    for (int k = 0; k < 10; ++k) std::cout << data[0 * 84 + k] << " ";
    std::cout << std::endl;

    std::cout << "Anchor 100: ";
    for (int k = 0; k < 10; ++k) std::cout << data[100 * 84 + k] << " ";
    std::cout << std::endl;

    // Check min and max across all class elements
    float min_cls = 1e9, max_cls = -1e9;
    for (int i = 0; i < 8400; ++i) {
        for (int c = 4; c < 84; ++c) {
            float v = data[i * 84 + c];
            min_cls = std::min(min_cls, v);
            max_cls = std::max(max_cls, v);
        }
    }
    std::cout << "Class channels min = " << min_cls << ", max = " << max_cls << std::endl;

    rknn_outputs_release(ctx, 1, &out);
    rknn_destroy(ctx);
    return 0;
}
