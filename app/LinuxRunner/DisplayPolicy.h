// Shared, platform-independent geometry/lifecycle policy for external output.
#ifndef LR_DISPLAY_POLICY_H
#define LR_DISPLAY_POLICY_H
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
static inline bool lr_external_output(bool enabled, bool connected) { return enabled && connected; }
static inline bool lr_keep_desktop_transport(bool viewer_visible, bool enabled, bool connected) {
    return viewer_visible || lr_external_output(enabled, connected);
}
// Relative device-point motion, independent of the external display's aspect
// ratio. Keep fractional coordinates across events; clamp at framebuffer edges.
static inline bool lr_touchpad_move(double *x, double *y, double dx, double dy,
                                    uint16_t width, uint16_t height) {
    if (!width || !height || !isfinite(dx) || !isfinite(dy)) return false;
    *x=fmax(0,fmin(width-1,*x+dx*1.5));
    *y=fmax(0,fmin(height-1,*y+dy*1.5));
    return true;
}
static inline double lr_input_gain(double value, double fallback) {
    return isfinite(value) && value >= .25 && value <= 4 ? value : fallback;
}
// Bound interpreted software rendering, preserving aspect ratio. This is a
// guest resolution, not a claim of driving a monitor at its native pixel count.
static inline void lr_desktop_geometry(double w, double h, uint16_t *width, uint16_t *height) {
    if (!isfinite(w) || !isfinite(h) || w < 1 || h < 1) { *width=960; *height=540; return; }
    double scale=fmax(1,540/fmin(w,h)); w*=scale; h*=scale;
    double cap=fmin(1,fmin(1600/w,1200/h)); w*=cap; h*=cap;
    *width=fmax(320,(unsigned)w/8*8); *height=fmax(240,(unsigned)h/8*8);
}
#endif
