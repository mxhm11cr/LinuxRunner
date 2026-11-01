// A multithreaded process forks (and the child execs) while its worker
// threads hand work back and forth with the main thread, as Hyprland does
// with llvmpipe's rasterizer threads while it launches clients. Three kinds
// of synchronisation: mutex+condvar, a C11 atomic flag with spinning, and a
// raw (non-private) futex word. Every round must complete; a lost wake-up or
// a stale view of memory after fork hangs a round, which the alarm reports.
// Build: gcc -O2 -pthread fork_while_threads_sync_contract.c
// Prints FORK_THREADS_CONTRACT PASS.
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define WORKERS 4
#define ROUNDS 150
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t go = PTHREAD_COND_INITIALIZER, done = PTHREAD_COND_INITIALIZER;
static unsigned generation, finished;
static _Atomic unsigned spin_generation, spin_finished;
static _Atomic uint32_t futex_word;
static _Atomic unsigned futex_finished;
static volatile int current_round, current_kind;

static void on_alarm(int sig) { (void) sig; char m[96]; int n = snprintf(m, sizeof m, "FORK_THREADS_CONTRACT FAIL hung in round %d kind %d\n", current_round, current_kind); write(1, m, n); _exit(1); }
static long futex(_Atomic uint32_t *word, int op, uint32_t value) { return syscall(SYS_futex, word, op, value, NULL, NULL, 0); }

static void *worker(void *arg) {
    (void) arg;
    for (unsigned seen = 0, spun = 0, waited = 0; ;) {
        pthread_mutex_lock(&lock);
        while (generation == seen) pthread_cond_wait(&go, &lock);
        seen = generation;
        if (++finished == WORKERS) pthread_cond_signal(&done);
        pthread_mutex_unlock(&lock);
        while (atomic_load(&spin_generation) == spun) ;   // spin, as rasterizer threads may
        spun = atomic_load(&spin_generation);
        atomic_fetch_add(&spin_finished, 1);
        while (atomic_load(&futex_word) == waited) futex(&futex_word, FUTEX_WAIT, waited);   // non-private, as Mesa's locks
        waited = atomic_load(&futex_word);
        atomic_fetch_add(&futex_finished, 1);
    }
    return NULL;
}

static void spawn(void) {
    pid_t child = fork();
    if (child == 0) { execl("/bin/true", "true", (char *) NULL); _exit(127); }
    int status; waitpid(child, &status, 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; i++) pthread_create(&threads[i], NULL, worker, NULL);
    for (int round = 1; round <= ROUNDS; round++) {
        current_round = round; alarm(30);
        current_kind = 1;
        pthread_mutex_lock(&lock); finished = 0; generation++; pthread_cond_broadcast(&go);
        spawn();   // fork while the workers wake up and take the lock
        while (finished < WORKERS) pthread_cond_wait(&done, &lock);
        pthread_mutex_unlock(&lock);
        current_kind = 2;
        atomic_store(&spin_finished, 0); atomic_fetch_add(&spin_generation, 1);
        spawn();   // fork while the workers spin on the flag
        while (atomic_load(&spin_finished) < WORKERS) ;
        current_kind = 3;
        atomic_store(&futex_finished, 0); atomic_fetch_add(&futex_word, 1); futex(&futex_word, FUTEX_WAKE, WORKERS);
        spawn();   // fork while the workers sleep on the futex
        while (atomic_load(&futex_finished) < WORKERS) ;
    }
    puts("FORK_THREADS_CONTRACT PASS");
    return 0;
}
