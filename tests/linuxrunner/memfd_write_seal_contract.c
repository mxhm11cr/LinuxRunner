// memfd F_SEAL_WRITE / F_SEAL_FUTURE_WRITE (Linux shmem semantics), including
// KWin's RamFile pattern (write, munmap, seal SHRINK|GROW|WRITE|SEAL).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void check(const char *name, int ok) { printf("SEAL_CASE %-34s %s\n", name, ok ? "PASS" : "FAIL"); if (!ok) failures++; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const size_t size = 3 * 4096;
    int fd = memfd_create("seal-write", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ftruncate(fd, size);
    char *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memcpy(rw, "table", 6);
    check("busy-with-writable-shared-map", fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) < 0 && errno == EBUSY);
    munmap(rw, size);
    check("ramfile-seal-after-munmap", fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) == 0);
    check("get-seals", fcntl(fd, F_GET_SEALS) == (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL));
    check("write-eperm", write(fd, "x", 1) < 0 && errno == EPERM);
    check("pwrite-eperm", pwrite(fd, "x", 1, 0) < 0 && errno == EPERM);
    check("shared-writable-map-eperm", mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EPERM);
    char *ro = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    check("shared-readonly-map-reads", ro != MAP_FAILED && !memcmp(ro, "table", 6));
    check("mprotect-upgrade-refused", mprotect(ro, size, PROT_READ | PROT_WRITE) < 0 && errno == EACCES);
    char *priv = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (priv != MAP_FAILED) priv[0] = 'X';
    check("private-cow-allowed-file-unchanged", priv != MAP_FAILED && ro[0] == 't');
    char buf[6] = {0};
    check("read-still-works", pread(fd, buf, 5, 0) == 5 && !memcmp(buf, "table", 5));

    int f2 = memfd_create("seal-future", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ftruncate(f2, size);
    char *live = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, f2, 0);
    check("future-write-with-live-map", fcntl(f2, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0);
    live[0] = 'L';
    check("future-write-existing-map-writes", pread(f2, buf, 1, 0) == 1 && buf[0] == 'L');
    check("future-write-new-map-eperm", mmap(NULL, size, PROT_WRITE, MAP_SHARED, f2, 0) == MAP_FAILED && errno == EPERM);
    check("future-write-write-eperm", write(f2, "x", 1) < 0 && errno == EPERM);

    // A writable shared mapping inherited by a child still blocks F_SEAL_WRITE.
    int f3 = memfd_create("seal-fork", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ftruncate(f3, size);
    char *shared = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, f3, 0);
    int pipefd[2]; pipe(pipefd);
    pid_t child = fork();
    if (child == 0) { char c; close(pipefd[1]); read(pipefd[0], &c, 1); shared[0] = 'c'; _exit(0); }
    close(pipefd[0]);
    munmap(shared, size);
    check("busy-while-child-maps-writable", fcntl(f3, F_ADD_SEALS, F_SEAL_WRITE) < 0 && errno == EBUSY);
    write(pipefd[1], "g", 1);
    waitpid(child, NULL, 0);
    check("seal-after-child-exit", fcntl(f3, F_ADD_SEALS, F_SEAL_WRITE) == 0);

    printf("SEAL_RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
