#ifndef LR_KMS_DISPLAY_H
#define LR_KMS_DISPLAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// Host-only owned snapshot. take transfers pixels to the caller (free()).
struct lr_kms_frame { void *pixels; uint32_t width, height; uint64_t sequence; };
// Fixed-size guest ABI. type: 1 = absolute pointer/RFB button mask, 2 = keysym.
struct lr_kms_input { uint32_t type, x, y, buttons, keysym, down, width, height; };
#define LR_KMS_INPUT_IOCTL 0x802064f0U
uint64_t lr_kms_display_begin(void);
void lr_kms_display_end(uint64_t token);
bool lr_kms_display_take(uint64_t token, struct lr_kms_frame *frame);
bool lr_kms_display_input(uint64_t token, struct lr_kms_input event);
#endif
