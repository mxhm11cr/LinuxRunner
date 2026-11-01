// SO_RCVTIMEO / SO_SNDTIMEO with the option numbers 64-bit libcs use (20/21):
// set, read back, and a receive that times out with EAGAIN instead of
// blocking. hyprctl and other IPC clients refuse to run without them.
// Prints SOCKET_TIMEOUT_CONTRACT PASS.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(const char *step, int ok) { if (!ok) { printf("SOCKET_TIMEOUT_CONTRACT FAIL %s errno=%d (%s)\n", step, errno, strerror(errno)); fails++; } }

int main(void) {
    int pair[2];
    check("socketpair", socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
    check("set SO_RCVTIMEO", setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    check("set SO_SNDTIMEO", setsockopt(pair[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0);
    struct timeval got = {0}; socklen_t n = sizeof got;
    check("get SO_RCVTIMEO", getsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &got, &n) == 0 && n == sizeof got &&
          got.tv_sec == 0 && got.tv_usec >= 290000 && got.tv_usec <= 310000);
    struct timespec a, b; char c;
    clock_gettime(CLOCK_MONOTONIC, &a);
    errno = 0;
    ssize_t r = recv(pair[0], &c, 1, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double waited = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    check("recv times out with EAGAIN", r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    check("timeout is honoured", waited > 0.2 && waited < 2.0);
    if (!fails) puts("SOCKET_TIMEOUT_CONTRACT PASS");
    return fails != 0;
}
