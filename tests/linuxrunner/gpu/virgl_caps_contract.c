// Host-only wire-capability regression; this is not a rendering test.
#include <assert.h>
#include <stdio.h>
#include "graphics/virgl_caps.h"
int main(void) {
    uint32_t words[128], expected[128];
    for (unsigned set = 1; set <= 2; set++) {
        memset(words, 0xff, sizeof words); memcpy(expected, words, sizeof words);
        expected[65] &= ~(1u << 12);
        lr_virgl_limit_timer_caps(set, words, sizeof words, 0);
        assert(!memcmp(words, expected, sizeof words));
        memset(words, 0xff, sizeof words); memset(expected, 0xff, sizeof expected);
        lr_virgl_limit_timer_caps(set, words, sizeof words, 1);
        assert(!memcmp(words, expected, sizeof words));
        lr_virgl_limit_timer_caps(set, words, 263, 0);
        assert(!memcmp(words, expected, sizeof words));
    }
    lr_virgl_limit_timer_caps(3, words, sizeof words, 0);
    assert(!memcmp(words, expected, sizeof words));
    puts("VIRGL_CAPS_CONTRACT PASS capsets=1,2 timestamp+elapsed requirement, bounds, unrelated bits preserved");
}
