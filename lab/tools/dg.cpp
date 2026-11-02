// dg.cpp ?? ??: ??????? + DRM buffer ????
#include "rk_mpi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <sys/time.h>
static int64_t now_us(){ struct timeval tv; gettimeofday(&tv,nullptr); return (int64_t)tv.tv_sec*1000000+tv.tv_usec; }
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
int main(){
    setvbuf(stdout,NULL,_IONBF,0);
    const char* path="/tmp/mppo/a.jpg";
    FILE* fp=fopen(path,"rb"); fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    char* jpg=(char*)malloc(sz); fread(jpg,1,sz,fp); fclose(fp);
    int W=1280,H=720; int hs=ALIGN(W,16), vs=ALIGN(H,16);

    MppCtx ctx; MppApi* mpi;
    mpp_create(&ctx,&mpi); mpp_init(ctx,MPP_CTX_DEC,MPP_VIDEO_CodingMJPEG);
    MppDecCfg cfg=nullptr; mpp_dec_cfg_init(&cfg); mpi->control(ctx,MPP_DEC_GET_CFG,cfg);
    mpp_dec_cfg_set_u32(cfg,"base:split_parse",1); mpi->control(ctx,MPP_DEC_SET_CFG,cfg);
    MppFrameFormat fmt=MPP_FMT_YUV420SP; mpi->control(ctx,MPP_DEC_SET_OUTPUT_FORMAT,&fmt);

    MppFrame frame=nullptr; mpp_frame_init(&frame);
    MppBufferGroup fg=nullptr, ig=nullptr;
    mpp_buffer_group_get_internal(&fg,MPP_BUFFER_TYPE_DRM);
    mpp_buffer_group_get_internal(&ig,MPP_BUFFER_TYPE_DRM);
    MppBuffer fb=nullptr, ib=nullptr;
    mpp_buffer_get(fg,&fb,(size_t)hs*vs*4);
    mpp_buffer_get(ig,&ib,sz);
    mpp_frame_set_buffer(frame,fb);
    memcpy(mpp_buffer_get_ptr(ib),jpg,sz);

    MppPacket pkt=nullptr;
    mpp_packet_init_with_buffer(&pkt,ib);
    mpp_packet_set_length(pkt,sz);
    mpp_packet_set_pos(pkt,mpp_buffer_get_ptr(ib));
    mpp_meta_set_frame(mpp_packet_get_meta(pkt),KEY_OUTPUT_FRAME,frame);
    printf("put=%d\n", mpi->decode_put_packet(ctx,pkt));

    MppFrame fr=nullptr;
    for(int g=0;g<200;g++){ if(mpi->decode_get_frame(ctx,&fr)||!fr) break;
        if(mpp_frame_get_info_change(fr)){ mpi->control(ctx,MPP_DEC_SET_INFO_CHANGE_READY,nullptr); continue; } break; }

    printf("returned frame=%p  our frame=%p  SAME=%d\n",(void*)fr,(void*)frame, fr==frame);
    printf("stride %dx%d buf=%p fd=%d size=%zu\n",
      mpp_frame_get_hor_stride(fr), mpp_frame_get_ver_stride(fr),
      mpp_buffer_get_ptr(mpp_frame_get_buffer(fr)), mpp_buffer_get_fd(mpp_frame_get_buffer(fr)),
      mpp_buffer_get_size(mpp_frame_get_buffer(fr)));

    const uint8_t* src=(const uint8_t*)mpp_buffer_get_ptr(mpp_frame_get_buffer(fr));
    size_t nv12=(size_t)hs*vs*3/2;
    std::vector<uint8_t> dst(nv12);

    // memcpy ??
    for(int k=0;k<3;k++){
      int64_t t0=now_us(); memcpy(dst.data(),src,nv12); int64_t t1=now_us();
      printf("memcpy NV12 %.2f MB: %.3f ms  (%.0f MB/s)\n", nv12/1048576.0,(t1-t0)/1000.0, nv12/(double)(t1-t0));
    }
    // ?????? (uncached ??)
    int64_t t0=now_us(); long s=0;
    for(int j=0;j<H;j++){ const uint8_t* r=src+(size_t)j*hs; for(int i=0;i<W;i++) s+=r[i]; }
    printf("direct RMW read %dx%d: %.3f ms (sum=%ld)\n",W,H,(now_us()-t0)/1000.0,s);
    // ?? cached ????
    t0=now_us(); s=0;
    for(int j=0;j<H;j++){ const uint8_t* r=dst.data()+(size_t)j*hs; for(int i=0;i<W;i++) s+=r[i]; }
    printf("cached read: %.3f ms (sum=%ld)\n",(now_us()-t0)/1000.0,s);

    mpi->reset(ctx); mpp_frame_deinit(&frame);
    mpp_buffer_put(fb); mpp_buffer_group_put(fg);
    mpp_buffer_put(ib); mpp_buffer_group_put(ig); mpp_destroy(ctx);
    free(jpg); return 0;
}
