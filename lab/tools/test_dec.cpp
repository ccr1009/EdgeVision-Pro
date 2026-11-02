// test_mpp_decoder.cpp ?? ?? MppJpegDecoder ???????
#include "mpp_jpeg_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <sys/time.h>
#include <opencv2/opencv.hpp>

static int64_t now_us(){ struct timeval tv; gettimeofday(&tv,nullptr); return (int64_t)tv.tv_sec*1000000+tv.tv_usec; }

int main(int argc, char** argv){
    const char* path = argc>1 ? argv[1] : "/tmp/mppo/a.jpg";
    int N = argc>2 ? atoi(argv[2]) : 200;

    // ??
    FILE* fp = fopen(path,"rb"); if(!fp){ printf("open fail %s\n",path); return 1; }
    fseek(fp,0,SEEK_END); long sz = ftell(fp); fseek(fp,0,SEEK_SET);
    std::vector<uint8_t> jpg(sz); fread(jpg.data(),1,sz,fp); fclose(fp);
    printf("file=%s size=%ld\n", path, sz);

    edgevision::MppJpegDecoder dec;
    int r = dec.init(1280,720);
    printf("init=%d ready=%d\n", r, dec.is_ready());
    if(r){ printf("err=%s\n", dec.last_error().c_str()); return 1; }

    // ????
    edgevision::DecodedFrame f;
    r = dec.decode(jpg.data(), jpg.size(), &f);
    printf("decode=%d valid=%d %dx%d stride=%dx%d fmt=0x%x ms=%.3f\n",
           r, f.valid, f.width, f.height, f.hor_stride, f.ver_stride, f.format, f.decode_ms);
    if(r || !f.valid){ printf("err=%s\n", dec.last_error().c_str()); return 1; }

    // ?? Y ????
    long ysum=0; int ymin=255,ymax=0;
    for(int j=0;j<f.height;j++){
        const uint8_t* row = f.data + (size_t)j*f.hor_stride;
        for(int i=0;i<f.width;i++){ int v=row[i]; ysum+=v; if(v<ymin)ymin=v; if(v>ymax)ymax=v; }
    }
    printf("Y plane: mean=%.1f min=%d max=%d\n", (double)ysum/(f.width*f.height), ymin, ymax);

    // ? RGB ???
    std::vector<uint8_t> rgb((size_t)f.width*f.height*3);
    int64_t t0=now_us();
    dec.to_rgb888(f, rgb.data());
    printf("to_rgb888: %.3f ms\n", (now_us()-t0)/1000.0);
    printf("RGB[0..8] = %d %d %d  %d %d %d  %d %d %d\n",
           rgb[0],rgb[1],rgb[2],rgb[3],rgb[4],rgb[5],rgb[6],rgb[7],rgb[8]);
    cv::Mat img(f.height, f.width, CV_8UC3, rgb.data());
    cv::imwrite("/tmp/mpp_lead_out.jpg", img);
    printf("wrote /tmp/mpp_lead_out.jpg\n");

    // ????
    std::vector<double> lat; lat.reserve(N);
    int ok=0, fail=0;
    for(int i=0;i<N;i++){
        int64_t a=now_us();
        int rr = dec.decode(jpg.data(), jpg.size(), &f);
        double ms=(now_us()-a)/1000.0;
        if(rr==0 && f.valid){ ok++; lat.push_back(ms); } else fail++;
    }
    std::sort(lat.begin(), lat.end());
    auto P=[&](double p){ return lat[(size_t)(p*(lat.size()-1))]; };
    double sum=0; for(double v:lat) sum+=v;
    printf("\n=== BENCH N=%d ok=%d fail=%d ===\n", N, ok, fail);
    printf("avg=%.3f p50=%.3f p95=%.3f p99=%.3f min=%.3f max=%.3f ms\n",
           sum/lat.size(), P(0.50),P(0.95),P(0.99), lat.front(), lat.back());
    printf("%.1f FPS\n", 1000.0/(sum/lat.size()));

    // OpenCV ??
    std::vector<double> slat;
    for(int i=0;i<50;i++){
        int64_t a=now_us(); cv::Mat m=cv::imdecode(jpg, cv::IMREAD_COLOR); double ms=(now_us()-a)/1000.0;
        if(!m.empty()) slat.push_back(ms);
    }
    std::sort(slat.begin(),slat.end());
    double ssum=0; for(double v:slat) ssum+=v;
    printf("soft imdecode avg=%.3f p50=%.3f ms\n", ssum/slat.size(), slat[slat.size()/2]);
    printf("SPEEDUP = %.2fx\n", (ssum/slat.size())/(sum/lat.size()));
    return 0;
}
