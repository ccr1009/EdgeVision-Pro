#include <iostream>
#include <vector>
#include <rknn_api.h>

int main() {
    FILE* fp = fopen("/root/edgevision/data/models/yolov8n_rk3588_i8.rknn", "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    size_t s = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<char> b(s);
    fread(b.data(), 1, s, fp);
    fclose(fp);

    rknn_context ctx = 0;
    rknn_init(&ctx, b.data(), s, 0, nullptr);

    rknn_input_output_num io;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    std::cout << "Inputs: " << io.n_input << ", Outputs: " << io.n_output << std::endl;

    for (uint32_t i = 0; i < io.n_output; ++i) {
        rknn_tensor_attr attr{};
        attr.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        std::cout << "Output[" << i << "]: name=" << attr.name << " n_dims=" << attr.n_dims << " dims=[";
        for (uint32_t d = 0; d < attr.n_dims; ++d) {
            std::cout << attr.dims[d] << (d + 1 < attr.n_dims ? ", " : "");
        }
        std::cout << "] fmt=" << attr.fmt << " type=" << attr.type << std::endl;
    }

    rknn_destroy(ctx);
    return 0;
}
