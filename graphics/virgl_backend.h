// Host renderer behind LinuxRunner's virtio-gpu render node (fs/drm.c).
// The guest's own Mesa virgl driver produces virgl command streams; the host
// side (graphics/virgl_host.c: virglrenderer on ANGLE's Metal backend)
// executes them on the GPU. Everything is synchronous: when a call returns,
// the GPU work it issued has completed, so the guest never needs fences.
#ifndef LINUXRUNNER_VIRGL_BACKEND_H
#define LINUXRUNNER_VIRGL_BACKEND_H
#include <stddef.h>
#include <stdint.h>

struct lr_virgl_resource_args {
    uint32_t handle, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
};
struct lr_virgl_box { uint32_t x, y, z, w, h, d; };

struct lr_virgl_backend {
    // Capability set: largest version and size, then its contents.
    int (*get_caps)(uint32_t set, uint32_t version, void *out, uint32_t size, uint32_t *written);
    int (*context_create)(uint32_t ctx_id, const char *name);
    void (*context_destroy)(uint32_t ctx_id);
    // `backing` is host memory shared with the guest's mapping of the resource.
    int (*resource_create)(const struct lr_virgl_resource_args *args, void *backing, size_t size);
    void (*resource_unref)(uint32_t handle);
    void (*context_attach)(uint32_t ctx_id, uint32_t handle);
    int (*submit)(uint32_t ctx_id, const void *commands, uint32_t dwords);
    int (*transfer)(uint32_t ctx_id, uint32_t handle, int to_host, uint32_t level, uint32_t stride,
                    uint32_t layer_stride, const struct lr_virgl_box *box, uint64_t offset);
    // Host GPU texture backing a resource (presentation); 0 if none.
    uint32_t (*resource_texture)(uint32_t handle, uint32_t *width, uint32_t *height);
};

// Installed once by the host driver before guests run; NULL keeps the virtio-gpu node absent.
void lr_virgl_install(const struct lr_virgl_backend *backend);
#endif
