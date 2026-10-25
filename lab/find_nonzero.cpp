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
    size_t count = out.size / sizeof(float);

    size_t non_zero_count = 0;
    float min_nz = 1e9, max_nz = -1e9;
    for (size_t i = 0; i < count; ++i) {
        if (data[i] != 0.0f) {
            non_zero_count++;
            min_nz = std::min(min_nz, data[i]);
            max_nz = std::max(max_nz, data[i]);
            if (non_zero_count <= 10) {
                std::cout << "Non-zero at index " << i << " = " << data[i] << std::endl;
            }
        }
    }
    std::cout << "Total elements: " << count << ", Non-zero count: " << non_zero_count 
              << ", Range: [" << min_nz << ", " << max_nz << "]" << std::endl;

    rknn_outputs_release(ctx, 1, &out);
    rknn_destroy(ctx);
    return 0;
}
