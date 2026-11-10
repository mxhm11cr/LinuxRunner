#ifndef KERNEL_FILECAPS_H
#define KERNEL_FILECAPS_H
#include "misc.h"
struct fd;
struct file_caps { uint64_t permitted, inheritable; bool effective, present; };
int file_caps_decode(const void *value, size_t size, struct file_caps *caps);
int file_caps_read(struct fd *fd, struct file_caps *caps);
// Private host xattr, never a host security.capability / host privilege grant.
ssize_t file_caps_host_get(int fd, void *value, size_t size);
int file_caps_host_set(int fd, const void *value, size_t size, int flags);
int file_caps_host_remove(int fd);
int file_caps_strip(struct fd *fd);
#endif
