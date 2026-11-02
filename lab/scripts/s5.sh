#!/bin/bash
echo "=== wrapbuffer_handle in im2d_type hpp ==="
grep -rn "wrapbuffer_handle\|importbuffer_fd" /usr/include/rga/im2d.hpp | head -10
echo "=== rga_buffer_t def ==="
grep -n -A15 "typedef struct.*rga_buffer_t\|} rga_buffer_t" /usr/include/rga/im2d_type.h | head -30
echo "=== RK_FORMAT enums ==="
grep -n "RK_FORMAT_YCbCr_420_SP\|RK_FORMAT_RGB_888\|RK_FORMAT_RGBA_8888" /usr/include/rga/im2d_type.h | head
