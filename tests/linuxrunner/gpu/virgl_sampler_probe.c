// Host-side desktop prerequisite probe for virglrenderer on ANGLE/Metal.
// Each state uses a fresh context: a rejected command poisons that context.
// This is not a guest or physical-device acceptance test. A nonzero result
// means these sampler semantics cannot be used by an accelerated desktop.
#include <stdio.h>
#include <errno.h>
#include "graphics/virgl_backend.h"

const struct lr_virgl_backend *lr_virgl_host_start(void);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const struct lr_virgl_backend *backend = lr_virgl_host_start();
    if (!backend) {
        puts("VIRGL_SAMPLER_PREREQUISITES UNAVAILABLE");
        return 77;
    }
    // virgl protocol: CREATE_OBJECT/SAMPLER_STATE, nine payload dwords.
    // Wrap enums are PIPE_TEX_WRAP_* from virglrenderer's pinned protocol.
    const struct { const char *name; uint32_t wrap, red; int expected; } cases[] = {
        {"repeat", 0, 0, 0}, {"edge", 2, 0, 0}, {"border", 3, 0, 0},
        {"mirror-edge", 6, 0, 0}, {"nonzero-border-rejected", 3, 0x3f800000, EINVAL},
    };
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t ctx = i + 1;
        int result = backend->context_create(ctx, cases[i].name);
        if (result) {
            printf("VIRGL_SAMPLER context=%s FAIL result=%d\n", cases[i].name, result);
            failures++;
            continue;
        }
        if (!result) {
            // Nearest min/mag filtering, no mipmaps, zero border color.
            uint32_t commands[] = {
                0x00090701, 1,
                cases[i].wrap | (cases[i].wrap << 3) | (2u << 6) | (2u << 11),
                0, 0, 0x447a0000, cases[i].red, 0, 0, 0,
            };
            result = backend->submit(ctx, commands, sizeof commands / sizeof commands[0]);
            backend->context_destroy(ctx);
        }
        printf("VIRGL_SAMPLER wrap=%s result=%d expected=%d %s\n", cases[i].name, result,
               cases[i].expected, result == cases[i].expected ? "PASS" : "FAIL");
        failures += result != cases[i].expected;
    }
    printf("VIRGL_SAMPLER_PREREQUISITES %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
