#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "RELATIVE_DIRFD_FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); return 1; } } while (0)
#define ERROR(x, e) do { errno = 0; CHECK((x) == -1 && errno == (e)); } while (0)

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--stdio-only")) {
        struct stat st;
        CHECK(fstat(STDIN_FILENO, &st) == 0 && !S_ISDIR(st.st_mode));
        // The actual native Steam updater probe that aborted the host runtime.
        ERROR(syscall(SYS_faccessat, STDIN_FILENO, "./.writable", W_OK), ENOTDIR);
        puts("RELATIVE_DIRFD_STDIO_PASS actual updater probe, no host abort");
        return 0;
    }
    char directory[] = "/tmp/lr-relative-dirfd-XXXXXX", file[256], link[256];
    CHECK(mkdtemp(directory));
    snprintf(file, sizeof file, "%s/file", directory);
    snprintf(link, sizeof link, "%s/link", directory);
    int dir = open(directory, O_PATH | O_DIRECTORY);
    int plain = open(file, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(dir >= 0 && plain >= 0 && write(plain, "data", 4) == 4);
    CHECK(symlink("file", link) == 0);
    int path = open(file, O_PATH), symlinkfd = open(link, O_PATH | O_NOFOLLOW);
    int pipes[2]; CHECK(pipe(pipes) == 0);
    int event = eventfd(0, EFD_CLOEXEC), epoll = epoll_create1(EPOLL_CLOEXEC);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    int mem = syscall(SYS_memfd_create, "relative-dirfd", 0);
    CHECK(path >= 0 && symlinkfd >= 0 && event >= 0 && epoll >= 0 && sock >= 0 && mem >= 0);
    const int descriptors[] = {plain, path, symlinkfd, pipes[0], event, epoll, sock, mem};
    for (unsigned i = 0; i < sizeof descriptors / sizeof descriptors[0]; ++i) {
        int fd = descriptors[i]; struct stat st; char buffer[32];
        ERROR(syscall(SYS_faccessat, fd, ".", F_OK), ENOTDIR);
        ERROR(syscall(SYS_faccessat, fd, "./.writable", W_OK), ENOTDIR);
        ERROR(faccessat(fd, ".", R_OK, AT_EACCESS), ENOTDIR);
#ifdef SYS_faccessat2
        ERROR(syscall(SYS_faccessat2, fd, "./.writable", W_OK, 0), ENOTDIR);
#endif
        ERROR(openat(fd, ".", O_RDONLY), ENOTDIR);
        ERROR(fstatat(fd, ".", &st, 0), ENOTDIR);
        ERROR(mkdirat(fd, "child", 0700), ENOTDIR);
        ERROR(unlinkat(fd, ".", 0), ENOTDIR);
        ERROR(readlinkat(fd, ".", buffer, sizeof buffer), ENOTDIR);
        ERROR(renameat(fd, ".", dir, "moved"), ENOTDIR);
        // Absolute paths ignore the valid, non-directory descriptor.
        CHECK(syscall(SYS_faccessat, fd, "/", F_OK) == 0);
        int root = openat(fd, "/", O_PATH | O_DIRECTORY);
        CHECK(root >= 0 && close(root) == 0);
        ERROR(syscall(SYS_faccessat, fd, "", F_OK), ENOENT);
    }
    CHECK(faccessat(dir, "file", R_OK, 0) == 0);
    int child = openat(dir, "file", O_RDONLY); CHECK(child >= 0 && close(child) == 0);
    struct stat st; CHECK(fstatat(dir, ".", &st, 0) == 0 && S_ISDIR(st.st_mode));
    CHECK(fstat(plain, &st) == 0 && st.st_nlink == 1 && st.st_size == 4);
    ERROR(syscall(SYS_faccessat, -1, "relative", F_OK), EBADF);
    for (unsigned i = 0; i < sizeof descriptors / sizeof descriptors[0]; ++i) close(descriptors[i]);
    close(pipes[1]); close(dir);
    CHECK(unlink(link) == 0 && unlink(file) == 0 && rmdir(directory) == 0);
    puts("RELATIVE_DIRFD_CONTRACT_PASS regular O_PATH symlink pipe eventfd epoll socket memfd absolute empty directory");
    return 0;
}
