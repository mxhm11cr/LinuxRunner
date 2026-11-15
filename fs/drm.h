#ifndef LR_SOFTWARE_DRM_H
#define LR_SOFTWARE_DRM_H
#include "fs/dev.h"
// Software KMS device for Mesa's kms_swrast (CPU rendering), not a GPU.
#define LR_DRM_MAJOR 226
#define LR_DRM_CARD_MINOR 0
#define LR_DRM_RENDER_MINOR 128
#define LR_VIRGL_CARD_MINOR 1       // virtio-gpu with the KMS output (host virglrenderer), ISH_VIRGL=1
#define LR_VIRGL_RENDER_MINOR 129   // virtio-gpu render node
extern struct dev_ops software_drm_dev;
// Preferred KMS mode = the desktop session size (set by the app before each desktop starts).
void drm_set_display_size(unsigned width, unsigned height);
// Direct presentation of a GPU-rendered desktop to the in-process viewer: a
// copy of the displayed framebuffer (BGRA, rows of width * 4 bytes, alpha
// opaque) when it changed after `serial`. The damage rectangle is the union of
// GPU updates since that serial (the whole frame after a resize or mode set).
// Returns 1 with a new frame, 0 when unchanged, or a negative errno: no
// GPU-backed scanout (use the RFB stream), or dst too small.
struct lr_scanout_frame { uint32_t width, height, damage_x, damage_y, damage_width, damage_height; uint64_t serial; };
int lr_drm_scanout_copy(void *dst, size_t capacity, uint64_t serial, struct lr_scanout_frame *frame);
#endif
