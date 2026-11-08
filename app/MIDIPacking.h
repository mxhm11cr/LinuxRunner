// Serial-over-MIDI 8-to-7 packing (docs/usb-midi-serial/PROTOCOL.md): for each
// group of up to 7 bytes, one byte holding their top bits (bit k = byte k),
// then the 7 bytes with their top bit cleared. Shared with the adapter firmware.
#pragma once
#include <stddef.h>
#include <stdint.h>
static inline size_t lr_midi_pack(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 7) {
        size_t g = n - i < 7 ? n - i : 7; uint8_t high = 0;
        for (size_t k = 0; k < g; k++) if (in[i + k] & 0x80) high |= (uint8_t)(1 << k);
        out[o++] = high;
        for (size_t k = 0; k < g; k++) out[o++] = in[i + k] & 0x7F;
    }
    return o;
}
static inline size_t lr_midi_unpack(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        uint8_t high = in[i++];
        for (size_t k = 0; k < 7 && i < n; k++, i++) out[o++] = (uint8_t)(in[i] | (((high >> k) & 1) << 7));
    }
    return o;
}

