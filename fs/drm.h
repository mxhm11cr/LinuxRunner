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
#endif
