// A real xdg-shell/wl_shm client: FD passing, shared mmap, frame callbacks and
// compositor buffer release. Build with generated stable xdg-shell protocol.
#define _GNU_SOURCE
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
struct state {struct wl_display *display;struct wl_compositor *compositor;struct wl_shm *shm;struct xdg_wm_base *wm;struct wl_surface *surface;struct wl_seat *seat;struct wl_pointer *pointer;struct wl_keyboard *keyboard;struct xkb_context *context;struct xkb_state *key_state;int extended,configured,closed,frames,releases,buttons,keys;};
static void ping(void *p,struct xdg_wm_base *wm,uint32_t serial){(void)p;xdg_wm_base_pong(wm,serial);}
static const struct xdg_wm_base_listener wm_listener={.ping=ping};
static void pointer_enter(void *p,struct wl_pointer *v,uint32_t n,struct wl_surface *s,wl_fixed_t x,wl_fixed_t y){(void)p;(void)v;(void)n;(void)s;(void)x;(void)y;}
static void pointer_leave(void *p,struct wl_pointer *v,uint32_t n,struct wl_surface *s){(void)p;(void)v;(void)n;(void)s;}
static void motion(void *p,struct wl_pointer *v,uint32_t n,wl_fixed_t x,wl_fixed_t y){
 (void)v;(void)n;struct state *s=p;
 if(s->extended){printf("WAYLAND_MOTION x=%d y=%d\n",wl_fixed_to_int(x),wl_fixed_to_int(y));fflush(stdout);}
}
static void button(void *p,struct wl_pointer *v,uint32_t n,uint32_t t,uint32_t b,uint32_t state){(void)v;(void)n;(void)t;struct state *s=p;if(s->extended){printf("WAYLAND_BUTTON code=%u down=%u\n",b,state);fflush(stdout);}if(state){s->buttons++;printf("WAYLAND_INPUT button=%u\n",b);fflush(stdout);}}
static void axis(void *p,struct wl_pointer *v,uint32_t t,uint32_t a,wl_fixed_t value){
 (void)v;(void)t;struct state *s=p;if(s->extended){printf("WAYLAND_AXIS axis=%u value=%d\n",a,wl_fixed_to_int(value));fflush(stdout);}
}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=motion,.button=button,.axis=axis};
static void keymap(void *p,struct wl_keyboard *v,uint32_t format,int32_t fd,uint32_t size){
 (void)v;struct state *s=p;
 if(format==WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1&&size){
  char *text=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);
  if(text!=MAP_FAILED){
   struct xkb_keymap *map=xkb_keymap_new_from_string(s->context,text,XKB_KEYMAP_FORMAT_TEXT_V1,0);
   if(map){if(s->key_state)xkb_state_unref(s->key_state);s->key_state=xkb_state_new(map);xkb_keymap_unref(map);}
   munmap(text,size);
  }
 }
 close(fd);
}
static void key_enter(void *p,struct wl_keyboard *v,uint32_t n,struct wl_surface *s,struct wl_array *keys){(void)p;(void)v;(void)n;(void)s;(void)keys;}
static void key_leave(void *p,struct wl_keyboard *v,uint32_t n,struct wl_surface *s){(void)p;(void)v;(void)n;(void)s;}
static void key(void *p,struct wl_keyboard *v,uint32_t n,uint32_t t,uint32_t code,uint32_t state){(void)v;(void)n;(void)t;struct state *s=p;
 if(s->extended&&s->key_state){printf("WAYLAND_KEY code=%u sym=%u utf32=%u down=%u\n",code,xkb_state_key_get_one_sym(s->key_state,code+8),xkb_state_key_get_utf32(s->key_state,code+8),state);fflush(stdout);}
 if(state){s->keys++;printf("WAYLAND_INPUT key=%u\n",code);fflush(stdout);}}
static void modifiers(void *p,struct wl_keyboard *v,uint32_t n,uint32_t a,uint32_t b,uint32_t c,uint32_t g){(void)v;(void)n;struct state *s=p;if(s->key_state)xkb_state_update_mask(s->key_state,a,b,c,0,0,g);if(s->extended){printf("WAYLAND_MODIFIERS depressed=%u latched=%u locked=%u\n",a,b,c);fflush(stdout);}}
static const struct wl_keyboard_listener keyboard_listener={.keymap=keymap,.enter=key_enter,.leave=key_leave,.key=key,.modifiers=modifiers};
static void capabilities(void *data,struct wl_seat *seat,uint32_t caps){struct state *s=data;
 if((caps&WL_SEAT_CAPABILITY_POINTER)&&!s->pointer){s->pointer=wl_seat_get_pointer(seat);wl_pointer_add_listener(s->pointer,&pointer_listener,s);}
 if((caps&WL_SEAT_CAPABILITY_KEYBOARD)&&!s->keyboard){s->keyboard=wl_seat_get_keyboard(seat);wl_keyboard_add_listener(s->keyboard,&keyboard_listener,s);}
}
static const struct wl_seat_listener seat_listener={.capabilities=capabilities};
static void global(void *data,struct wl_registry *registry,uint32_t name,const char *interface,uint32_t version){
 struct state *s=data;
 if(!strcmp(interface,"wl_compositor"))s->compositor=wl_registry_bind(registry,name,&wl_compositor_interface,version<3?version:3);
 else if(!strcmp(interface,"wl_shm"))s->shm=wl_registry_bind(registry,name,&wl_shm_interface,1);
 else if(!strcmp(interface,"wl_seat")){s->seat=wl_registry_bind(registry,name,&wl_seat_interface,1);wl_seat_add_listener(s->seat,&seat_listener,s);}
 else if(!strcmp(interface,"xdg_wm_base")){s->wm=wl_registry_bind(registry,name,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(s->wm,&wm_listener,s);}
}
static void removed(void *p,struct wl_registry *r,uint32_t name){(void)p;(void)r;(void)name;}
static const struct wl_registry_listener registry_listener={global,removed};
static void configure(void *data,struct xdg_surface *surface,uint32_t serial){struct state *s=data;xdg_surface_ack_configure(surface,serial);s->configured=1;}
static const struct xdg_surface_listener surface_listener={configure};
static void top_configure(void *p,struct xdg_toplevel *t,int32_t w,int32_t h,struct wl_array *states){(void)p;(void)t;(void)w;(void)h;(void)states;}
static void close_window(void *p,struct xdg_toplevel *t){(void)t;((struct state*)p)->closed=1;}
static const struct xdg_toplevel_listener top_listener={.configure=top_configure,.close=close_window};
static void release(void *data,struct wl_buffer *buffer){((struct state*)data)->releases++;wl_buffer_destroy(buffer);}
static const struct wl_buffer_listener buffer_listener={release};
static void frame(void *data,struct wl_callback *callback,uint32_t time){(void)time;((struct state*)data)->frames++;wl_callback_destroy(callback);}
static const struct wl_callback_listener frame_listener={frame};
static int draw(struct state *s,unsigned index){
 const int w=320,h=200,stride=w*4;char file[]="/tmp/linuxrunner-wayland-XXXXXX";int fd=mkstemp(file);if(fd<0)return -1;unlink(file);
 if(ftruncate(fd,stride*h)<0){close(fd);return -1;}
 uint32_t *pixels=mmap(NULL,stride*h,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(pixels==MAP_FAILED){close(fd);return -1;}
 for(int y=0;y<h;y++)for(int x=0;x<w;x++)pixels[y*w+x]=0xff000000u|((x+index*50)%256<<16)|((y+index*70)%256<<8)|0x55;
 struct wl_shm_pool *pool=wl_shm_create_pool(s->shm,fd,stride*h);struct wl_buffer *buffer=wl_shm_pool_create_buffer(pool,0,w,h,stride,WL_SHM_FORMAT_XRGB8888);
 wl_shm_pool_destroy(pool);wl_buffer_add_listener(buffer,&buffer_listener,s);munmap(pixels,stride*h);close(fd);
 struct wl_callback *callback=wl_surface_frame(s->surface);wl_callback_add_listener(callback,&frame_listener,s);
 wl_surface_attach(s->surface,buffer,0,0);wl_surface_damage(s->surface,0,0,w,h);wl_surface_commit(s->surface);return 0;
}
int main(void){
 struct state s={0};s.extended=getenv("LR_WAYLAND_EXTENDED_INPUT")!=NULL;s.context=xkb_context_new(0);if(!s.context)return 7;s.display=wl_display_connect(NULL);if(!s.display){perror("WAYLAND_CONNECT_FAIL");return 1;}
 struct wl_registry *registry=wl_display_get_registry(s.display);wl_registry_add_listener(registry,&registry_listener,&s);
 if(wl_display_roundtrip(s.display)<0||!s.compositor||!s.shm||!s.wm){fputs("WAYLAND_PROTOCOLS_MISSING\n",stderr);return 2;}
 s.surface=wl_compositor_create_surface(s.compositor);struct xdg_surface *xdg=xdg_wm_base_get_xdg_surface(s.wm,s.surface);xdg_surface_add_listener(xdg,&surface_listener,&s);
 struct xdg_toplevel *top=xdg_surface_get_toplevel(xdg);xdg_toplevel_add_listener(top,&top_listener,&s);xdg_toplevel_set_title(top,"LinuxRunner Wayland Shared Memory Probe");wl_surface_commit(s.surface);
 while(!s.configured&&!s.closed)if(wl_display_dispatch(s.display)<0)return 3;
 for(int n=0;n<3&&!s.closed;n++){if(draw(&s,n))return 4;while(s.frames<=n&&!s.closed)if(wl_display_dispatch(s.display)<0)return 5;}
 wl_display_roundtrip(s.display);
 puts("WAYLAND_INPUT_READY");fflush(stdout);
 const char *hold=getenv("LR_WAYLAND_INPUT_SECONDS");int seconds=hold?atoi(hold):0;if(seconds>300)seconds=300;
 struct timespec started,now;clock_gettime(CLOCK_MONOTONIC,&started);
 while(seconds>0&&!s.closed){
  clock_gettime(CLOCK_MONOTONIC,&now);if(now.tv_sec-started.tv_sec>=seconds)break;
  if(!s.extended&&s.keys>0&&s.buttons>0)break;
  wl_display_dispatch_pending(s.display);wl_display_flush(s.display);struct pollfd p={wl_display_get_fd(s.display),POLLIN,0};
  if(poll(&p,1,100)>0&&wl_display_dispatch(s.display)<0)return 5;
 }
 int input_ok=!getenv("LR_WAYLAND_REQUIRE_INPUT")||(s.keys>0&&s.buttons>0);
 printf("WAYLAND_INPUT_SUMMARY keys=%d buttons=%d required=%d\n",s.keys,s.buttons,getenv("LR_WAYLAND_REQUIRE_INPUT")!=NULL);
 int pass=input_ok&&s.frames==3&&s.releases>=1;
 printf("WAYLAND_SHM_%s frames=%d releases=%d actual compositor, xdg-shell, shared mmap and Unix FD transfer; see input summary\n",pass?"PASS":"FAIL",s.frames,s.releases);
 if(s.keyboard)wl_keyboard_destroy(s.keyboard);
 if(s.pointer)wl_pointer_destroy(s.pointer);
 if(s.seat)wl_seat_destroy(s.seat);
 xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(s.surface);xdg_wm_base_destroy(s.wm);wl_shm_destroy(s.shm);wl_compositor_destroy(s.compositor);wl_registry_destroy(registry);wl_display_disconnect(s.display);if(s.key_state)xkb_state_unref(s.key_state);xkb_context_unref(s.context);return pass?0:6;
}
