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
// Portrait phone touchpad. The desktop keeps exactly its aspect-fit size; it
// moves to the top so the otherwise black space beneath it becomes a touchpad.
// No pad (desktop layout unchanged) in landscape, with the software keyboard
// up, while the phone is the external-display touchpad, or when too small.
typedef struct { double x, y, w, h; } lr_rect;
#define LR_PORTRAIT_PAD_MIN_HEIGHT 150.0
#define LR_PORTRAIT_PAD_MAX_HEIGHT 240.0  // owner feedback: full height was too big
#define LR_PORTRAIT_PAD_MARGIN 12.0
#define LR_PORTRAIT_PAD_SIDE_MARGIN 20.0
static inline bool lr_portrait_touchpad_layout(double bw, double bh, uint16_t pw, uint16_t ph,
        bool enabled, bool keyboard_visible, bool relative_mode, lr_rect *desktop, lr_rect *pad) {
    if (!enabled || keyboard_visible || relative_mode || !pw || !ph ||
            !isfinite(bw) || !isfinite(bh) || bw <= 0 || bh <= bw) return false;
    double fit = fmin(bw / pw, bh / ph), dw = pw * fit, dh = ph * fit;
    double pad_h = bh - dh - 2 * LR_PORTRAIT_PAD_MARGIN;
    if (pad_h < LR_PORTRAIT_PAD_MIN_HEIGHT || bw <= 2 * LR_PORTRAIT_PAD_SIDE_MARGIN) return false;
    if (pad_h > LR_PORTRAIT_PAD_MAX_HEIGHT) pad_h = LR_PORTRAIT_PAD_MAX_HEIGHT;
    // Anchored to the bottom (owner feedback): the space right under the desktop stays clear.
    *pad = (lr_rect){LR_PORTRAIT_PAD_SIDE_MARGIN, bh - LR_PORTRAIT_PAD_MARGIN - pad_h, bw - 2 * LR_PORTRAIT_PAD_SIDE_MARGIN, pad_h};
    // The desktop is centred in the space above the pad (owner feedback), same size.
    double above = pad->y - LR_PORTRAIT_PAD_MARGIN;
    *desktop = (lr_rect){(bw - dw) / 2, above > dh ? (above - dh) / 2 : 0, dw, dh};
    return true;
}
// OLED care: instructional touchpad text is shown on entry or on request,
// then faded out; the touchpad itself stays fully functional.
#define LR_TOUCHPAD_HINT_SECONDS 10.0
static inline bool lr_touchpad_hint_visible(double shown_at, double now) {
    return isfinite(shown_at) && isfinite(now) && shown_at > 0 && now >= shown_at &&
           now - shown_at < LR_TOUCHPAD_HINT_SECONDS;
}
// RFB counters restart when a viewer reconnects. Check that discontinuity
// before subtracting unsigned counters; updates/s is not application FPS.
static inline void lr_display_update_rate(double now, uint64_t frames,
                                          double *at, uint64_t *previous, double *rate) {
    if (!isfinite(now) || now <= 0) { *at=0; *previous=frames; *rate=0; return; }
    if (!isfinite(*at) || *at <= 0 || now < *at || frames < *previous) {
        *at=now; *previous=frames; *rate=0;
    } else if (now-*at >= .9) {
        *rate=(double)(frames-*previous)/(now-*at); *previous=frames; *at=now;
    }
}
// SteamOS on iPad replaces the old 16:9 default once. Later explicit choices
// (including Wide) survive; the existing iPhone and other distro defaults stay.
static inline unsigned lr_desktop_resolution(int saved, bool steamos_ipad, bool migrated) {
    if (saved < 0) return steamos_ipad ? 4 : 3;
    if (steamos_ipad && !migrated && saved == 3) return 4;
    return saved > 4 ? 4 : (unsigned)saved;
}
// The guest desktop is landscape, independently of the native window's
// orientation (including portrait monitors and narrow iPad windows). Rotation
// only changes the viewer's aspect-fit viewport, never the running guest mode.
// Bound rendering cost; these are logical guest pixels, not native monitor pixels.
static inline void lr_desktop_geometry(double w, double h, uint16_t *width, uint16_t *height) {
    if (!isfinite(w) || !isfinite(h) || w < 1 || h < 1) { *width=960; *height=540; return; }
    double short_side=fmin(w,h);
    double aspect=fmax(4.0/3.0,fmax(w,h)/short_side);
    // Calculate the capped short side first to avoid overflowing on large inputs.
    double logical_height=fmin(fmax(540,short_side),fmin(1200,1600/aspect));
    *width=fmax(320,(unsigned)(logical_height*aspect)/8*8);
    *height=fmax(240,(unsigned)logical_height/8*8);
}
#endif
