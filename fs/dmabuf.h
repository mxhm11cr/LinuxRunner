#ifndef LR_SOFTWARE_DMABUF_H
#define LR_SOFTWARE_DMABUF_H
#include "fs/dev.h"
// A CPU-coherent, linear memfd export, not a physical DMA/GPU device.
#define LR_UDMABUF_MAJOR 10
#define LR_UDMABUF_MINOR 254
extern struct dev_ops software_udmabuf_dev;
bool fd_is_software_dmabuf(const struct fd *fd);
// The memfd range behind a software dmabuf (borrowed reference).
bool software_dmabuf_source(const struct fd *fd,struct fd **source,uint64_t *offset,uint64_t *size);
// Export a page-aligned range of a shrink-sealed, writable memfd as a new
// descriptor; consumes the caller's reference to source. flags&1: close-on-exec.
int software_dmabuf_export(struct fd *source,uint64_t offset,uint64_t size,unsigned flags);
#endif
