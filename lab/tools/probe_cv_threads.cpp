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
