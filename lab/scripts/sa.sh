#!/bin/bash
echo "=== all wrapbuffer / importbuffer decls in im2d.h ==="
grep -n "wrapbuffer\|importbuffer\|im_handle_param" /usr/include/rga/im2d.h | head -40
echo "=== im_handle_param_t ==="
grep -n -A12 "im_handle_param" /usr/include/rga/im2d_type.h | head -25
