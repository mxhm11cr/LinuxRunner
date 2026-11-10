// Submit padded scanout, then reuse its storage before the host reads it.
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm.h>
#include <drm_mode.h>
int main(int argc, char **argv) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    uint32_t crtc = 0, connector = 0;
    struct drm_mode_card_res resources = {0};
    assert(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) == 0);
    resources.crtc_id_ptr = (uintptr_t)&crtc;
    resources.connector_id_ptr = (uintptr_t)&connector;
    resources.count_crtcs = resources.count_connectors = 1;
    assert(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) == 0);
    struct drm_mode_get_connector output = {.connector_id = connector};
    assert(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &output) == 0);
    struct drm_mode_modeinfo modes[16];
    assert(output.count_modes > 0 && output.count_modes <= 16);
    output.modes_ptr = (uintptr_t)modes;
    output.count_props = output.count_encoders = 0;
    assert(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &output) == 0);
    unsigned width = modes[0].hdisplay, height = modes[0].vdisplay;
    struct drm_mode_create_dumb buffer = {.width = width + 17, .height = height + 3, .bpp = 32};
    assert(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer) == 0);
    struct drm_mode_map_dumb mapping = {.handle = buffer.handle};
    assert(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mapping) == 0);
    unsigned char *pixels = mmap(NULL, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mapping.offset);
    assert(pixels != MAP_FAILED);
    memset(pixels, 0xaa, buffer.size);
    uint32_t offset = buffer.pitch * 2 + 12;
    for (unsigned y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)(pixels + offset + (size_t)y * buffer.pitch);
        for (unsigned x = 0; x < width; x++) row[x] = 0x40000055U | ((x & 255) << 16) | ((y & 255) << 8);
    }
    uint32_t format = argc > 1 && !strcmp(argv[1], "argb") ? 0x34325241 : 0x34325258;
    struct drm_mode_fb_cmd2 framebuffer = {.width = width, .height = height, .pixel_format = format,
        .handles = {buffer.handle}, .pitches = {buffer.pitch}, .offsets = {offset}};
    assert(ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &framebuffer) == 0);
    struct drm_mode_crtc mode = {.set_connectors_ptr = (uintptr_t)&connector, .count_connectors = 1,
        .crtc_id = crtc, .fb_id = framebuffer.fb_id, .mode_valid = 1, .mode = modes[0]};
    assert(ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &mode) == 0);
    memset(pixels, 0, buffer.size);
    const char *ready = getenv("LR_KMS_SNAPSHOT_READY");
    if (ready) {
        FILE *marker = fopen(ready, "w");
        assert(marker);
        fputs("READY", marker);
        assert(fclose(marker) == 0);
    }
    usleep(300000);
    printf("KMS_SNAPSHOT_SUBMITTED width=%u height=%u pitch=%u offset=%u format=%08x storage_reused=1\n",
        width, height, buffer.pitch, offset, format);
    munmap(pixels, buffer.size);
    close(fd);
    return 0;
}
