#include <iostream>
#include <vector>
#include <rknn_api.h>

int main() {
    FILE* fp = fopen("/root/edgevision/data/models/yolov8n_rk3588_i8.rknn", "rb");
    fseek(fp, 0, SEEK_END); size_t s = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::vector<char> b(s); fread(b.data(), 1, s, fp); fclose(fp);

    rknn_context ctx = 0;
    rknn_init(&ctx, b.data(), s, 0, nullptr);

    // 虚拟 640x640x3 图像
    std::vector<uint8_t> img(640 * 640 * 3, 128);
    rknn_input in{};
    in.index = 0;
    in.buf = img.data();
    in.size = img.size();
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    rknn_inputs_set(ctx, 1, &in);

    int r_run = rknn_run(ctx, nullptr);
    std::cout << "rknn_run ret = " << r_run << std::endl;

    rknn_output out{};
    out.want_float = 1;
    out.index = 0;
    int r_get = rknn_outputs_get(ctx, 1, &out, nullptr);
    std::cout << "rknn_outputs_get ret = " << r_get << ", size = " << out.size << std::endl;

    float* f = (float*)out.buf;
    std::cout << "First 10 floats: ";
    for (int i = 0; i < 10; ++i) std::cout << f[i] << " ";
    std::cout << std::endl;

    rknn_outputs_release(ctx, 1, &out);
    rknn_destroy(ctx);
    return 0;
}
