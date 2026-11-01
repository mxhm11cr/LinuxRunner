// Controlled guest test producer for KDE's versioned fake-input extension.
// This is compositor-side automation, not physical iPhone/trackpad validation.
#include <wayland-client.h>
#include "fake-input-client-protocol.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static struct org_kde_kwin_fake_input *input;
static void global(void *data,struct wl_registry *registry,uint32_t id,const char *name,uint32_t version){(void)data;if(!strcmp(name,"org_kde_kwin_fake_input")&&version>=4)input=wl_registry_bind(registry,id,&org_kde_kwin_fake_input_interface,4);}
static void removed(void *data,struct wl_registry *registry,uint32_t id){(void)data;(void)registry;(void)id;}
int main(void){struct wl_display *display=wl_display_connect(NULL);if(!display)return 1;struct wl_registry *registry=wl_display_get_registry(display);const struct wl_registry_listener listener={global,removed};wl_registry_add_listener(registry,&listener,NULL);if(wl_display_roundtrip(display)<0||!input)return 77;
 org_kde_kwin_fake_input_authenticate(input,"LinuxRunner controlled input test","Developer-requested disposable compositor validation");if(wl_display_roundtrip(display)<0)return 2;
 org_kde_kwin_fake_input_pointer_motion_absolute(input,wl_fixed_from_int(480),wl_fixed_from_int(270));if(wl_display_roundtrip(display)<0)return 3;
 usleep(100000);org_kde_kwin_fake_input_button(input,0x110,1);org_kde_kwin_fake_input_button(input,0x110,0);org_kde_kwin_fake_input_button(input,0x111,1);org_kde_kwin_fake_input_button(input,0x111,0);org_kde_kwin_fake_input_axis(input,0,wl_fixed_from_int(15));org_kde_kwin_fake_input_keyboard_key(input,30,1);org_kde_kwin_fake_input_keyboard_key(input,30,0);if(wl_display_roundtrip(display)<0)return 4;
 puts("KWIN_WAYLAND_INPUT_SENT pointer/buttons/axis/key; receiver log must verify delivery");wl_proxy_destroy((struct wl_proxy *)input);wl_registry_destroy(registry);wl_display_disconnect(display);return 0;}
