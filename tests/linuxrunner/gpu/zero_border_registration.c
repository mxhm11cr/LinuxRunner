// Compile four variants on macOS: neither symbol, either symbol alone, both.
// The optional bridge must never call a missing function.
#include <assert.h>
#include <stdbool.h>
#include "graphics/virgl_zero_border.h"

static int calls;
#if defined(TEST_ANGLE)
unsigned char LinuxRunnerANGLESetSamplerZeroBorder(unsigned int sampler, unsigned int pname) {
    return sampler == 7 && pname == 9;
}
#endif
#if defined(TEST_VIRGL)
void lr_vrend_set_zero_border_sampler(lr_zero_border_operation operation) {
    assert(operation && operation(7, 9));
    calls++;
}
#endif
int main(void) {
    bool registered = lr_register_zero_border_bridge();
#if defined(TEST_ANGLE) && defined(TEST_VIRGL)
    assert(registered && calls == 1);
#else
    assert(!registered && calls == 0);
#endif
    return 0;
}
