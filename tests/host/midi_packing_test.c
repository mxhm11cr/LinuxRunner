// Serial-over-MIDI packing: SysEx data bytes must be 7-bit, and unpack(pack(x)) == x.
#include "app/MIDIPacking.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    uint8_t in[300], packed[400], out[300];
    srand(902);
    for (int round = 0; round < 20000; round++) {
        size_t n = (size_t) rand() % 49;     // a frame carries at most 48 bytes
        for (size_t i = 0; i < n; i++) in[i] = (uint8_t) rand();
        size_t p = lr_midi_pack(in, n, packed);
        assert(p == n + (n + 6) / 7);
        for (size_t i = 0; i < p; i++) assert(packed[i] < 0x80);
        size_t u = lr_midi_unpack(packed, p, out);
        assert(u == n && !memcmp(in, out, n));
    }
    const uint8_t edge[7] = {0xFF, 0x80, 0x7F, 0x00, 0xF0, 0xF7, 0x01};
    size_t p = lr_midi_pack(edge, 7, packed);
    assert(p == 8 && packed[0] == 0x33);   // high bits of FF,80,F0,F7 -> bits 0,1,4,5
    puts("MIDI_PACKING_PASS 20000 random frames, 7-bit clean, round-trip exact");
    return 0;
}
