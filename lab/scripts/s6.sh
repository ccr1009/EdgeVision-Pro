#!/bin/bash
sed -n '30,90p' /usr/include/rga/im2d.hpp
echo "=== rga_buffer_t struct ==="
sed -n '240,285p' /usr/include/rga/im2d_type.h
echo "=== format enum location ==="
grep -rn "RK_FORMAT_YCbCr_420_SP" /usr/include/rga/*.h | head -3
