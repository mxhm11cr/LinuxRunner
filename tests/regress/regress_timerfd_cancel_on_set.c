// timerfd_settime(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET) on
// CLOCK_REALTIME must be accepted (Plasma's clock and many daemons use it),
// expire normally while the wall clock is not stepped, and be ignored for
// relative or monotonic timers, as on Linux. Unknown flags stay EINVAL.
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#ifndef TFD_TIMER_CANCEL_ON_SET
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#endif
static int fail(const char *what) { printf("FAIL %s errno=%d (%s)\n", what, errno, strerror(errno)); return 1; }
static int expire_once(int fd, int flags, int absolute) {
    struct itimerspec spec = {0};
    if (absolute) { clock_gettime(CLOCK_REALTIME, &spec.it_value); spec.it_value.tv_nsec += 150000000; if (spec.it_value.tv_nsec >= 1000000000) { spec.it_value.tv_sec++; spec.it_value.tv_nsec -= 1000000000; } }
    else spec.it_value.tv_nsec = 150000000;
    if (timerfd_settime(fd, flags, &spec, NULL) != 0) return fail("settime");
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 3000) != 1 || !(p.revents & POLLIN)) return fail("poll for expiration");
    uint64_t n = 0;
    if (read(fd, &n, sizeof n) != sizeof n || n != 1) return fail("read expiration count");
    return 0;
}
int main(void) {
    int rt = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
    int mono = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (rt < 0 || mono < 0) return fail("timerfd_create");
    if (expire_once(rt, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, 1)) return 1;
    // Ignored (not an error) without an absolute realtime timer.
    if (expire_once(rt, TFD_TIMER_CANCEL_ON_SET, 0)) return 1;
    if (expire_once(mono, TFD_TIMER_CANCEL_ON_SET, 0)) return 1;
    struct itimerspec spec = {0}; spec.it_value.tv_sec = 1;
    errno = 0;
    if (timerfd_settime(rt, 1 << 5, &spec, NULL) != -1 || errno != EINVAL) return fail("unknown flag must be EINVAL");
    // Disarm; a non-blocking read with nothing pending is EAGAIN, not ECANCELED.
    struct itimerspec off = {0};
    if (timerfd_settime(rt, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &off, NULL) != 0) return fail("disarm");
    int nb = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    uint64_t n;
    if (timerfd_settime(nb, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &off, NULL) != 0) return fail("arm nonblocking");
    errno = 0;
    if (read(nb, &n, sizeof n) != -1 || errno != EAGAIN) return fail("idle read must be EAGAIN");
    puts("PASS timerfd TFD_TIMER_CANCEL_ON_SET");
    return 0;
}
