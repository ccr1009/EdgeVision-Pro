// rga_nv12.cpp ?? MPP ?? NV12(DRM fd) -> RGA ?? CSC+?? -> RGB888
// ??: ??"??->???"???? CPU ? DRM ??
#include "rk_mpi.h"
#include "im2d.h"
#include "rga.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <sys/time.h>

static int64_t now_us(){ struct timeval tv; gettimeofday(&tv,nullptr); return (int64_t)tv.tv_sec*1000000+tv.tv_usec; }
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))

int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IONBF,0);
  const char* path = argc>1?argv[1]:"/tmp/mppo/a.jpg";
  int OW = argc>2?atoi(argv[2]):640, OH = argc>3?atoi(argv[3]):640;
  int N = argc>4?atoi(argv[4]):100;

  FILE* fp=fopen(path,"rb"); if(!fp){printf("open fail\n");return 1;}
  fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
  char* jpg=(char*)malloc(sz); fread(jpg,1,sz,fp); fclose(fp);

  int W=1280,H=720, hs=ALIGN(W,16), vs=ALIGN(H,16);
  MppCtx ctx; MppApi* mpi;
  mpp_create(&ctx,&mpi); mpp_init(ctx,MPP_CTX_DEC,MPP_VIDEO_CodingMJPEG);
  MppDecCfg cfg=nullptr; mpp_dec_cfg_init(&cfg); mpi->control(ctx,MPP_DEC_GET_CFG,cfg);
  mpp_dec_cfg_set_u32(cfg,"base:split_parse",1); mpi->control(ctx,MPP_DEC_SET_CFG,cfg);
  MppFrameFormat fmt=MPP_FMT_YUV420SP; mpi->control(ctx,MPP_DEC_SET_OUTPUT_FORMAT,&fmt);

  MppFrame frame=nullptr; mpp_frame_init(&frame);
  MppBufferGroup fg, ig;
  mpp_buffer_group_get_internal(&fg,MPP_BUFFER_TYPE_DRM);
  mpp_buffer_group_get_internal(&ig,MPP_BUFFER_TYPE_DRM);
  MppBuffer fb, ib;
  mpp_buffer_get(fg,&fb,(size_t)hs*vs*4);
  mpp_buffer_get(ig,&ib,sz);
  mpp_frame_set_buffer(frame,fb);
  memcpy(mpp_buffer_get_ptr(ib),jpg,sz);
  int dec_fd = mpp_buffer_get_fd(fb);
  printf("decoder DRM fd=%d size=%zu  out=%dx%d\n", dec_fd, mpp_buffer_get_size(fb), OW, OH);

  size_t out_size=(size_t)OW*OH*3;
  std::vector<uint8_t> outbuf(out_size, 0xAB);
  rga_buffer_t dst = wrapbuffer_virtualaddr(outbuf.data(), OW, OH, RK_FORMAT_RGB_888);
  printf("dst handle=%d\n", (int)dst.handle);

  std::vector<double> vd, vr;
  int ok=0;
  for(int it=-5; it<N; it++){
    MppPacket pkt=nullptr;
    mpp_packet_init_with_buffer(&pkt,ib);
    mpp_packet_set_length(pkt,sz);
    mpp_packet_set_pos(pkt,mpp_buffer_get_ptr(ib));
    mpp_meta_set_frame(mpp_packet_get_meta(pkt),KEY_OUTPUT_FRAME,frame);

    int64_t t0=now_us();
    int ret = mpi->decode_put_packet(ctx,pkt);
    MppFrame fr=nullptr;
    if(ret==0){
      for(int g=0;g<300;g++){
        if(mpi->decode_get_frame(ctx,&fr)||!fr) break;
        if(mpp_frame_get_info_change(fr)){ mpi->control(ctx,MPP_DEC_SET_INFO_CHANGE_READY,nullptr); fr=nullptr; continue; }
        break;
      }
    }
    int64_t t1=now_us();
    mpp_packet_deinit(&pkt);
    if(!fr){ if(it>=0) printf("it=%d NO FRAME ret=%d\n",it,ret); continue; }

    // ? ??: ? DRM fd ???? RGA buffer, ? CPU ??
    rga_buffer_t src = wrapbuffer_fd(dec_fd, (int)mpp_frame_get_width(fr), (int)mpp_frame_get_height(fr),
                                     RK_FORMAT_YCbCr_420_SP,
                                     (int)mpp_frame_get_hor_stride(fr), (int)mpp_frame_get_ver_stride(fr));
    if(src.handle==0){ printf("wrap src fail\n"); break; }

    int64_t t2=now_us();
    IM_STATUS st = imresize(src, dst);   // ? CSC + ??, ????
    int64_t t3=now_us();

    if(st!=IM_STATUS_SUCCESS){ printf("RGA fail: %s\n", imStrError(st)); break; }
    if(it>=0){ vd.push_back((t1-t0)/1000.0); vr.push_back((t3-t2)/1000.0); ok++; }
  }

  auto stats=[](const char* n, std::vector<double>& v){
    if(v.empty()){printf("%-8s no data\n",n);return;}
    std::sort(v.begin(),v.end());
    double s=0; for(double x:v) s+=x;
    printf("%-8s avg=%.3f p50=%.3f p95=%.3f p99=%.3f min=%.3f max=%.3f ms\n",
      n, s/v.size(), v[v.size()/2], v[(size_t)(v.size()*0.95)], v[(size_t)(v.size()*0.99)], v.front(), v.back());
  };
  printf("\n=== N=%d ok=%d ===\n",N,ok);
  stats("decode",vd); stats("rga",vr);
  if(!vd.empty() && !vr.empty()){
    double d=0,r=0; for(double x:vd)d+=x; for(double x:vr)r+=x;
    d/=vd.size(); r/=vr.size();
    printf("decode+rga = %.3f ms  => %.1f FPS\n", d+r, 1000.0/(d+r));
  }

  long sum=0; int mn=255,mx=0,cnt171=0;
  for(size_t i=0;i<out_size;i++){ int v=outbuf[i]; sum+=v; if(v<mn)mn=v; if(v>mx)mx=v; if(v==171)cnt171++; }
  printf("RGB mean=%.1f min=%d max=%d  unwritten(0xAB)=%d/%zu\n", (double)sum/out_size, mn, mx, cnt171, out_size);

  FILE* o=fopen("/tmp/rga_out.ppm","wb");
  fprintf(o,"P6\n%d %d\n255\n",OW,OH); fwrite(outbuf.data(),1,out_size,o); fclose(o);
  printf("wrote /tmp/rga_out.ppm\n");

  mpi->reset(ctx); mpp_frame_deinit(&frame);
  mpp_buffer_put(fb); mpp_buffer_group_put(fg);
  mpp_buffer_put(ib); mpp_buffer_group_put(ig); mpp_destroy(ctx);
  free(jpg); return 0;
}
