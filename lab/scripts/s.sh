#!/bin/bash
cd /tmp/mppsrc/mpp-develop 2>/dev/null || { echo "no src"; exit 1; }
echo "=== grep NULL input frame ==="
grep -rn "found NULL input frame" --include=*.c | head -5
echo "=== context around it ==="
grep -rn -B8 -A4 "found NULL input frame" mpp/mpi_dec.c 2>/dev/null | head -40
echo "=== what the demo passes to release ==="
grep -rn "decode_get_frame" test/mpi_dec_test.c | head -20
echo "=== RGA headers ==="
ls /usr/include/rga/ 2>/dev/null
echo "=== librga ==="
ls -la /usr/lib/aarch64-linux-gnu/librga* 2>/dev/null
