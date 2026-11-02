// rga_min.cpp ?? RGA ??????? (???, ??? _t ??)
#include "im2d.h"
#include "rga.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main(){
  setvbuf(stdout,NULL,_IONBF,0);
  printf("=== RGA minimal probe ===\n");
  int OW=640, OH=640;
  std::vector<unsigned char> dst((size_t)OW*OH*4, 0);

  rga_buffer_t d = wrapbuffer_virtualaddr_t(dst.data(), OW, OH, OW, OH, RK_FORMAT_RGBA_8888);
  printf("dst.handle=%d vir=%p fd=%d %dx%d wstride=%d hstride=%d fmt=0x%x\n",
     (int)d.handle, d.vir_addr, d.fd, d.width, d.height, d.wstride, d.hstride, d.format);

  im_rect full; full.x=0; full.y=0; full.width=OW; full.height=OH;
  IM_STATUS st = imfill_t(d, full, 0x00FF00FF, IM_SYNC);
  printf("imfill_t ret=%d (%s)\n", (int)st, imStrError(st));
  printf("pixel[0..3]=%02x %02x %02x %02x\n", dst[0],dst[1],dst[2],dst[3]);

  printf("=== im_handle_param_t import ===\n");
  im_handle_param_t p; memset(&p,0,sizeof(p));
  p.width=OW; p.height=OH; p.format=RK_FORMAT_RGBA_8888;
  rga_buffer_handle_t h = importbuffer_virtualaddr(dst.data(), &p);
  printf("handle=%d\n", (int)h);

  printf("=== imcheck ===\n");
  im_rect sr; sr.x=0;sr.y=0;sr.width=OW;sr.height=OH;
  im_rect dr = sr;
  im_rect pr; pr.x=0;pr.y=0;pr.width=0;pr.height=0;
  IM_STATUS ck = imcheck(d, d, pr, sr, dr);
  printf("imcheck ret=%d (%s)\n",(int)ck, imStrError(ck));
  return 0;
}
