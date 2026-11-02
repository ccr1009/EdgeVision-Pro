#!/bin/bash
echo "=== color space mode enums ==="
grep -n "IM_COLOR_SPACE" /usr/include/rga/im2d_type.h | head -20
echo "=== imcvtcolor_t ==="
grep -n "imcvtcolor_t" /usr/include/rga/im2d.hpp | head -3
echo "=== imcomposite/improcess usage with opt ==="
grep -n "typedef struct.*im_opt_t\|} im_opt_t" /usr/include/rga/im2d_type.h
grep -n -A10 "im_opt_t {" /usr/include/rga/im2d_type.h | head -20
