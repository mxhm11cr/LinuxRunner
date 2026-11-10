#include "RFBProtocol.h"
#include <string.h>
bool lr_rfb_dimensions(uint16_t w,uint16_t h,size_t *bytes) {
    if(!w||!h||w>LR_RFB_MAX_WIDTH||h>LR_RFB_MAX_HEIGHT||!bytes)return false;
    *bytes=(size_t)w*h*4;return true;
}
bool lr_rfb_rectangle(uint16_t fw,uint16_t fh,uint16_t x,uint16_t y,uint16_t w,uint16_t h,size_t *bytes) {
    size_t total;
    if(!lr_rfb_dimensions(fw,fh,&total)||!bytes||x>fw||y>fh||w>fw-x||h>fh-y)return false;
    *bytes=(size_t)w*h*4;return true;
}
bool lr_rfb_apply_raw(uint8_t *frame,uint16_t fw,uint16_t fh,uint16_t x,uint16_t y,uint16_t w,uint16_t h,const uint8_t *source,size_t count) {
    size_t bytes;
    if(!frame||!source||!lr_rfb_rectangle(fw,fh,x,y,w,h,&bytes)||count!=bytes)return false;
    for(size_t row=0;row<h;row++)memcpy(frame+((size_t)(y+row)*fw+x)*4,source+row*w*4,(size_t)w*4);
    return true;
}
uint8_t lr_rfb_reverse_bits(uint8_t n){n=(n>>4)|(n<<4);n=((n&0xcc)>>2)|((n&0x33)<<2);return ((n&0xaa)>>1)|((n&0x55)<<1);}
bool lr_rfb_fill(uint8_t *frame,uint16_t fw,uint16_t fh,uint16_t x,uint16_t y,uint16_t w,uint16_t h,const uint8_t color[4]) {
    size_t bytes;
    if(!frame||!color||!lr_rfb_rectangle(fw,fh,x,y,w,h,&bytes))return false;
    for(size_t row=y;row<(size_t)y+h;row++)for(size_t col=x;col<(size_t)x+w;col++)memcpy(frame+(row*fw+col)*4,color,4);
    return true;
}

#include <math.h>
bool lr_rfb_viewport_point(double x,double y,double left,double top,double rw,double rh,uint16_t w,uint16_t h,uint16_t *px,uint16_t *py){
 size_t bytes;if(!px||!py||!lr_rfb_dimensions(w,h,&bytes)||!isfinite(x)||!isfinite(y)||!isfinite(left)||!isfinite(top)||!isfinite(rw)||!isfinite(rh)||rw<=0||rh<=0)return false;
 double gx=(x-left)*w/rw,gy=(y-top)*h/rh;
 *px=(uint16_t)fmax(0,fmin(w-1,gx));*py=(uint16_t)fmax(0,fmin(h-1,gy));return true;
}
bool lr_rfb_cursor_sizes(uint16_t w,uint16_t h,size_t *pixel_bytes,size_t *mask_bytes){
 if(!pixel_bytes||!mask_bytes||w>LR_RFB_CURSOR_MAX||h>LR_RFB_CURSOR_MAX)return false;
 *pixel_bytes=(size_t)w*h*4;*mask_bytes=(size_t)((w+7)/8)*h;return true;
}
bool lr_rfb_cursor_bgra(const uint8_t *pixels,const uint8_t *mask,uint16_t w,uint16_t h,uint8_t *out){
 size_t pb,mb;if(!pixels||!mask||!out||!lr_rfb_cursor_sizes(w,h,&pb,&mb))return false;
 size_t stride=(w+7)/8;
 for(size_t y=0;y<h;y++)for(size_t x=0;x<w;x++){
  const uint8_t *s=pixels+(y*w+x)*4;uint8_t *d=out+(y*w+x)*4;
  bool on=(mask[y*stride+x/8]>>(7-x%8))&1;
  d[0]=on?s[0]:0;d[1]=on?s[1]:0;d[2]=on?s[2]:0;d[3]=on?255:0;
 }
 return true;
}
