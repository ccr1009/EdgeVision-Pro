#!/bin/bash
S=/tmp/mppsrc/mpp-develop/mpp
echo "=== jpegd hal ??????? ==="
grep -rn "MPP_FMT_RGB\|MPP_FMT_BGR\|MPP_FMT_YUV422SP\|MPP_FMT_YUV420SP" $S/hal/rkdec/hal_jpegd_rkv.c $S/hal/rkdec/hal_jpegd_com.c 2>/dev/null | head -40
echo
echo "=== hal_jpegd_dec_set_out_fmt / ?????? ==="
grep -rn "set_out_fmt\|out_fmt\|yuv_mode\|input_yuv_mode" $S/hal/rkdec/*.c 2>/dev/null | head -30
echo
echo "=== mpp_dec ? ? jpeg ? output format ???? ==="
grep -rn "MPP_VIDEO_CodingMJPEG" $S/codec/mpp_dec*.c $S/codec/*.c 2>/dev/null | head -20
echo
echo "=== rga ????? full range ?? ==="
grep -rn "FULL\|LIMITED\|RANGE" /usr/include/rga/im2d.h /usr/include/rga/RgaApi.h /usr/include/rga/rga.h 2>/dev/null | head -30
