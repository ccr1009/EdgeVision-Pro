#!/bin/bash
set -o pipefail
cd /root/stereo3d || exit 1
echo "=== cmake build ==="
cmake --build build -j8 2>&1 | tail -25
rc=${PIPESTATUS[0]}
echo "=== build exit=$rc ==="
[ $rc -ne 0 ] && exit $rc
echo "=== run gen_synthetic_stereo ==="
cd /root/stereo3d && ./build/gen_synthetic_stereo 2>&1
echo "=== run exit=$? ==="