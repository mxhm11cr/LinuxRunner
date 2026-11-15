// Integration test: a separately built, patched virgl library and a stubbed
// ANGLE operation must be discoverable and register without rebuilding ANGLE.
#include <assert.h>
#include <dlfcn.h>
#include "graphics/virgl_zero_border.h"

unsigned char LinuxRunnerANGLESetSamplerZeroBorder(unsigned int sampler, unsigned int pname) {
    return sampler == 7 && pname == 9;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    assert(library);
    assert(lr_register_zero_border_bridge());
    dlclose(library);
    return 0;
}
