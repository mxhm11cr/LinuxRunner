// macOS test runtime with the virtio-gpu render node backed by virglrenderer
// on ANGLE/Metal (graphics/virgl_host.c). Same CLI as ish; guests opt in with
// ISH_VIRGL=1 and /dev/dri/renderD129 (226:129). Not an iPhone result.
#include <stdio.h>
#include "graphics/virgl_backend.h"
const struct lr_virgl_backend *lr_virgl_host_start(void);
int lr_cli_main(int argc, char *const argv[]);
int main(int argc, char *const argv[]) {
    const struct lr_virgl_backend *backend = lr_virgl_host_start();
    if (!backend) fprintf(stderr, "VIRGL_HOST: unavailable, virtio-gpu node disabled\n");
    lr_virgl_install(backend);
    return lr_cli_main(argc, argv);
}
