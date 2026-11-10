#ifndef PLATFORM_GUEST_FILE_CAPS_H
#define PLATFORM_GUEST_FILE_CAPS_H
// Private, inode-backed guest metadata. It never grants a host capability.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/xattr.h>
#ifdef __APPLE__
#define LR_GUEST_CAPS_XATTR "com.linuxrunner.guest.capability"
#else
#define LR_GUEST_CAPS_XATTR "user.linuxrunner.capability"
#endif
#define LR_GUEST_CAPS_MAX 24
// 0 = valid, -1 = malformed, -2 = namespaced rootid not implemented.
static inline int lr_guest_caps_validate(const void *value, size_t size) {
    uint32_t w[6] = {0};
    if (size < 4 || size > sizeof w) return -1;
    memcpy(w, value, size); unsigned rev = w[0] >> 24;
    if ((rev == 1 && size != 12) || (rev == 2 && size != 20) ||
            (rev == 3 && size != 24) || rev < 1 || rev > 3 || (w[0] & 0xfffffe)) return -1;
    if (rev == 3 && w[5] != 0) return -2;
    uint64_t permitted = w[1] | (uint64_t)(rev >= 2 ? w[3] : 0) << 32;
    uint64_t inherited = w[2] | (uint64_t)(rev >= 2 ? w[4] : 0) << 32;
    return (permitted | inherited) & ~((1ull << 41) - 1) ? -1 : 0;
}
static inline ssize_t lr_guest_caps_get(int fd, void *value, size_t size) {
#ifdef __APPLE__
    return fgetxattr(fd, LR_GUEST_CAPS_XATTR, value, size, 0, 0);
#else
    return fgetxattr(fd, LR_GUEST_CAPS_XATTR, value, size);
#endif
}
static inline int lr_guest_caps_set(int fd, const void *value, size_t size, int flags) {
#ifdef __APPLE__
    return fsetxattr(fd, LR_GUEST_CAPS_XATTR, value, size, 0, flags);
#else
    return fsetxattr(fd, LR_GUEST_CAPS_XATTR, value, size, flags);
#endif
}
static inline int lr_guest_caps_remove(int fd) {
#ifdef __APPLE__
    return fremovexattr(fd, LR_GUEST_CAPS_XATTR, 0);
#else
    return fremovexattr(fd, LR_GUEST_CAPS_XATTR);
#endif
}
#endif
