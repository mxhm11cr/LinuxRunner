#include "app/LinuxRunner/RFBProtocol.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void){
    size_t bytes=0;assert(lr_rfb_dimensions(640,480,&bytes)&&bytes==1228800);
    assert(!lr_rfb_dimensions(0,1,&bytes));assert(!lr_rfb_dimensions(2561,1,&bytes));assert(!lr_rfb_dimensions(1,1601,&bytes));
    assert(lr_rfb_dimensions(2560,1600,&bytes)&&bytes==16384000);assert(lr_rfb_dimensions(824,1200,&bytes));  // Fit Screen / external displays
    uint8_t storage[4*4*4+32];memset(storage,0xa5,sizeof storage);uint8_t source[16];memset(source,0x42,sizeof source);
    assert(lr_rfb_apply_raw(storage+16,4,4,1,1,2,2,source,16));
    assert(storage[16+(1*4+1)*4]==0x42&&storage[16+(2*4+2)*4]==0x42&&storage[16]==0xa5);
    assert(!lr_rfb_apply_raw(storage+16,4,4,3,3,2,2,source,16));
    assert(!lr_rfb_apply_raw(storage+16,4,4,1,1,2,2,source,15));
    assert(!lr_rfb_rectangle(4,4,65535,0,1,1,&bytes));
    assert(!lr_rfb_rectangle(4,4,4,0,1,1,&bytes));
    assert(lr_rfb_reverse_bits(0x12)==0x48);
    for(size_t i=0;i<16;i++)assert(storage[i]==0xa5&&storage[sizeof(storage)-i-1]==0xa5);
    uint8_t color[4]={1,2,3,4};assert(lr_rfb_fill(storage+16,4,4,3,3,1,1,color));
    assert(storage[16+60]==1&&storage[16+63]==4);
    assert(!lr_rfb_fill(storage+16,4,4,3,3,2,1,color));
    for(size_t i=0;i<16;i++)assert(storage[i]==0xa5&&storage[sizeof(storage)-i-1]==0xa5);
    srand(47);for(unsigned i=0;i<50000;i++){
        uint16_t x=rand(),y=rand(),w=rand(),h=rand();
        if(lr_rfb_rectangle(640,480,x,y,w,h,&bytes))assert((size_t)x+w<=640&&(size_t)y+h<=480&&bytes<=1228800);
    }
    // Cursor pseudo-encoding: 9x2 cursor, mask stride 2 bytes, MSB first.
    {size_t pb=0,mb=0;assert(lr_rfb_cursor_sizes(9,2,&pb,&mb)&&pb==72&&mb==4);
     assert(!lr_rfb_cursor_sizes(257,1,&pb,&mb));assert(lr_rfb_cursor_sizes(0,0,&pb,&mb)&&pb==0&&mb==0);
     uint8_t px[72];for(int i=0;i<72;i++)px[i]=(uint8_t)(i+1);
     const uint8_t m[4]={0x80,0x80,0x01,0x00}; // row0: x=0 and x=8 on; row1: x=7 on
     uint8_t out[72];memset(out,0x55,sizeof out);assert(lr_rfb_cursor_bgra(px,m,9,2,out));
     assert(out[0]==1&&out[1]==2&&out[2]==3&&out[3]==255);          // (0,0) opaque, colour kept
     assert(out[4]==0&&out[7]==0);                                   // (1,0) transparent
     assert(out[8*4]==33&&out[8*4+3]==255);                          // (8,0) from second mask byte
     assert(out[(9+7)*4+3]==255&&out[(9+7)*4]==65);                  // (7,1)
     assert(out[(9+8)*4+3]==0);                                      // (8,1) off
     assert(!lr_rfb_cursor_bgra(px,m,300,2,out));}
    puts("RFB cursor pseudo-encoding mask/premultiplied decode passed");
    puts("RFB bounded rectangle, guard bytes, malformed dimensions and randomized bounds passed");
}
