// utimensat/futimens contract (Build 902, Fedora RPM "cpio: utime failed").
// RPM's fsmUtime() calls futimens() on each regular file it has just written
// under a temporary "name;XXXXXXXX" path, and lutimes() on symlinks. Linux
// sets the times on the open file itself; it must not re-resolve a path.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s (errno=%d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); failures++; } } while (0)

static struct timespec ts(long sec, long nsec) { struct timespec t = {sec, nsec}; return t; }

int main(void) {
    char dir[] = "/tmp/lr-utime-XXXXXX", path[256], moved[256], link[256], sub[256];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(path, sizeof(path), "%s/libmpfr.so.6.2.2;6aee3ff5", dir);
    snprintf(moved, sizeof(moved), "%s/libmpfr.so.6.2.2", dir);
    snprintf(link, sizeof(link), "%s/gawk.csh;6aee3ff5", dir);
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    struct stat st;

    // futimens on a freshly created regular file (the RPM pattern).
    int fd = open(path, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "x", 1) == 1);
    struct timespec t1[2] = {ts(1000, 0), ts(1568000000, 500)};
    CHECK(futimens(fd, t1) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_atime == 1000 && st.st_mtime == 1568000000);

    // The descriptor keeps working after rename and unlink.
    CHECK(rename(path, moved) == 0);
    struct timespec t2[2] = {ts(2000, 0), ts(2001, 0)};
    CHECK(futimens(fd, t2) == 0);
    CHECK(stat(moved, &st) == 0 && st.st_atime == 2000 && st.st_mtime == 2001);
    CHECK(unlink(moved) == 0);
    struct timespec t3[2] = {ts(3000, 0), ts(3001, 0)};
    CHECK(futimens(fd, t3) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_mtime == 3001);

    // Raw syscall with a NULL path: same as futimens, and EFAULT on AT_FDCWD.
    struct timespec t4[2] = {ts(4000, 0), ts(4001, 0)};
    CHECK(syscall(SYS_utimensat, fd, NULL, t4, 0) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_mtime == 4001);
    errno = 0; CHECK(syscall(SYS_utimensat, AT_FDCWD, NULL, t4, 0) == -1 && errno == EFAULT);
    errno = 0; CHECK(syscall(SYS_utimensat, -1, NULL, t4, 0) == -1 && errno == EBADF);
    // AT_EMPTY_PATH with "" acts on the descriptor; "" without it is ENOENT.
    struct timespec t5[2] = {ts(5000, 0), ts(5001, 0)};
    CHECK(utimensat(fd, "", t5, AT_EMPTY_PATH) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_mtime == 5001);
    errno = 0; CHECK(utimensat(fd, "", t5, 0) == -1 && errno == ENOENT);
    errno = 0; CHECK(utimensat(AT_FDCWD, dir, t5, 0x40000000) == -1 && errno == EINVAL);
    close(fd);

    // UTIME_OMIT keeps a time, UTIME_NOW sets the current time.
    fd = open(moved, O_WRONLY|O_CREAT|O_EXCL, 0644);
    CHECK(fd >= 0);
    struct timespec base[2] = {ts(5, 0), ts(6, 0)};
    CHECK(utimensat(AT_FDCWD, moved, base, 0) == 0);
    struct timespec omit_a[2] = {ts(0, UTIME_OMIT), ts(7, 0)};
    CHECK(utimensat(AT_FDCWD, moved, omit_a, 0) == 0);
    CHECK(stat(moved, &st) == 0 && st.st_atime == 5 && st.st_mtime == 7);
    struct timespec now_a[2] = {ts(0, UTIME_NOW), ts(0, UTIME_OMIT)};
    CHECK(futimens(fd, now_a) == 0);
    CHECK(stat(moved, &st) == 0 && st.st_atime > 1600000000 && st.st_mtime == 7);
    struct timespec both_omit[2] = {ts(0, UTIME_OMIT), ts(0, UTIME_OMIT)};
    CHECK(utimensat(AT_FDCWD, moved, both_omit, 0) == 0);
    CHECK(stat(moved, &st) == 0 && st.st_mtime == 7);
    struct timespec bad[2] = {ts(0, 1000000000), ts(0, 0)};
    errno = 0; CHECK(utimensat(AT_FDCWD, moved, bad, 0) == -1 && errno == EINVAL);
    CHECK(futimens(fd, NULL) == 0);
    CHECK(stat(moved, &st) == 0 && st.st_mtime > 1600000000);
    close(fd);

    // lutimes on a dangling symlink changes the link, not a target.
    CHECK(symlink("not-installed-yet", link) == 0);
    struct timespec t6[2] = {ts(6000, 0), ts(6001, 0)};
    CHECK(utimensat(AT_FDCWD, link, t6, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(lstat(link, &st) == 0 && S_ISLNK(st.st_mode) && st.st_mtime == 6001);
    errno = 0; CHECK(utimensat(AT_FDCWD, link, t6, 0) == -1 && errno == ENOENT);

    // Directory descriptors and legacy utimes().
    CHECK(mkdir(sub, 0755) == 0);
    int dfd = open(sub, O_RDONLY|O_DIRECTORY);
    CHECK(dfd >= 0);
    struct timespec t7[2] = {ts(7000, 0), ts(7001, 0)};
    CHECK(futimens(dfd, t7) == 0);
    CHECK(stat(sub, &st) == 0 && st.st_mtime == 7001);
    close(dfd);
    struct timeval tv[2] = {{8000, 0}, {8001, 0}};
    CHECK(utimes(sub, tv) == 0);
    CHECK(stat(sub, &st) == 0 && st.st_mtime == 8001);

    unlink(link); unlink(moved); rmdir(sub); rmdir(dir);
    if (failures) { fprintf(stderr, "regress_utimensat: %d failure(s)\n", failures); return 1; }
    puts("regress_utimensat: PASS");
    return 0;
}
