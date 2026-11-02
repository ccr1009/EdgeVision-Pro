#!/bin/bash
echo "=== around line 100-130 im2d_type.h ==="
sed -n '95,135p' /usr/include/rga/im2d_type.h
echo "=== imcvtcolor in im2d.hpp ==="
grep -n -B2 -A3 "imcvtcolor" /usr/include/rga/im2d.hpp | head -20
echo "=== rga.h color mode ==="
grep -n "full_range\|FULL_RANGE\|color_mode\|COLOR_MODE" /usr/include/rga/rga.h | head -20
