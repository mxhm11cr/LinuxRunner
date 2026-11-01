// mincore must say whether memory is mapped: ENOMEM for unmapped pages,
// EINVAL for an unaligned address, one byte per page otherwise. Mesa's EGL
// (_eglPointerIsDereferenceable) relies on it to tell a wl_egl_window version
// number from a pointer. Prints MINCORE_CONTRACT PASS.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fails;
static void check(const char *step, int ok) { if (!ok) { printf("MINCORE_CONTRACT FAIL %s errno=%d (%s)\n", step, errno, strerror(errno)); fails++; } }

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    unsigned char vec[4] = {0};
    errno = 0;
    check("page 0 is ENOMEM", mincore((void *) 0, page, vec) < 0 && errno == ENOMEM);
    char *p = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap", p != MAP_FAILED);
    p[0] = 1; p[2 * page] = 1;
    check("mapped range", mincore(p, 3 * page, vec) == 0 && (vec[0] & 1) && (vec[2] & 1));
    errno = 0;
    check("unaligned is EINVAL", mincore(p + 1, page, vec) < 0 && errno == EINVAL);
    munmap(p + 2 * page, page);
    errno = 0;
    check("range with unmapped tail is ENOMEM", mincore(p, 3 * page, vec) < 0 && errno == ENOMEM);
    check("prefix still fine", mincore(p, 2 * page, vec) == 0);
    errno = 0;
    check("small integer as pointer is ENOMEM", mincore((void *) ((uintptr_t) 3 & ~(uintptr_t) (page - 1)), page, vec) < 0 && errno == ENOMEM);
    if (!fails) puts("MINCORE_CONTRACT PASS");
    return fails != 0;
}
