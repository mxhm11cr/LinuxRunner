#define _GNU_SOURCE
// Exercise native Wayland input against real xkbcommon maps, recording protocol
// calls rather than requiring a compositor. See docs/engine/DIRECT_KMS.md.
#include <assert.h>
#include <stdarg.h>
#define main bridge_main
#include "app/LinuxRunner/GUISupport.bundle/lr-kms-bridge.c"
#undef main
static struct native_input events[] = {
    {.type=2,.keysym='A',.down=1}, {.type=2,.keysym='A'},
    {.type=2,.keysym=0xffe3,.down=1}, // Control_L held across lowercase shortcut
    {.type=2,.keysym='c',.down=1}, {.type=2,.keysym='c'},
    {.type=2,.keysym=0xffe3},
    {.type=2,.keysym=0x0101f600,.down=1}, {.type=2,.keysym=0x0101f600},
    {.type=1,.x=100,.y=50,.width=960,.height=540,.buttons=1},
    {.type=1,.x=110,.y=60,.width=960,.height=540,.buttons=1},
    {.type=1,.width=960,.height=540,.buttons=8}, // release and wheel up
    {.type=2,.keysym='b',.down=1}, {.type=3}, // detach releases held input
};
static unsigned next_event, key_down, key_up, button_down, button_up, moves, wheels, maps;
static struct wl_proxy *vp=(void *)1, *vk=(void *)2, *text_vk=(void *)3;
static struct xkb_context *context;
int __wrap_ioctl(int fd, unsigned long command, ...) {
    (void)fd; assert(command == NATIVE_INPUT_IOCTL);
    if (next_event == sizeof events / sizeof *events) { errno=EAGAIN; return -1; }
    va_list ap; va_start(ap,command); struct native_input *e=va_arg(ap,struct native_input *); va_end(ap);
    *e=events[next_event++]; return 0;
}
struct wl_proxy *__wrap_wl_proxy_marshal_constructor(struct wl_proxy *p, uint32_t op, const struct wl_interface *iface, ...) {
    (void)p;(void)op;(void)iface;return text_vk;
}
int __wrap_wl_display_dispatch_pending(struct wl_display *wl) { (void)wl;return -1; }
void __wrap_wl_proxy_marshal(struct wl_proxy *p, uint32_t op, ...) {
    va_list ap; va_start(ap,op);
    if (p == vp) {
        if(op == VP_BUTTON) { va_arg(ap,uint32_t); va_arg(ap,uint32_t); if(va_arg(ap,uint32_t))button_down++;else button_up++; }
        if(op == VP_MOTION_ABSOLUTE) moves++;
        if(op == VP_AXIS_DISCRETE) wheels++;
    } else if (p==vk || p==text_vk) {
        if(op==VK_KEY) { va_arg(ap,uint32_t); va_arg(ap,uint32_t); if(va_arg(ap,uint32_t))key_down++;else key_up++; }
        if(op==VK_KEYMAP) {
            assert(va_arg(ap,uint32_t)==1);int fd=va_arg(ap,int);uint32_t size=va_arg(ap,uint32_t);
            char *s=malloc(size); assert(pread(fd,s,size,0)==size);
            struct xkb_keymap *map=xkb_keymap_new_from_string(context,s,XKB_KEYMAP_FORMAT_TEXT_V1,0);
            assert(map); const xkb_keysym_t *symbols;
            assert(xkb_keymap_key_get_syms_by_level(map,8,0,0,&symbols)==1 && symbols[0]==0x0101f600);
            xkb_keymap_unref(map);free(s);maps++;
        }
    }
    va_end(ap);
}
int main(void) {
    context=xkb_context_new(0); assert(context);
    struct xkb_keymap *map=xkb_keymap_new_from_names(context,NULL,0); assert(map);
    struct xkb_state *state=xkb_state_new(map);assert(state);
    native_loop(7,(void *)4,vp,vk,state);
    assert(next_event==sizeof events/sizeof *events);
    assert(key_down==key_up && key_down==6); // shift, A, control, c, unicode, b
    assert(button_down==1 && button_up==1 && moves==3 && wheels==1 && maps==1);
    assert(!xkb_state_serialize_mods(state,XKB_STATE_MODS_DEPRESSED));
    xkb_state_unref(state);xkb_keymap_unref(map);xkb_context_unref(context);
    puts("Native KMS input PASS");
}
