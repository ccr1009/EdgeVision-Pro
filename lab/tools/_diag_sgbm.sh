#!/bin/bash
# SGBM parallel diagnosis (task-5 step 1) - read-only, no camera access
echo "=== 1. CPU / OpenCV backend ==="
echo -n "nproc: "; nproc
echo -n "opencv4 version: "; pkg-config --modversion opencv4
CORESO=$(pkg-config --variable=libdir opencv4)/libopencv_core.so
echo "ldd libopencv_core parallel backends:"
ldd "$CORESO" 2>/dev/null | grep -Ei "tbb|gomp|omp" || echo "  (no tbb/libgomp found in ldd output)"

echo "=== 2. cv thread probe ==="
cat > /root/lab/probe_cv_threads.cpp <<'EOF'
#include <opencv2/core.hpp>
#include <chrono>
#include <cstdio>
int main(){
    printf("cv::getNumberOfCPUs()=%d\n", cv::getNumberOfCPUs());
    printf("cv::getNumThreads()=%d\n", cv::getNumThreads());
    const int N = 20000000;
    long par = 0, ser = 0;
    auto t0 = std::chrono::steady_clock::now();
    cv::parallel_for_(cv::Range(0, N), [&](const cv::Range& r){
        long s = 0; for(int i = r.start; i < r.end; i++) s += i % 7;
        __sync_fetch_and_add(&par, s);
    });
    auto t1 = std::chrono::steady_clock::now();
    for(int i = 0; i < N; i++) ser += i % 7;
    auto t2 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b){ return std::chrono::duration<double,std::milli>(b-a).count(); };
    printf("parallel_for_ sum=%ld  %.2f ms\n", par, ms(t0,t1));
    printf("serial loop   sum=%ld  %.2f ms  (speedup=%.2fx)\n", ser, ms(t1,t2), ms(t1,t2)/ms(t0,t1));
    return 0;
}
EOF
g++ -O2 -std=c++17 /root/lab/probe_cv_threads.cpp -o /root/lab/probe_cv_threads \
    $(pkg-config --cflags --libs opencv4) -lpthread && /root/lab/probe_cv_threads

echo "=== 3. NPU load / top CPU ==="
cat /sys/kernel/debug/rknpu/load 2>/dev/null || echo "rknpu load read failed"
top -bn1 | head -5

echo "=== 4. demo binary & edgevision service (status only) ==="
ls -l /root/stereo3d/build/stereo3d_demo 2>/dev/null || echo "stereo3d_demo MISSING"
echo -n "edgevision: "; systemctl is-active edgevision
