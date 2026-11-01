// Cross-process coherence of a software udmabuf export, as KWin -> PipeWire ->
// KRFB uses it: the producer keeps writing through its own mapping (of the
// memfd and of the dmabuf) after the consumer received the dmabuf over
// SCM_RIGHTS and mapped it. Every write must be visible to the consumer, and
// consumer writes to the producer. Needs ISH_SOFTWARE_DMABUF=1 and root.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

static void send_fd(int sock, int fd) {
    char c = 'f'; struct iovec io = {&c, 1};
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b};
    struct cmsghdr *h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    if (sendmsg(sock, &m, 0) != 1) { perror("sendmsg"); exit(3); }
}
static int recv_fd(int sock) {
    char c; struct iovec io = {&c, 1};
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b};
    if (recvmsg(sock, &m, 0) != 1) { perror("recvmsg"); exit(3); }
    int fd; memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof fd); return fd;
}
static void step(int sock) { char c = 's'; if (write(sock, &c, 1) != 1 || read(sock, &c, 1) != 1) exit(4); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const size_t size = 4 * 4096;
    char path[64]; snprintf(path, sizeof path, "/tmp/lr-udmabuf-xproc-%d", getpid());
    if (mknod(path, S_IFCHR | 0600, makedev(10, 254))) { perror("mknod"); return 2; }
    int control = open(path, O_RDWR | O_CLOEXEC); unlink(path);
    if (control < 0) { perror("open control"); return 2; }
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t child = fork();
    if (child == 0) {   // consumer (KRFB's role)
        close(sv[0]);
        int buf = recv_fd(sv[1]);
        unsigned char *v = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, buf, 0);
        if (v == MAP_FAILED) { perror("consumer mmap"); _exit(5); }
        step(sv[1]);                                   // producer writes round 1 after our map
        printf("XPROC consumer sees round1 %s (%02x %02x)\n", v[0] == 0x11 && v[size - 1] == 0x12 ? "PASS" : "FAIL", v[0], v[size - 1]);
        int ok1 = v[0] == 0x11 && v[size - 1] == 0x12;
        step(sv[1]);                                   // round 2 through the producer's dmabuf mapping
        int ok2 = v[4096] == 0x21;
        printf("XPROC consumer sees round2-via-dmabuf %s (%02x)\n", ok2 ? "PASS" : "FAIL", v[4096]);
        v[8192] = 0x31;                                // consumer writes back
        step(sv[1]);
        _exit(ok1 && ok2 ? 0 : 1);
    }
    close(sv[1]);
    int memfd = memfd_create("producer", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    ftruncate(memfd, size); fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);
    unsigned char *mine = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    struct udmabuf_create c = {memfd, UDMABUF_FLAGS_CLOEXEC, 0, size};
    int buf = ioctl(control, UDMABUF_CREATE, &c);
    if (buf < 0 || mine == MAP_FAILED) { perror("export"); return 2; }
    unsigned char *mydma = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, buf, 0);
    send_fd(sv[0], buf);
    char s; if (read(sv[0], &s, 1) != 1) return 4;     // consumer has mapped
    mine[0] = 0x11; mine[size - 1] = 0x12;               // round 1: through the memfd
    if (write(sv[0], &s, 1) != 1 || read(sv[0], &s, 1) != 1) return 4;
    mydma[4096] = 0x21;                                  // round 2: through the producer's dmabuf mapping
    if (write(sv[0], &s, 1) != 1 || read(sv[0], &s, 1) != 1) return 4;
    int back = mine[8192] == 0x31;
    printf("XPROC producer sees consumer write %s (%02x)\n", back ? "PASS" : "FAIL", mine[8192]);
    if (write(sv[0], &s, 1) != 1) return 4;
    int status; waitpid(child, &status, 0);
    int pass = back && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("XPROC_RESULT %s\n", pass ? "PASS" : "FAIL");
    return !pass;
}
