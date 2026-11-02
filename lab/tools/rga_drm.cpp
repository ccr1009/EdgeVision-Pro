// rga_drm.cpp ?? RGA ? DRM ??: ?? DRM buf -> RGA -> ?? DRM buf
// ?????? MPP DRM buffer ??, ? NPU ?? (????)
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

  printf("=== RGA ? DRM ???? ===\n");

  FILE* fp=fopen(path,"rb"); if(!fp){printf("open fail\n");return 1;}
  fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
  char* jpg=(char*)malloc(sz); fread(jpg,1,sz,fp); fclose(fp);
  int W=1280,H=720, hs=ALIGN(W,16), vs=ALIGN(H,16);

  // --- MPP ??? ---
  MppCtx ctx; MppApi* mpi;
  if(mpp_create(&ctx,&mpi)){printf("mpp_create fail\n");return 1;}
  if(mpp_init(ctx,MPP_CTX_DEC,MPP_VIDEO_CodingMJPEG)){printf("mpp_init fail\n");return 1;}
  MppDecCfg cfg=nullptr; mpp_dec_cfg_init(&cfg); mpi->control(ctx,MPP_DEC_GET_CFG,cfg);
  mpp_dec_cfg_set_u32(cfg,"base:split_parse",1); mpi->control(ctx,MPP_DEC_SET_CFG,cfg);
  MppFrameFormat fmt=MPP_FMT_YUV420SP; mpi->control(ctx,MPP_DEC_SET_OUTPUT_FORMAT,&fmt);

  MppFrame frame=nullptr; mpp_frame_init(&frame);
  MppBufferGroup fg, ig, og;
  mpp_buffer_group_get_internal(&fg,MPP_BUFFER_TYPE_DRM);
  mpp_buffer_group_get_internal(&ig,MPP_BUFFER_TYPE_DRM);
  mpp_buffer_group_get_internal(&og,MPP_BUFFER_TYPE_DRM);
  MppBuffer fb, ib, ob;
  mpp_buffer_get(fg,&fb,(size_t)hs*vs*4);
  mpp_buffer_get(ig,&ib,sz);
  mpp_buffer_get(og,&ob,(size_t)OW*OH*4);
  mpp_frame_set_buffer(frame,fb);
  memcpy(mpp_buffer_get_ptr(ib),jpg,sz);
  int dec_fd=mpp_buffer_get_fd(fb), out_fd=mpp_buffer_get_fd(ob);
  printf("decode DRM fd=%d  out DRM fd=%d  out size=%zu\n", dec_fd, out_fd, mpp_buffer_get_size(ob));

  // --- RGA ??? DRM fd ---
  rga_buffer_t dst = wrapbuffer_fd_t(out_fd, OW, OH, OW, OH, RK_FORMAT_RGBA_8888);
  printf("dst.handle=%d fd=%d vir=%p\n",(int)dst.handle, dst.fd, dst.vir_addr);
  if(dst.handle==0){ printf("wrap dst fail (handle=0)\n"); }

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
      if(mpp_frame_get_info_change(fr)){ mpi->control(ctx,MPP_DEC_SET_INFO_CHANGE_READY,nullptr); fr=nullptr; continue; }
      break;
    }
    int64_t t1=now_us();
    mpp_packet_deinit(&pkt);
    if(!fr){ if(it>=0) printf("it=%d NOFRAME ret=%d\n",it,ret); continue; }

    rga_buffer_t src = wrapbuffer_fd_t(dec_fd,(int)mpp_frame_get_width(fr),(int)mpp_frame_get_height(fr),
        (int)mpp_frame_get_hor_stride(fr),(int)mpp_frame_get_ver_stride(fr), RK_FORMAT_YCbCr_420_SP);
    int64_t t2=now_us();
    IM_STATUS st = imresize_t(src, dst, 0, 0, INTER_LINEAR, 1);
    int64_t t3=now_us();
    if(st!=IM_STATUS_SUCCESS){ if(it>=0) printf("RGA fail: %s (%d)\n", imStrError(st), (int)st); break; }
    if(it>=0){ vd.push_back((t1-t0)/1000.0); vr.push_back((t3-t2)/1000.0); ok++; }
  }

  auto stats=[](const char* n, std::vector<double>& v){
    if(v.empty()){printf("%-8s no data\n",n);return;}
    std::sort(v.begin(),v.end());
    double s=0; for(double x:v) s+=x;
    printf("%-8s avg=%.3f p50=%.3f p95=%.3f p99=%.3f min=%.3f max=%.3f\n",
      n,s/v.size(),v[v.size()/2],v[(size_t)(v.size()*0.95)],v[(size_t)(v.size()*0.99)],v.front(),v.back());
  };
  printf("\n=== N=%d ok=%d ===\n",N,ok);
  stats("decode",vd); stats("rga",vr);
  if(!vd.empty()&&!vr.empty()){ double d=0,r=0; for(double x:vd)d+=x; for(double x:vr)r+=x; d/=vd.size(); r/=vr.size();
    printf("decode+rga=%.3f ms => %.1f FPS\n", d+r, 1000.0/(d+r)); }

  // ?????? (DRM uncached, ?? 64x64 ?????)
  const uint8_t* p=(const uint8_t*)mpp_buffer_get_ptr(ob);
  long sum=0; int mn=255,mx=0;
  for(int j=0;j<64;j++) for(int i=0;i<64*4;i++){ int v=p[(size_t)j*OW*4+i]; sum+=v; if(v<mn)mn=v; if(v>mx)mx=v; }
  printf("out[64x64 sample] mean=%.1f min=%d max=%d\n",(double)sum/(64*64*4),mn,mx);

  mpi->reset(ctx); mpp_frame_deinit(&frame);
  mpp_buffer_put(fb); mpp_buffer_group_put(fg);
  mpp_buffer_put(ib); mpp_buffer_group_put(ig);
  mpp_buffer_put(ob); mpp_buffer_group_put(og);
  mpp_destroy(ctx); free(jpg); return 0;
}
