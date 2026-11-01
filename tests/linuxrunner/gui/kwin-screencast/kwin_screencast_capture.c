// Capture KWin's own screencast of an output straight from PipeWire, without
// KRFB: asks KWin for a zkde_screencast stream of the first wl_output, offers
// only shared-memory video formats (no modifiers, so no DMA-BUF), and writes
// raw frames plus pixel statistics. Decides whether KWin's screencast frames
// already hold the window contents.
// Usage: kwin_screencast_capture OUTDIR [virtual]
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/debug/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "zkde-screencast-client-protocol.h"

static struct zkde_screencast_unstable_v1 *screencast;
static struct wl_output *output;
static uint32_t node;
static int failed;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    (void) d;
    if (!strcmp(iface, zkde_screencast_unstable_v1_interface.name))
        screencast = wl_registry_bind(r, name, &zkde_screencast_unstable_v1_interface, version < 3 ? version : 3);
    else if (!strcmp(iface, wl_output_interface.name) && !output)
        output = wl_registry_bind(r, name, &wl_output_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) { (void) d; (void) r; (void) name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void closed(void *d, struct zkde_screencast_stream_unstable_v1 *s) { (void) d; (void) s; printf("CAPTURE stream closed\n"); failed = 1; }
static void created(void *d, struct zkde_screencast_stream_unstable_v1 *s, uint32_t n) { (void) d; (void) s; node = n; printf("CAPTURE node=%u\n", n); }
static void failed_ev(void *d, struct zkde_screencast_stream_unstable_v1 *s, const char *e) { (void) d; (void) s; printf("CAPTURE failed: %s\n", e); failed = 1; }
static const struct zkde_screencast_stream_unstable_v1_listener stream_listener = {closed, created, failed_ev};

struct capture {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_video_info_raw info;
    const char *dir;
    int frames, dumps;
};

static void param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    struct capture *c = data;
    if (!param || id != SPA_PARAM_Format) return;
    spa_format_video_raw_parse(param, &c->info);
    printf("CAPTURE format=%s size=%ux%u\n", spa_debug_type_find_name(spa_type_video_format, c->info.format),
           c->info.size.width, c->info.size.height);
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    pw_stream_update_params(c->stream, params, 1);
}

static void process(void *data) {
    struct capture *c = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    c->frames++;
    if (d->data && d->chunk->size && (c->frames == 1 || c->frames % 20 == 0)) {
        const uint8_t *p = d->data;
        uint32_t stride = d->chunk->stride ? (uint32_t) d->chunk->stride : c->info.size.width * 4;
        unsigned maxv = 0; unsigned long long sum = 0, n = 0;
        for (uint32_t y = 0; y < c->info.size.height; y += 4)
            for (uint32_t x = 0; x < c->info.size.width; x += 4) {
                const uint8_t *px = p + (size_t) y * stride + x * 4;
                unsigned v = px[0] > px[1] ? px[0] : px[1]; v = v > px[2] ? v : px[2];
                if (v > maxv) maxv = v; sum += v; n++;
            }
        printf("CAPTURE frame=%d bytes=%u stride=%u type=%u max=%u mean=%.2f alpha0=%u\n", c->frames, d->chunk->size, stride,
               d->type, maxv, n ? (double) sum / n : 0, p[3]);
        char path[512];
        snprintf(path, sizeof path, "%s/kwin-frame-%d.raw", c->dir, c->frames);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(p, 1, d->chunk->size, f); fclose(f); }
        if (++c->dumps >= 4) pw_main_loop_quit(c->loop);
    }
    pw_stream_queue_buffer(c->stream, b);
}
static void on_wayland(void *data, int fd, uint32_t mask) { (void) fd; (void) mask; wl_display_dispatch(data); }
static const struct pw_stream_events events = {PW_VERSION_STREAM_EVENTS, .param_changed = param_changed, .process = process};

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { puts("CAPTURE no wayland display"); return 2; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!screencast || !output) { printf("CAPTURE missing screencast=%p output=%p\n", (void *) screencast, (void *) output); return 3; }
    const char *mode = argc > 2 ? argv[2] : "";
    struct zkde_screencast_stream_unstable_v1 *stream =
        !strcmp(mode, "virtual") ? zkde_screencast_unstable_v1_stream_virtual_output(screencast, "LinuxRunner-capture", 640, 400, wl_fixed_from_int(1), 4)
        : !strcmp(mode, "region") ? zkde_screencast_unstable_v1_stream_region(screencast, 0, 0, 1280, 800, wl_fixed_from_int(1), 4)
        : zkde_screencast_unstable_v1_stream_output(screencast, output, 4);
    zkde_screencast_stream_unstable_v1_add_listener(stream, &stream_listener, NULL);
    while (!node && !failed && wl_display_roundtrip(display) >= 0) {}
    if (!node) return 4;

    pw_init(&argc, &argv);
    struct capture c = {.dir = argv[1]};
    c.loop = pw_main_loop_new(NULL);
    c.stream = pw_stream_new_simple(pw_main_loop_get_loop(c.loop), "linuxrunner-capture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL),
        &events, &c);
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                                                        SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(1280, 800), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(30, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
    pw_stream_connect(c.stream, PW_DIRECTION_INPUT, node, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    // Keep the Wayland side responsive while PipeWire runs.
    struct spa_source *wl = pw_loop_add_io(pw_main_loop_get_loop(c.loop), wl_display_get_fd(display), SPA_IO_IN, false, on_wayland, display);
    (void) wl;
    wl_display_flush(display);
    pw_main_loop_run(c.loop);
    printf("CAPTURE_DONE frames=%d dumps=%d\n", c.frames, c.dumps);
    return c.dumps ? 0 : 5;
}
