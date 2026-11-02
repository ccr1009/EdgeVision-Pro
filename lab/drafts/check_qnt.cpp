#include <iostream>
#include <vector>
#include <rknn_api.h>

int main() {
    FILE* fp = fopen("/root/edgevision/data/models/yolov8n_rk3588_i8.rknn", "rb");
    fseek(fp, 0, SEEK_END); size_t s = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::vector<char> b(s); fread(b.data(), 1, s, fp); fclose(fp);

    rknn_context ctx = 0;
    rknn_init(&ctx, b.data(), s, 0, nullptr);

    rknn_tensor_attr attr{};
    attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
    std::cout << "Output[0]: type=" << attr.type << ", qnt_type=" << attr.qnt_type 
              << ", scale=" << attr.scale << ", zp=" << attr.zp << std::endl;

    rknn_destroy(ctx);
    return 0;
}
