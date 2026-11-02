#!/bin/bash
echo "=== /dev/rga ==="
ls -la /dev/rga* 2>/dev/null || echo "NO /dev/rga"
echo "=== dri devices ==="
ls -la /dev/dri/
echo "=== rga in dmesg ==="
dmesg 2>/dev/null | grep -i rga | head -20
echo "=== rga2/rga3 nodes ==="
ls -la /dev/ | grep -i rga
echo "=== librga version strings ==="
strings /usr/lib/aarch64-linux-gnu/librga.so.2.1.0 | grep -iE "rga_version|2\.[0-9]\.[0-9]" | head -10
echo "=== devices tree ==="
ls /sys/class/ | grep -i rga
cat /sys/kernel/debug/rkrga/* 2>/dev/null | head -20
echo "=== devfreq ==="
ls /sys/class/devfreq/ 2>/dev/null
