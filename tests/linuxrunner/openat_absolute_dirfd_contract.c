#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line=%d errno=%d: %s\n", __LINE__, errno, #c); return 1; } } while (0)
int main(void) {
    char dir[] = "/tmp/lr-openat-XXXXXX", file[128], link[128];
    CHECK(mkdtemp(dir));
    snprintf(file, sizeof file, "%s/file", dir);
    snprintf(link, sizeof link, "%s/link", dir);
    int d = open(dir, O_RDONLY | O_DIRECTORY);
    CHECK(d >= 0);
    int plain = open(file, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(plain >= 0);
    CHECK(symlink("file", link) == 0);
    const int fds[] = {-1, 1000000, plain};
    for (unsigned i = 0; i < sizeof fds / sizeof fds[0]; i++) {
        // RPM's ensureDir() begins with openat(-1, "/", O_NOFOLLOW).
        int fd = openat(fds[i], "/", O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        CHECK(fd >= 0);
        struct stat st;
        CHECK(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode));
        CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        CHECK(close(fd) == 0);
        fd = openat(fds[i], file, O_RDONLY);
        CHECK(fd >= 0 && close(fd) == 0);
        errno = 0;
        CHECK(openat(fds[i], link, O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
#ifdef SYS_openat2
        struct { uint64_t flags, mode, resolve; } how = {O_RDONLY | O_DIRECTORY, 0, 0};
        fd = syscall(SYS_openat2, fds[i], "/", &how, sizeof how);
        CHECK(fd >= 0 && close(fd) == 0);
#endif
    }
    errno = 0;
    CHECK(openat(-1, "file", O_RDONLY) == -1 && errno == EBADF);
    int fd = openat(d, "file", O_RDONLY);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(close(plain) == 0 && close(d) == 0);
    CHECK(unlink(link) == 0 && unlink(file) == 0 && rmdir(dir) == 0);
    puts("OPENAT_ABSOLUTE_DIRFD_PASS invalid_dirfd_ignored relative_errors nofollow cloexec openat2");
    return 0;
}
