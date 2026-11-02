#!/bin/bash
echo "=== mpp_buffer_group_get_internal signature ==="
grep -n -A4 "mpp_buffer_group_get_internal" /root/edgevision/third_party/mpp/include/mpp_buffer.h | head -20
echo "=== buffer group set flags ==="
grep -n "set_buffer_flags\|group_set" /root/edgevision/third_party/mpp/include/mpp_buffer.h | head
echo "=== RGA im2d key funcs ==="
grep -n "imcvtcolor\|imresize\|improcess\|IM_STATUS_SUCCESS" /usr/include/rga/im2d.hpp | head -20
echo "=== im2d_type.h import buf ==="
grep -n "importbuffer_fd\|wrapbuffer_handle\|rga_buffer_t\|IM_HAL_TRANSFORM" /usr/include/rga/im2d_type.h | head -20
