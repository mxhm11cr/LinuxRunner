// Native regression with real ANGLE/Metal and virgl contexts. Only the fence
// allocation failure is injected; subsequent real completion must still work.
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "graphics/virgl_backend.h"

extern const struct lr_virgl_backend *lr_virgl_host_start(void);
static atomic_bool fail_next;
int virgl_renderer_create_fence(int id, uint32_t context) {
    if (atomic_exchange(&fail_next, false)) return ENOMEM;
    int (*real_fence)(int, uint32_t) = dlsym(RTLD_NEXT, "virgl_renderer_create_fence");
    assert(real_fence && real_fence != virgl_renderer_create_fence);
    return real_fence(id, context);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const struct lr_virgl_backend *gpu = lr_virgl_host_start();
    assert(gpu && gpu->context_create(1, "fence-regression") == 0);
    // An empty, valid stream still exercises the embedder completion fence.
    assert(gpu->submit(1, NULL, 0) == 0);
    puts("VIRGL_FENCE_FAILURE injecting allocation error after real GPU completion");
    atomic_store(&fail_next, true);
    assert(gpu->submit(1, NULL, 0) == ENOMEM);
    assert(gpu->submit(1, NULL, 0) == 0);
    gpu->context_destroy(1);
    puts("VIRGL_FENCE_FAILURE PASS allocation-error propagation and subsequent GPU completion");
    return 0;
}
