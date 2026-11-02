#!/bin/bash
set -e
cd /root/stereo3d
echo "=== 1) face_3d.cpp ==="
g++ -O2 -std=c++17 -Wall -Wextra -c src/face_3d.cpp -Iinclude $(pkg-config --cflags opencv4) -o /tmp/f3.o
echo "FACE3D_OK rc=$?"
echo "=== 2) http_server.cpp ==="
g++ -O2 -std=c++17 -Wall -Wextra -c src/http_server.cpp -Iinclude $(pkg-config --cflags opencv4) -pthread -o /tmp/hs.o
echo "HTTPSRV_OK rc=$?"
echo "=== 3) objects ==="
ls -l /tmp/f3.o /tmp/hs.o
echo "=== 4) symbols sanity ==="
nm -C /tmp/f3.o | grep -E " T s3d::Face3DLocator::" | sed 's/^[0-9a-f]* T //'
echo "---"
nm -C /tmp/hs.o | grep -E " T s3d::HttpPreview::" | sed 's/^[0-9a-f]* T //'