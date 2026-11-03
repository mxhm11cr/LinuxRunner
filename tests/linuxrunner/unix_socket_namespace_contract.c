#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failure(const char *stage, unsigned cycle) {
    printf("UNIX_NAMESPACE_FAIL stage=%s cycle=%u errno=%d\n", stage, cycle, errno);
    return 1;
}

int main(void) {
    for (unsigned i = 0; i < 100; i++) {
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) return failure("abstract-socket", i);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        int n = snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
                         "lr-reference-%u-%u", (unsigned)getpid(), i);
        socklen_t length = offsetof(struct sockaddr_un, sun_path) + 1 + n;
        if (bind(fd, (struct sockaddr *)&address, length) != 0) {
            int saved = errno; close(fd); errno = saved;
            return failure("abstract-bind", i);
        }
        struct sockaddr_un observed = {0}; socklen_t observed_length = sizeof(observed);
        if (getsockname(fd, (struct sockaddr *)&observed, &observed_length) ||
            observed_length != length || memcmp(&observed, &address, length)) {
            close(fd); return failure("abstract-name", i);
        }
        close(fd);
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/lr-reference-%u-", (unsigned)getpid());
    size_t used = strlen(address.sun_path);
    memset(address.sun_path + used, 'x', 100 - used); address.sun_path[100] = 0;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return failure("filesystem-socket", 100);
    if (bind(fd, (struct sockaddr *)&address, offsetof(struct sockaddr_un, sun_path) + 101)) {
        int saved = errno; close(fd); errno = saved;
        return failure("filesystem-bind", 100);
    }
    struct sockaddr_un observed = {0}; socklen_t observed_length = sizeof(observed);
    int result = getsockname(fd, (struct sockaddr *)&observed, &observed_length);
    close(fd); unlink(address.sun_path);
    if (result || strcmp(observed.sun_path, address.sun_path)) return failure("filesystem-name", 100);
    puts("UNIX_NAMESPACE_PASS abstract_cycles=100 filesystem_path_bytes=100");
    return 0;
}
