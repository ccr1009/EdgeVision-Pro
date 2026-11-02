#!/bin/bash
echo "=== where is RK_FORMAT_RGB_888 defined ==="
grep -rn "RK_FORMAT_RGB_888" /usr/include/rga/*.h | head -5
echo "=== im2d_type.h head includes ==="
sed -n '1,40p' /usr/include/rga/im2d_type.h
