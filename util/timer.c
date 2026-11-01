#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include "util/timer.h"
#include "misc.h"

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data) {
//    assert(clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME);
    struct timer *timer = malloc(sizeof(struct timer));
    timer->clockid = clockid;
    timer->callback = callback;
    timer->data = data;
    timer->active = false;
    timer->thread_running = false;
    lock_init(&timer->lock);
    pthread_cond_init(&timer->wake, NULL);
    timer->dead = false;
    return timer;
}

void timer_free(struct timer *timer) {
    lock(&timer->lock);
    timer->active = false;
    if (timer->thread_running) {
        timer->dead = true;
        pthread_cond_signal(&timer->wake);
        unlock(&timer->lock);
    } else {
        unlock(&timer->lock);
        pthread_cond_destroy(&timer->wake);
        free(timer);
    }
}

// Sleep for at most `remaining` with timer->lock held; returns with it held.
// The wait releases the lock atomically, so a timer_set between computing the
// deadline and sleeping cannot be missed (a signal sent before nanosleep was).
static void timer_sleep(struct timer *timer, struct timespec remaining) {
#ifdef __APPLE__
    pthread_cond_timedwait_relative_np(&timer->wake, &timer->lock.m, &remaining);
#else
    struct timespec deadline = timespec_add(timespec_now(CLOCK_REALTIME), remaining);
    pthread_cond_timedwait(&timer->wake, &timer->lock.m, &deadline);
#endif
    timer->lock.owner = pthread_self(); // the wait reacquired the mutex
}

static void *timer_thread(void *param) {
    struct timer *timer = param;
    lock(&timer->lock);
    while (true) {
        struct timespec remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        while (timer->active && !timer->dead && timespec_positive(remaining)) {
            timer_sleep(timer, remaining);
            remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        }
        if (timer->dead)
            break;
        if (timer->active)
            timer->callback(timer->data);
        if (timer->active && timespec_positive(timer->interval)) {
            timer->start = timer->end;
            timer->end = timespec_add(timer->start, timer->interval);
        } else {
            break;
        }
    }
    timer->thread_running = false;
    if (timer->dead) {
        unlock(&timer->lock);
        pthread_cond_destroy(&timer->wake);
        free(timer);
    }
    else
        unlock(&timer->lock);
    return NULL;
}

int timer_set(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec) {
    lock(&timer->lock);
    struct timespec now = timespec_now(timer->clockid);
    if (oldspec != NULL) {
        oldspec->value = timespec_subtract(timer->end, now);
        oldspec->interval = timer->interval;
    }

    timer->start = now;
    timer->end = timespec_add(timer->start, spec.value);
    timer->interval = spec.interval;
    timer->active = !timespec_is_zero(spec.value);
    if (timer->thread_running) {
        pthread_cond_signal(&timer->wake);
    } else if (timer->active) {
        timer->thread_running = true;
        pthread_create(&timer->thread, NULL, timer_thread, timer);
        pthread_detach(timer->thread);
    }
    unlock(&timer->lock);
    return 0;
}
