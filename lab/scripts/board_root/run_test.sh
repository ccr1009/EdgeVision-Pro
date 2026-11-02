#!/bin/bash
set -o pipefail
cd /root/stereo3d || exit 1
echo "=== 编译自测 ==="
g++ -O2 -std=c++17 -Wall -Wextra /tmp/test_stereo.cpp src/stereo_pipeline.cpp -Iinclude $(pkg-config --cflags --libs opencv4) -o /tmp/test_stereo 2>&1
rc=$?
echo "=== compile exit=$rc ==="
[ $rc -ne 0 ] && exit $rc
echo "=== 运行 ==="
/tmp/test_stereo 2>&1
echo "=== run exit=$? ==="