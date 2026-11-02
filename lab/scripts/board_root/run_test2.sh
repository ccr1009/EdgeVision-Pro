#!/bin/bash
set -o pipefail
cd /root/stereo3d || exit 1
g++ -O2 -std=c++17 -Wall -Wextra /tmp/test_stereo.cpp src/stereo_pipeline.cpp -Iinclude $(pkg-config --cflags --libs opencv4) -o /tmp/test_stereo 2>&1 | tail -10
/tmp/test_stereo 2>&1 | grep -E 'PASS|FAIL|结果|=>'