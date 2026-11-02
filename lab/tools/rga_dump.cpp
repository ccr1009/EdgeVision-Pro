// rga_dump.cpp ?? RGA ??? PPM + OpenCV ????
#include "rk_mpi.h"
#include "im2d.h"
#include "rga.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <sys/time.h>
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))

int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IONBF,0);
  const char* path = argc>1?argv[1]:"/tmp/mppo/a.jpg";
  int OW = argc>2?atoi(argv[2]):1280, OH = argc>3?atoi(argv[3]):720;
  const char* outp = argc>4?argv[4]:"/tmp/rga_out.ppm";

  FILE* fp=fopen(path,"rb"); if(!fp) return 1;
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

  MppPacket pkt=nullptr;
  mpp_packet_init_with_buffer(&pkt,ib);
  mpp_packet_set_length(pkt,sz);
  mpp_packet_set_pos(pkt,mpp_buffer_get_ptr(ib));
  mpp_meta_set_frame(mpp_packet_get_meta(pkt),KEY_OUTPUT_FRAME,frame);
  printf("put=%d\n", mpi->decode_put_packet(ctx,pkt));
  MppFrame fr=nullptr;
  for(int g=0;g<300;g++){
    if(mpi->decode_get_frame(ctx,&fr)||!fr) break;
    if(mpp_frame_get_info_change(fr)){ mpi->control(ctx,MPP_DEC_SET_INFO_CHANGE_READY,nullptr); fr=nullptr; continue; }
    break;
  }
  if(!fr){ printf("no frame\n"); return 1; }
  printf("frame %dx%d hs=%d vs=%d\n",(int)mpp_frame_get_width(fr),(int)mpp_frame_get_height(fr),
        (int)mpp_frame_get_hor_stride(fr),(int)mpp_frame_get_ver_stride(fr));

  int dec_fd=mpp_buffer_get_fd(fb), out_fd=mpp_buffer_get_fd(ob);
  rga_buffer_t src=wrapbuffer_fd_t(dec_fd,(int)mpp_frame_get_width(fr),(int)mpp_frame_get_height(fr),
      (int)mpp_frame_get_hor_stride(fr),(int)mpp_frame_get_ver_stride(fr),RK_FORMAT_YCbCr_420_SP);
  rga_buffer_t dst=wrapbuffer_fd_t(out_fd,OW,OH,OW,OH,RK_FORMAT_RGBA_8888);
  IM_STATUS st=imresize_t(src,dst,0,0,INTER_LINEAR,1);
  printf("RGA imresize=%d (%s)\n",(int)st, imStrError(st));

  // ???? (DRM uncached, ???)
  std::vector<uint8_t> rgba((size_t)OW*OH*4);
  const uint8_t* p=(const uint8_t*)mpp_buffer_get_ptr(ob);
  memcpy(rgba.data(),p,rgba.size());

  // ? PPM (RGB)
  FILE* o=fopen(outp,"wb");
  fprintf(o,"P6\n%d %d\n255\n",OW,OH);
  std::vector<uint8_t> rgb((size_t)OW*OH*3);
  for(size_t i=0;i<(size_t)OW*OH;i++){ rgb[i*3]=rgba[i*4]; rgb[i*3+1]=rgba[i*4+1]; rgb[i*3+2]=rgba[i*4+2]; }
  fwrite(rgb.data(),1,rgb.size(),o); fclose(o);
  printf("wrote %s\n", outp);

  long s=0; int mn=255,mx=0;
  for(size_t i=0;i<rgb.size();i++){int v=rgb[i];s+=v;if(v<mn)mn=v;if(v>mx)mx=v;}
  printf("RGB mean=%.1f min=%d max=%d  px[0]=(%d,%d,%d)\n",(double)s/rgb.size(),mn,mx,rgb[0],rgb[1],rgb[2]);

  mpi->reset(ctx); mpp_frame_deinit(&frame);
  mpp_buffer_put(fb);mpp_buffer_group_put(fg);
  mpp_buffer_put(ib);mpp_buffer_group_put(ig);
  mpp_buffer_put(ob);mpp_buffer_group_put(og);
  mpp_destroy(ctx); free(jpg); return 0;
}
