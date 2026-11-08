#ifndef LINUXRUNNER_VIRGL_CAPS_H
#define LINUXRUNNER_VIRGL_CAPS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Frozen virgl capset 1/2 wire prefix: max_version, four 16-word format
// masks, then bool_set1. timer_query is bit 12. Both capset versions share
// this prefix (virglrenderer src/virgl_hw.h: virgl_caps_v1/bool_set1).
// A single protocol bit advertises timestamp AND elapsed queries; do not
// advertise it when the host only supports elapsed queries.
static inline void lr_virgl_limit_timer_caps(uint32_t set, void *caps, size_t size, int supported) {
    const size_t offset = (1 + 4 * 16) * sizeof(uint32_t);
    if (supported || (set != 1 && set != 2) || size < offset + sizeof(uint32_t)) return;
    uint32_t bits;
    memcpy(&bits, (char *) caps + offset, sizeof bits);
    bits &= ~(1u << 12);
    memcpy((char *) caps + offset, &bits, sizeof bits);
}
#endif
