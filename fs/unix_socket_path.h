#ifndef ISH_UNIX_SOCKET_PATH_H
#define ISH_UNIX_SOCKET_PATH_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Only the backing filename changes; guest pathname/abstract names remain in
// the inode/name tables. Base 36 needs at most 13 bytes for the full ID pair
// and uses one case, including on case-insensitive host filesystems.
// Returns the pathname byte length, or zero without writing if it cannot fit.
static inline size_t unix_socket_host_path(const char *prefix, uint32_t process,
                                         uint32_t socket_id, char *out, size_t capacity) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    uint64_t value = ((uint64_t)process << 32) | socket_id;
    char reversed[13]; size_t count = 0;
    do { reversed[count++] = digits[value % 36]; value /= 36; } while (value);
    size_t length = strlen(prefix);
    if (length >= capacity || count >= capacity - length) return 0;
    memcpy(out, prefix, length);
    for (size_t i = 0; i < count; i++) out[length + i] = reversed[count - i - 1];
    out[length + count] = 0;
    return length + count;
}
#endif
