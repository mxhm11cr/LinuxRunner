#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// Damage rectangles accumulate across skipped deliveries and texture slots.
typedef struct { uint16_t x,y,width,height; } LRFrameDamage;
static inline LRFrameDamage lr_damage_union(LRFrameDamage a,LRFrameDamage b) {
 if(!a.width||!a.height)return b;if(!b.width||!b.height)return a;
 unsigned x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
 unsigned r=a.x+a.width>b.x+b.width?a.x+a.width:b.x+b.width;
 unsigned d=a.y+a.height>b.y+b.height?a.y+a.height:b.y+b.height;
 return (LRFrameDamage){x,y,r-x,d-y};
}
#define LR_RFB_MAX_WIDTH 2560   // session.sh accepts up to 2560x1600 ("Fit Screen", external displays)
#define LR_RFB_MAX_HEIGHT 1600
bool lr_rfb_dimensions(uint16_t width,uint16_t height,size_t *bytes);
bool lr_rfb_rectangle(uint16_t fw,uint16_t fh,uint16_t x,uint16_t y,uint16_t w,uint16_t h,size_t *bytes);
bool lr_rfb_apply_raw(uint8_t *frame,uint16_t fw,uint16_t fh,uint16_t x,uint16_t y,uint16_t w,uint16_t h,const uint8_t *source,size_t count);
uint8_t lr_rfb_reverse_bits(uint8_t value);
bool lr_rfb_fill(uint8_t *frame,uint16_t fw,uint16_t fh,uint16_t x,uint16_t y,uint16_t w,uint16_t h,const uint8_t color[4]);

// Invert the actual rendered viewport after fit, zoom, pan or rotation.
bool lr_rfb_viewport_point(double x,double y,double left,double top,double rendered_width,double rendered_height,uint16_t width,uint16_t height,uint16_t *px,uint16_t *py);

// RFB Cursor pseudo-encoding (-239) with the session's 32-bit B,G,R,pad pixel
// format: w*h*4 pixel bytes, then a 1-bit mask of ((w+7)/8)*h bytes (MSB is
// the leftmost pixel). The server stops drawing the cursor into the
// framebuffer for this client, which draws it locally at its own pointer.
#define LR_RFB_CURSOR_MAX 256
bool lr_rfb_cursor_sizes(uint16_t w,uint16_t h,size_t *pixel_bytes,size_t *mask_bytes);
// Premultiplied B,G,R,A (kCGImageAlphaPremultipliedFirst, little endian).
bool lr_rfb_cursor_bgra(const uint8_t *pixels,const uint8_t *mask,uint16_t w,uint16_t h,uint8_t *out);
