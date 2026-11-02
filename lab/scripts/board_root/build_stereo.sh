#!/bin/bash
set -o pipefail
cd /root/stereo3d || exit 1
echo "=== files ==="
ls -l src include
echo "=== compile ==="
g++ -O2 -std=c++17 -Wall -Wextra -c src/stereo_pipeline.cpp -Iinclude $(pkg-config --cflags opencv4) -o /tmp/sp.o 2>&1
rc=$?
echo "=== exit=$rc ==="
if [ $rc -eq 0 ]; then echo "SIZE:"; ls -l /tmp/sp.o; fi