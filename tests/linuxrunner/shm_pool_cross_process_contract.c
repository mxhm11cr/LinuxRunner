// wl_shm pool pattern across processes: the client creates a sealed memfd
// (libwayland os_create_anonymous_file), maps it, passes the fd over a Unix
// socket; the compositor maps it read-only. The client draws after the
// compositor mapped, then grows the pool (ftruncate) and draws into the new
// tail; the compositor remaps the larger size (as wl_shm_pool.resize does) and
// must see both. Compositor-side mmap offsets that are not 16 KiB aligned are
// covered by a buffer at a 4 KiB offset.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void send_fd(int sock, int fd) {
    char c = 'f'; struct iovec io = {&c, 1};
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b};
    struct cmsghdr *h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    if (sendmsg(sock, &m, 0) != 1) exit(3);
}
static int recv_fd(int sock) {
    char c; struct iovec io = {&c, 1};
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b};
    if (recvmsg(sock, &m, 0) != 1) exit(3);
    int fd; memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof fd); return fd;
}
static void sync_step(int sock) { char c = 's'; if (write(sock, &c, 1) != 1 || read(sock, &c, 1) != 1) exit(4); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const size_t small = 256 * 256 * 4, big = 2 * small;
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t child = fork();
    if (child == 0) {   // compositor
        close(sv[0]);
        int pool = recv_fd(sv[1]);
        const unsigned char *v = mmap(NULL, small, PROT_READ, MAP_SHARED, pool, 0);
        const unsigned char *off = mmap(NULL, 4096, PROT_READ, MAP_SHARED, pool, 4096);
        if (v == MAP_FAILED || off == MAP_FAILED) { perror("compositor mmap"); _exit(5); }
        sync_step(sv[1]);                                    // client draws
        int ok1 = v[0] == 0x40 && v[small - 1] == 0x41 && off[0] == 0x42;
        printf("SHMPOOL draw-after-map %s (%02x %02x off=%02x)\n", ok1 ? "PASS" : "FAIL", v[0], v[small - 1], off[0]);
        sync_step(sv[1]);                                    // client grows and draws in the tail
        munmap((void *) v, small);
        v = mmap(NULL, big, PROT_READ, MAP_SHARED, pool, 0);
        int ok2 = v != MAP_FAILED && v[small] == 0x50 && v[big - 1] == 0x51 && v[0] == 0x40;
        printf("SHMPOOL grow-and-remap %s (%02x %02x %02x)\n", ok2 ? "PASS" : "FAIL", v[0], v[small], v[big - 1]);
        sync_step(sv[1]);                                    // client redraws the head
        int ok3 = v[0] == 0x60;
        printf("SHMPOOL redraw-visible %s (%02x)\n", ok3 ? "PASS" : "FAIL", v[0]);
        _exit(ok1 && ok2 && ok3 ? 0 : 1);
    }
    close(sv[1]);
    int pool = memfd_create("wayland-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    fcntl(pool, F_ADD_SEALS, F_SEAL_SHRINK);
    if (posix_fallocate(pool, 0, small)) ftruncate(pool, small);
    unsigned char *mine = mmap(NULL, small, PROT_READ | PROT_WRITE, MAP_SHARED, pool, 0);
    send_fd(sv[0], pool);
    char s; if (read(sv[0], &s, 1) != 1) return 4;
    mine[0] = 0x40; mine[small - 1] = 0x41; mine[4096] = 0x42;
    if (write(sv[0], &s, 1) != 1 || read(sv[0], &s, 1) != 1) return 4;
    if (ftruncate(pool, big)) { perror("grow"); return 6; }
    unsigned char *grown = mremap(mine, small, big, MREMAP_MAYMOVE);
    if (grown == MAP_FAILED) { perror("mremap"); return 6; }
    grown[small] = 0x50; grown[big - 1] = 0x51;
    if (write(sv[0], &s, 1) != 1 || read(sv[0], &s, 1) != 1) return 4;
    grown[0] = 0x60;
    if (write(sv[0], &s, 1) != 1) return 4;
    int status; waitpid(child, &status, 0);
    int pass = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("SHMPOOL_RESULT %s\n", pass ? "PASS" : "FAIL");
    return !pass;
}
