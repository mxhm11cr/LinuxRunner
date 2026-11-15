// Optional private bridge between compatible virglrenderer and ANGLE builds.
// Missing symbols are normal for older GPU bundles; never pretend the full
// border-color extension is supported or change their rendering behavior.
#pragma once
#include <stdbool.h>
#include <dlfcn.h>

typedef unsigned char (*lr_zero_border_operation)(unsigned int sampler, unsigned int pname);

static inline bool lr_register_zero_border_bridge(void) {
    // Both symbols must already be in the process image. Resolve them without
    // pulling in or changing an existing GPU archive; partial bundles stay off.
    lr_zero_border_operation operation = (lr_zero_border_operation)dlsym(RTLD_DEFAULT, "LinuxRunnerANGLESetSamplerZeroBorder");
    void (*set_operation)(lr_zero_border_operation) = (void (*)(lr_zero_border_operation))dlsym(RTLD_DEFAULT, "lr_vrend_set_zero_border_sampler");
    if (!operation || !set_operation)
        return false;
    set_operation(operation);
    return true;
}
