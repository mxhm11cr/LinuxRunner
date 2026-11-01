// Guest FP exception flags and rounding mode must survive leaving translated
// code (syscalls, yields, a second thread running FP code) and be cleared only
// by the guest. The runtime keeps the guest FPSR live in the host register and
// writes FPCR/FPSR only when they differ, so this checks that both directions
// stay exact. Prints FPSR_CONTRACT PASS, or the first failed step.
#include <fenv.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

static volatile double zero = 0.0, one = 1.0, three = 3.0;
static int fails;
static void check(const char *step, int ok) { if (!ok) { printf("FPSR_CONTRACT FAIL %s\n", step); fails++; } }
static void leave(void) { (void) getpid(); sched_yield(); (void) getppid(); }

static void *noisy(void *arg) {
    (void) arg;
    // Different flags and rounding in another thread must not leak.
    fesetround(FE_DOWNWARD);
    volatile double x = 0;
    for (int i = 0; i < 20000; i++) x += one / three + one / zero;
    feclearexcept(FE_ALL_EXCEPT);
    return NULL;
}

int main(void) {
    feclearexcept(FE_ALL_EXCEPT);
    leave();
    check("clear survives syscalls", fetestexcept(FE_ALL_EXCEPT) == 0);

    volatile double r = one / zero;
    (void) r;
    leave();
    check("divbyzero survives syscalls", fetestexcept(FE_DIVBYZERO) != 0);
    check("no stray inexact", fetestexcept(FE_INEXACT) == 0);

    r = one / three;
    for (int i = 0; i < 1000; i++) leave();
    check("accumulated flags survive", fetestexcept(FE_DIVBYZERO | FE_INEXACT) == (FE_DIVBYZERO | FE_INEXACT));

    feclearexcept(FE_ALL_EXCEPT);
    leave();
    check("clear after flags survives", fetestexcept(FE_ALL_EXCEPT) == 0);

    fesetround(FE_UPWARD);
    leave();
    double up = one / three;
    fesetround(FE_DOWNWARD);
    leave();
    double down = one / three;
    fesetround(FE_TONEAREST);
    check("rounding mode survives syscalls", up > down && fegetround() == FE_TONEAREST);

    feclearexcept(FE_ALL_EXCEPT);
    pthread_t t;
    pthread_create(&t, NULL, noisy, NULL);
    for (int i = 0; i < 2000; i++) { leave(); }
    pthread_join(t, NULL);
    check("other thread's flags do not leak", fetestexcept(FE_ALL_EXCEPT) == 0);
    check("other thread's rounding does not leak", fegetround() == FE_TONEAREST);

    if (!fails) printf("FPSR_CONTRACT PASS\n");
    return fails != 0;
}
