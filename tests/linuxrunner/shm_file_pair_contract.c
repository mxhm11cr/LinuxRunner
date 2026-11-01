// wlroots' allocate_shm_file_pair (keymaps for every Wayland keyboard): an
// exclusive POSIX shm object, a read-only second open, unlink, fchmod(rw, 0)
// so it cannot be reopened writable, then ftruncate/mmap through the
// already-open writable descriptor. Linux checks permissions at open time
// only, so every step after fchmod must still succeed. Also the memfd variant
// (memfd_create + seals + read-only /proc/self/fd reopen). Prints
// SHM_PAIR_CONTRACT PASS or the first failing step with errno.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static void check(const char *step, int ok) { if (!ok) { printf("SHM_PAIR_CONTRACT FAIL %s errno=%d (%s)\n", step, errno, strerror(errno)); fails++; } }

static void pair(int rw, int ro, const char *kind) {
    char step[64];
    snprintf(step, sizeof step, "%s ftruncate after fchmod 0", kind);
    check(step, ftruncate(rw, 8192) == 0);
    if (fails) return;
    char *w = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, rw, 0);
    snprintf(step, sizeof step, "%s mmap rw", kind);
    check(step, w != MAP_FAILED);
    if (w == MAP_FAILED) return;
    strcpy(w + 4096, "xkb_keymap");
    char *r = mmap(NULL, 8192, PROT_READ, MAP_SHARED, ro, 0);
    snprintf(step, sizeof step, "%s mmap ro", kind);
    check(step, r != MAP_FAILED);
    if (r != MAP_FAILED) {
        snprintf(step, sizeof step, "%s ro view sees data", kind);
        check(step, !strcmp(r + 4096, "xkb_keymap"));
        munmap(r, 8192);
    }
    snprintf(step, sizeof step, "%s ro fd is read-only", kind);
    check(step, write(ro, "x", 1) < 0);
    munmap(w, 8192);
}

int main(void) {
    char name[] = "/lr-shm-pair-XXXXXX";
    snprintf(name + 13, 7, "%06d", (int) getpid() % 1000000);
    int rw = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check("shm_open excl", rw >= 0);
    int ro = rw >= 0 ? shm_open(name, O_RDONLY | O_CLOEXEC, 0) : -1;
    check("shm_open rdonly", ro >= 0);
    if (rw >= 0) {
        check("shm_unlink", shm_unlink(name) == 0);
        check("fchmod 0", fchmod(rw, 0) == 0);
        if (ro >= 0) pair(rw, ro, "shm");
    }
    int mfd = memfd_create("lr-shm-pair", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    check("memfd_create", mfd >= 0);
    if (mfd >= 0) {
        char path[64];
        snprintf(path, sizeof path, "/proc/self/fd/%d", mfd);
        int mro = open(path, O_RDONLY | O_CLOEXEC);
        check("memfd reopen rdonly", mro >= 0);
        if (mro >= 0) {
            check("fchmod memfd 0", fchmod(mfd, 0) == 0);
            pair(mfd, mro, "memfd");
            check("seal shrink/grow", fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0);
        }
    }
    if (!fails) printf("SHM_PAIR_CONTRACT PASS\n");
    return fails != 0;
}
