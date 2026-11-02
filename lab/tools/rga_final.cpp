// rga_final.cpp ?? MPP ?? NV12(DRM fd) --RGA full-range CSC+scale--> RGBA8888(DRM fd)
// ??: ? IM_YUV_TO_RGB_BT601_FULL | IM_RGB_FULL ??????
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
  int MODE = argc>5?atoi(argv[5]):0;  // 0=default 1=full

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
  MppBufferGroup fg,ig,og;
  mpp_buffer_group_get_internal(&fg,MPP_BUFFER_TYPE_DRM);
  mpp_buffer_group_get_internal(&ig,MPP_BUFFER_TYPE_DRM);
  mpp_buffer_group_get_internal(&og,MPP_BUFFER_TYPE_DRM);
  MppBuffer fb,ib,ob;
  mpp_buffer_get(fg,&fb,(size_t)hs*vs*4);
  mpp_buffer_get(ig,&ib,sz);
  mpp_buffer_get(og,&ob,(size_t)OW*OH*4);
  mpp_frame_set_buffer(frame,fb);
  memcpy(mpp_buffer_get_ptr(ib),jpg,sz);
  int dec_fd=mpp_buffer_get_fd(fb), out_fd=mpp_buffer_get_fd(ob);

  rga_buffer_t dst = wrapbuffer_fd_t(out_fd, OW, OH, OW, OH, RK_FORMAT_RGBA_8888);

  std::vector<double> vd, vr;
  int ok=0;
  for(int it=-5; it<N; it++){
    MppPacket pkt=nullptr;
    mpp_packet_init_with_buffer(&pkt,ib);
    mpp_packet_set_length(pkt,sz);
    mpp_packet_set_pos(pkt,mpp_buffer_get_ptr(ib));
    mpp_meta_set_frame(mpp_packet_get_meta(pkt),KEY_OUTPUT_FRAME,frame);
    int64_t t0=now_us();
    int ret=mpi->decode_put_packet(ctx,pkt);
    MppFrame fr=nullptr;
    if(ret==0) for(int g=0;g<300;g++){
      if(mpi->decode_get_frame(ctx,&fr)||!fr) break;
      if(mpp_frame_get_info_change(fr)){mpi->control(ctx,MPP_DEC_SET_INFO_CHANGE_READY,nullptr);fr=nullptr;continue;}
      break;
    }
    int64_t t1=now_us();
    mpp_packet_deinit(&pkt);
    if(!fr){ if(it>=0)printf("it=%d NOFRAME ret=%d\n",it,ret); continue; }

    rga_buffer_t src = wrapbuffer_fd_t(dec_fd,(int)mpp_frame_get_width(fr),(int)mpp_frame_get_height(fr),
        (int)mpp_frame_get_hor_stride(fr),(int)mpp_frame_get_ver_stride(fr), RK_FORMAT_YCbCr_420_SP);

    int64_t t2=now_us();
    IM_STATUS st;
    if(MODE==1){
      // ???: ? imcvtcolor ? RGB (full range), ? imresize
      rga_buffer_t tmp = wrapbuffer_fd_t(out_fd, OW, OH, OW, OH, RK_FORMAT_RGBA_8888);
      st = imcvtcolor(src, tmp, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGBA_8888,
                      IM_YUV_TO_RGB_BT601_FULL | IM_RGB_FULL, 1);
    } else {
      st = imresize_t(src, dst, 0, 0, INTER_LINEAR, 1);
    }
    int64_t t3=now_us();
    if(st!=IM_STATUS_SUCCESS){ if(it>=0)printf("RGA fail: %s\n", imStrError(st)); break; }
    if(it>=0){ vd.push_back((t1-t0)/1000.0); vr.push_back((t3-t2)/1000.0); ok++; }
  }

  auto stats=[](const char* n, std::vector<double>& v){
    if(v.empty()){printf("%-8s no data\n",n);return;}
    std::sort(v.begin(),v.end());
    double s=0; for(double x:v) s+=x;
    printf("%-8s avg=%.3f p50=%.3f p95=%.3f p99=%.3f min=%.3f max=%.3f\n",
      n,s/v.size(),v[v.size()/2],v[(size_t)(v.size()*0.95)],v[(size_t)(v.size()*0.99)],v.front(),v.back());
  };
  printf("\n=== MODE=%d (%s) N=%d ok=%d  out=%dx%d ===\n",MODE, MODE?"full-range":"default",N,ok,OW,OH);
  stats("decode",vd); stats("rga",vr);

  // dump ??????? (? dump ??, ?????)
  std::vector<uint8_t> rgba((size_t)OW*OH*4);
  memcpy(rgba.data(), mpp_buffer_get_ptr(ob), rgba.size());
  char outpath[256];
  snprintf(outpath,sizeof(outpath),"/tmp/rga_m%d.ppm",MODE);
  FILE* o=fopen(outpath,"wb");
  fprintf(o,"P6\n%d %d\n255\n",OW,OH);
  for(size_t i=0;i<(size_t)OW*OH;i++){ fputc(rgba[i*4],o); fputc(rgba[i*4+1],o); fputc(rgba[i*4+2],o); }
  fclose(o);
  printf("wrote %s\n", outpath);

  mpi->reset(ctx); mpp_frame_deinit(&frame);
  mpp_buffer_put(fb);mpp_buffer_group_put(fg);
  mpp_buffer_put(ib);mpp_buffer_group_put(ig);
  mpp_buffer_put(ob);mpp_buffer_group_put(og);
  mpp_destroy(ctx); free(jpg); return 0;
}
