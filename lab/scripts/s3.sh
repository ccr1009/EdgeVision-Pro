#!/bin/bash
cd /tmp/mppsrc/mpp-develop
echo "=== mpp_buffer.h flags ==="
grep -n "FLAGS\|CACHABLE\|MppBufferFlag" inc/mpp_buffer.h | head -30
echo "=== mpp_buffer_group_set_buffer_flags ==="
grep -rn "mpp_buffer_group_set_buffer_flags" inc/*.h | head
echo "=== demo get_frame/release 100-150 ==="
sed -n '100,150p' test/mpi_dec_test.c
echo "=== how demo releases frames ==="
grep -n "mpp_frame_deinit\|MPP_DEC_SET_INFO_CHANGE_READY" test/mpi_dec_test.c | head
