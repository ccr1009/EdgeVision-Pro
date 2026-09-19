#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <iostream>

namespace edgevision {

enum RKNNCoreMaskCpp {
    RKNN_CORE_AUTO = 0,
    RKNN_CORE_0    = 1,
    RKNN_CORE_1    = 2,
    RKNN_CORE_2    = 4,
    RKNN_CORE_ALL  = 7
};

struct DMABufDescriptor {
    int fd;
    size_t size_bytes;
    int width;
    int height;
    std::string format;
    bool is_busy;
};

class ZeroCopyPipelineEngine {
public:
    ZeroCopyPipelineEngine(int pool_size = 6) : pool_size_(pool_size) {
        // Pre-allocate buffer pool
        for (int i = 0; i < pool_size_; ++i) {
            mpp_pool_.push_back({1000 + i, 1920 * 1080 * 3 / 2, 1920, 1080, "NV12", false});
            rga_pool_.push_back({2000 + i, 640 * 640 * 3, 640, 640, "RGB888", false});
        }
    }

    DMABufDescriptor* acquire_mpp_buffer() {
        for (auto& buf : mpp_pool_) {
            if (!buf.is_busy) {
                buf.is_busy = true;
                return &buf;
            }
        }
        return nullptr;
    }

    DMABufDescriptor* rga_hardware_transform(DMABufDescriptor* src_buf) {
        if (!src_buf) return nullptr;
        for (auto& dst_buf : rga_pool_) {
            if (!dst_buf.is_busy) {
                dst_buf.is_busy = true;
                // Recycle source MPP buffer back to pool
                src_buf->is_busy = false;
                bytes_saved_total_ += (src_buf->size_bytes + dst_buf.size_bytes);
                zero_copy_count_++;
                return &dst_buf;
            }
        }
        return nullptr;
    }

    void release_rga_buffer(DMABufDescriptor* buf) {
        if (buf) buf->is_busy = false;
    }

    size_t get_bytes_saved() const { return bytes_saved_total_; }
    int get_zero_copy_count() const { return zero_copy_count_; }

private:
    int pool_size_;
    std::vector<DMABufDescriptor> mpp_pool_;
    std::vector<DMABufDescriptor> rga_pool_;
    size_t bytes_saved_total_ = 0;
    int zero_copy_count_ = 0;
};

} // namespace edgevision
