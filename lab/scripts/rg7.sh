#!/bin/bash
S=/tmp/mppsrc/mpp-develop/mpp
echo "=== ? jpegd ??? ==="
find $S -iname "*jpegd*" 2>/dev/null | head -30
echo
echo "=== ? jpega / jpeg ?? ==="
find $S -iname "*jpeg*" 2>/dev/null | head -30
echo
echo "=== RGA ????? ==="
ls /usr/include/rga/
echo
echo "=== im2d.h ?? mode ??? ==="
grep -n "IM_YUV_TO_RGB\|IM_RGB_TO_YUV\|_FULL\|_LIMITED\|rect_" /usr/include/rga/im2d.h 2>/dev/null | head -40
echo
echo "=== im2d_common.h / RgaUtils ?? color space ==="
grep -rn "FULL_RANGE\|LIMITED_RANGE\|full_range\|limit_range" /usr/include/rga/ 2>/dev/null | head -20
