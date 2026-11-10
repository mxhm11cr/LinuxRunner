#include "kernel/task.h"
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <sys/resource.h>
#include <stdio.h>
#include "misc.h"
#include "util/list.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/errqueue.h"

#include "fs/sockrestart.h"
extern const struct fd_ops socket_fdops;

// From kernel/calls.h - avoid circular include


#if defined(__linux__)
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#elif defined(__APPLE__)
#include <sys/event.h>
#define HAVE_KQUEUE 1
#endif

static int real_poll_init(struct real_poll *real);
static void real_poll_close(struct real_poll *real);
struct real_poll_event {
#if HAVE_EPOLL
    struct epoll_event real;
#elif HAVE_KQUEUE
    struct kevent real;
#endif
};
static void *rpe_data(struct real_poll_event *rpe);
static int rpe_events(struct real_poll_event *rpe);
static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout);
static int real_poll_update(struct real_poll *real, int fd, int types, void *data);

static inline void owned_close(int fd) {
    // These are owned backend/notification handles, not guest fd numbers.
    // A closed host stdio slot may legitimately be reused by kqueue/pipe.
    if (fd >= 0) close(fd);
}

// A kqueue user event wakes pollers and makes a nested kqueue readable
// without reserving two host descriptors for every guest epoll/poll instance.
static void poll_notify(struct poll *poll) {
#if HAVE_KQUEUE
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    (void)kevent(poll->real.fd, &event, 1, NULL, 0, NULL);
#else
    (void)write(poll->notify_pipe[1], "", 1);
#endif
}

static void poll_drain_notify(struct poll *poll) {
#if !HAVE_KQUEUE
    char bytes[256];
    while (read(poll->notify_pipe[0], bytes, sizeof bytes) > 0) {}
#else
    (void)poll; // EV_CLEAR drains the user event when kevent returns it.
#endif
}

// lock order: fd, then poll

struct poll *poll_create() {
    struct poll *poll = malloc(sizeof(struct poll));
    if (poll == NULL)
        return ERR_PTR(_ENOMEM);
    int err = real_poll_init(&poll->real);
    if (err < 0) {
        int error = errno_map();
        // Distinguish host backend exhaustion from guest EMFILE. Bound logs;
        // never conceal the failure by changing the guest descriptor limit.
        static atomic_uint reports = 0;
        if (atomic_fetch_add(&reports, 1) < 8) {
            struct rlimit host = {0};
            getrlimit(RLIMIT_NOFILE, &host);
            char line[256];
            snprintf(line,sizeof line,"POLL_BACKEND_FAILURE: pid=%d error=%d host_nofile_cur=%llu host_nofile_max=%llu",
                current ? current->pid : 0, error, (unsigned long long)host.rlim_cur, (unsigned long long)host.rlim_max);
            fprintf(stderr,"%s\n",line);
            extern void (*runtime_fault_context)(const char *);
            if (runtime_fault_context) runtime_fault_context(line);
        }
        free(poll);
        return ERR_PTR(error);
    }
    poll->waiters = 0;
    list_init(&poll->epoll_link);
    poll->epoll_epoch = 0; poll->epoll_height = 0;
    poll->notify_pipe[0] = -1;
    poll->notify_pipe[1] = -1;
    list_init(&poll->poll_fds);
    list_init(&poll->pollfd_freelist);
    lock_init(&poll->lock);
    // Persistent notifications make an epoll backend itself pollable, including
    // custom guest eventfds/timers, without a worker thread or periodic probing.
#if HAVE_KQUEUE
    struct kevent event;
    EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(poll->real.fd, &event, 1, NULL, 0, NULL) < 0) {
#else
    if (pipe(poll->notify_pipe) < 0 ||
        fcntl(poll->notify_pipe[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(poll->notify_pipe[1], F_SETFL, O_NONBLOCK) < 0 ||
        real_poll_update(&poll->real, poll->notify_pipe[0], POLL_READ, NULL) < 0) {
#endif
        int error = errno_map();
        owned_close(poll->notify_pipe[0]); owned_close(poll->notify_pipe[1]);
        real_poll_close(&poll->real); pthread_mutex_destroy(&poll->lock.m);
        free(poll); return ERR_PTR(error);
    }
    return poll;
}

static inline bool poll_fd_is_real(struct poll_fd *pollfd) {
    // poll/select accept regular files and directories as immediately ready,
    // but the host epoll backend rejects their registration with EPERM. Keep
    // them in the userspace readiness scan instead. Guest epoll_ctl has its
    // own explicit regular-file/directory rejection and remains unchanged.
    mode_t_ type = pollfd->fd->type;
    return (pollfd->fd->ops->poll == realfs_poll || pollfd->fd->ops == &socket_fdops || pollfd->fd->ops == &epoll_fdops)
        && type != S_IFREG && type != S_IFDIR;
}

// does not do its own locking
// Match on the (struct fd, guest fd number) pair — see fd_no in fs/poll.h.
static struct poll_fd *poll_find_fd(struct poll *poll, struct fd *fd, int fd_no) {
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        if (poll_fd->fd == fd && poll_fd->fd_no == fd_no)
            return poll_fd;
    }
    return NULL;
}

// Recompute the host (real) registration for `fd` in this poll as the UNION
// of the event types of every registration referring to it. Several guest
// registrations (different fd numbers, same description after dup) share one
// host registration, since the host backend is keyed by real_fd. Passing 0
// types deletes the host registration. poll->lock must be held.
static int poll_real_refresh(struct poll *poll, struct fd *fd) {
    int types = 0;
    struct poll_fd *pf, *first = NULL;
    list_for_each_entry(&poll->poll_fds, pf, fds) {
        if (pf->fd == fd) {
            if (first == NULL)
                first = pf;
            if (!pf->oneshot_fired)
                types |= pf->types;
        }
    }
    if (fd->ops == &epoll_fdops) types &= POLL_READ | POLL_EDGETRIGGERED;
    // IP_RECVERR: ICMP errors arrive on a companion host socket. Linux reports
    // POLLERR whatever was requested, so any registration also watches it;
    // the wake is level-checked through the socket's own poll op.
    int companion = ip_errqueue_host_fd(fd);
    if (companion >= 0)
        real_poll_update(&poll->real, companion, types ? POLL_READ : 0, first);
    return real_poll_update(&poll->real, fd->real_fd, types, first);
}

// See comment on pollfd_freelist for context
static void poll_fd_free(struct poll_fd *poll_fd) {
    struct poll *poll = poll_fd->poll;
    memset(poll_fd, 0xba, sizeof(*poll_fd));
    poll_fd->poll = NULL; // used to mark it as free
    list_add(&poll->pollfd_freelist, &poll_fd->fds);
}

bool poll_has_fd(struct poll *poll, struct fd *fd, int fd_no) {
    lock(&poll->lock);
    bool found = poll_find_fd(poll, fd, fd_no) != NULL;
    unlock(&poll->lock);
    return found;
}

int poll_add_fd(struct poll *poll, struct fd *fd, int fd_no, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);

    struct poll_fd *poll_fd;
    if (poll_find_fd(poll, fd, fd_no)) { err = _EEXIST; goto out; }
    if (!list_empty(&poll->pollfd_freelist)) {
        poll_fd = list_first_entry(&poll->pollfd_freelist, struct poll_fd, fds);
        list_remove(&poll_fd->fds);
    } else {
        poll_fd = malloc(sizeof(struct poll_fd));
        if (poll_fd == NULL) {
            err = _ENOMEM;
            goto out;
        }
    }
    poll_fd->fd = fd;
    poll_fd->fd_no = fd_no;
    poll_fd->poll = poll;
    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0;
    poll_fd->oneshot_fired = false;

    // Link first, then refresh the (shared) host registration to the union of
    // all registrations of this description in this poll.
    list_add(&fd->poll_fds, &poll_fd->polls);
    list_add(&poll->poll_fds, &poll_fd->fds);

    if (poll_fd_is_real(poll_fd)) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            list_remove(&poll_fd->polls);
            list_remove(&poll_fd->fds);
            poll_fd_free(poll_fd);
            err = errno_map();
            goto out;
        }
    }

    err = 0;
    poll_notify(poll);
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_del_fd(struct poll *poll, struct fd *fd, int fd_no) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, fd_no);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    bool is_real = poll_fd_is_real(poll_fd);
    list_remove(&poll_fd->polls);
    list_remove(&poll_fd->fds);
    poll_fd_free(poll_fd);

    // Refresh (or delete, if this was the last registration) the shared host
    // registration to the union of the remaining registrations.
    if (is_real) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    err = 0;
    poll_notify(poll);
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_mod_fd(struct poll *poll, struct fd *fd, int fd_no, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, fd_no);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0; // MOD re-evaluates current readiness, including ET oneshots
    poll_fd->oneshot_fired = false; // EPOLL_CTL_MOD re-arms a fired oneshot

    if (poll_fd_is_real(poll_fd)) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    err = 0;
    poll_notify(poll);
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

void poll_cleanup_fd(struct fd *fd) {
    lock(&fd->poll_lock);
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&fd->poll_fds, poll_fd, tmp, polls) {
        lock(&poll_fd->poll->lock);
        if (poll_fd_is_real(poll_fd)) {
            int companion = ip_errqueue_host_fd(fd);
            if (companion >= 0) real_poll_update(&poll_fd->poll->real, companion, 0, poll_fd);
            real_poll_update(&poll_fd->poll->real, fd->real_fd, 0, poll_fd);
        }
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        struct poll *owner = poll_fd->poll;
        // The freelist is owned by the same lock as the active registrations.
        poll_fd_free(poll_fd);
        unlock(&owner->lock);
    }
    unlock(&fd->poll_lock);
}

void poll_wakeup(struct fd *fd, int events) {
    struct poll_fd *poll_fd;
    lock(&fd->poll_lock);
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock);
        if (poll_fd->oneshot_fired) {
            unlock(&poll->lock);
            continue;
        }
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_fd->triggered_types &= ~events;
        poll_notify(poll);
        unlock(&poll->lock);
        // oneshot?
    }
    unlock(&fd->poll_lock);
}

static void poll_record_backend(struct poll *poll, struct real_poll_event *events, int count) {
    for (int i=0;i<count;i++) {
        struct poll_fd *source=rpe_data(&events[i]);
        if (!source || source->poll != poll) continue;
        struct poll_fd *pf;
        list_for_each_entry(&poll->poll_fds,pf,fds)
            if (pf->fd == source->fd && (pf->types & POLL_EDGETRIGGERED))
                pf->triggered_types &= ~rpe_events(&events[i]);
    }
}

static void poll_observe_backend(struct poll *poll) {
    struct real_poll_event events[64]; struct timespec zero = {0};
    // Backend registrations are edge notifications, including guest LT sets.
    // Drain the complete static batch before delivering guest events: otherwise
    // a deferred native edge can re-arm an ET event we just delivered by scan.
    size_t budget = 1; struct poll_fd *pf;
    list_for_each_entry(&poll->poll_fds,pf,fds) budget += 3;
    int count;
    do {
        count = real_poll_wait(&poll->real, events, 64, &zero);
        poll_record_backend(poll,events,count);
        if (count < 64 || budget <= (size_t)count) break;
        budget -= count;
    } while (true);
    poll_drain_notify(poll);
}

bool poll_has_ready(struct poll *poll) {
    lock(&poll->lock);
    poll_observe_backend(poll);
    bool ready = false; struct poll_fd *pf;
    list_for_each_entry(&poll->poll_fds, pf, fds) {
        if (pf->oneshot_fired || !pf->fd->ops->poll) continue;
        unsigned events = pf->fd->ops->poll(pf->fd) & (pf->types | POLL_ERR | POLL_HUP);
        if (pf->types & POLL_EDGETRIGGERED) events &= ~pf->triggered_types;
        if (events) { ready = true; break; }
    }
    unlock(&poll->lock);
    return ready;
}

int poll_wait(struct poll *poll_, poll_callback_t callback, void *context, struct timespec *timeout) {
    struct timespec deadline = {0};
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) return _EINVAL;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        long nanos = deadline.tv_nsec + timeout->tv_nsec;
        time_t seconds;
        if (__builtin_add_overflow(deadline.tv_sec, timeout->tv_sec, &seconds) ||
            __builtin_add_overflow(seconds, (time_t)(nanos / 1000000000), &deadline.tv_sec)) {
            deadline.tv_sec = INT64_MAX; deadline.tv_nsec = 999999999;
        } else deadline.tv_nsec = nanos % 1000000000;
    }
    lock(&poll_->lock);

    poll_->waiters++;

    // Notifications cannot extend a finite call beyond its monotonic deadline.
    int res = 0;
    while (true) {
        poll_observe_backend(poll_);
        // check if any fds are ready
        struct poll_fd *poll_fd, *tmp;
        struct list *last_delivered = NULL;
        list_for_each_entry_safe(&poll_->poll_fds, poll_fd, tmp, fds) {
            if (poll_fd->oneshot_fired)
                continue;
            struct fd *fd = poll_fd->fd;
            int poll_types = 0;
            if (fd->ops->poll)
                poll_types = fd->ops->poll(fd);
            poll_types &= poll_fd->types | POLL_HUP | POLL_ERR;
            if (poll_fd->types & POLL_EDGETRIGGERED) {
                poll_types &= ~poll_fd->triggered_types;
            }
            if (poll_types) {
                // A bounded caller may reject this ready event because its
                // batch is full. Do not consume edge/oneshot state until the
                // event was actually accepted for delivery.
                if (callback(context, poll_types, poll_fd->info) != 1)
                    continue;
                res++;
                last_delivered = &poll_fd->fds;

                // The real poll does not actually get the FDs set as oneshot.
                // But this loop is done while holding the lock, so only one
                // thread can get each oneshot event. This doesn't solve the
                // thundering herd problem at all, but at least the semantics
                // are right. I'll just leave that as a TODO.
                if (poll_fd->types & POLL_ONESHOT) {
                    // Disable in place; do NOT unlink. We hold only
                    // poll->lock here, but poll_fd->polls is walked by
                    // poll_wakeup under fd->poll_lock — unlinking/freeing
                    // raced it and crashed the tty input thread (SIGSEGV in
                    // poll_wakeup when bun registers stdin with
                    // EPOLLONESHOT). Linux semantics also keep a fired
                    // oneshot registered, disabled until EPOLL_CTL_MOD.
                    poll_fd->oneshot_fired = true;
                    if (poll_fd_is_real(poll_fd)) {
                        // Refresh the shared host registration to the union
                        // of the still-armed registrations.
                        poll_real_refresh(poll_, fd);
                    }
                }

                if (poll_fd->types & POLL_EDGETRIGGERED) {
                    poll_fd->triggered_types |= poll_types;
                }
            }
        }
        if (res > 0) {
            // Rotate the accepted prefix behind the undelivered registrations
            // in O(1). A full batch must not starve persistent level readiness
            // at the list tail. Only the poll-owned fds list is modified.
            struct list *head = &poll_->poll_fds;
            if (last_delivered && last_delivered->next != head) {
                struct list *first = head->next, *next = last_delivered->next, *tail = head->prev;
                head->next = next; next->prev = head;
                tail->next = first; first->prev = tail;
                last_delivered->next = head; head->prev = last_delivered;
            }
            break;
        }

        bool signal_pending = false;
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            signal_pending = !!(current->pending & ~current->blocked);
            unlock(&current->sighand->lock);
        }
        if (signal_pending) {
            res = _EINTR;
            break;
        }

        // wait for a ready notification
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_begin_listen_wait(poll_fd->fd);
        }
        unlock(&poll_->lock);
        int err;
        int saved_errno;
        struct real_poll_event e[4];
        // Preserve the existing signal/group-exit recheck cap. Compute the
        // remaining budget on every wake, including notifications that did not
        // produce readiness; do not reset or subtract nominal whole seconds.
        struct timespec bounded_timeout = {.tv_sec = 1, .tv_nsec = 0};
        struct timespec remaining;
        struct timespec *wait_timeout = &bounded_timeout;
        if (timeout) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                lock(&poll_->lock);
                list_for_each_entry(&poll_->poll_fds, poll_fd, fds) sockrestart_end_listen_wait(poll_fd->fd);
                break;
            }
            remaining.tv_sec = deadline.tv_sec - now.tv_sec;
            remaining.tv_nsec = deadline.tv_nsec - now.tv_nsec;
            if (remaining.tv_nsec < 0) { remaining.tv_nsec += 1000000000; remaining.tv_sec--; }
            if (remaining.tv_sec < 1) wait_timeout = &remaining;
        }
        current->blocking = true;
        do {
            err = real_poll_wait(&poll_->real, e, sizeof(e)/sizeof(e[0]), wait_timeout);
            saved_errno = errno;  // save immediately before anything clobbers it
        } while (saved_errno == EINTR && sockrestart_should_restart_listen_wait());
        current->blocking = false;
        // Only update last_unblocked_ns when actual events were received.
        // Timeout returns (err==0) don't count as real progress — the poll_wait
        // loop is just cycling. This prevents the deadlock detector from being
        // fooled by idle poll_wait loops during exit cleanup.
        if (err > 0) {
            struct timespec _ts;
            clock_gettime(CLOCK_MONOTONIC, &_ts);
            uint64_t now = (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec;
            current->last_unblocked_ns = now;
            atomic_store_explicit(&current->group->last_progress_ns, now,
                                  memory_order_relaxed);
        }
        // If we timed out from our bounded timeout, loop back to re-check
        // fd readiness and signal pending.
        if (err == 0 && wait_timeout == &bounded_timeout) {
            lock(&poll_->lock);
            list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
                sockrestart_end_listen_wait(poll_fd->fd);
            }
            // Check for group exit so blocking threads unblock promptly
            if (current->group->doing_group_exit) {
                res = _EINTR;
                break;
            }
            // An idle Linux process is not an exited process. Leave group
            // termination to Linux exit/signal semantics, never a poll timeout.
            continue;
        }
        lock(&poll_->lock);
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_end_listen_wait(poll_fd->fd);
        }

        if (err < 0) {
            errno = saved_errno;
            res = errno_map();
            break;
        }
        if (err == 0) continue; // recheck readiness, signals and actual deadline

        poll_record_backend(poll_,e,err);

        poll_drain_notify(poll_);
    }

    poll_->waiters--;

    unlock(&poll_->lock);
    return res;
}

void poll_destroy(struct poll *poll) {
    struct poll_fd *poll_fd;
    struct poll_fd *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        lock(&poll_fd->fd->poll_lock);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->fd->poll_lock);
        free(poll_fd);
    }

    list_for_each_entry_safe(&poll->pollfd_freelist, poll_fd, tmp, fds) {
        list_remove(&poll_fd->fds);
        free(poll_fd);
    }

    owned_close(poll->notify_pipe[0]); owned_close(poll->notify_pipe[1]);
    real_poll_close(&poll->real);
    pthread_mutex_destroy(&poll->lock.m);
    free(poll);
}

// Platform-specific real_poll implementations

#if HAVE_EPOLL

static int real_poll_init(struct real_poll *real) {
    real->fd = epoll_create1(0);
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    int timeout_millis = -1;
    if (timeout != NULL)
        timeout_millis = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
    return epoll_wait(real->fd, (struct epoll_event *) events, max, timeout_millis);
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    // Guest LT/ET/ONESHOT semantics are applied by the readiness scan. The
    // native backend supplies transitions only, so observation can drain it.
    types = (types & ~EPOLLONESHOT) | (types ? EPOLLET : 0);
    if (types == 0)
        return epoll_ctl(real->fd, EPOLL_CTL_DEL, fd, NULL);
    struct epoll_event epevent = {.events = types, .data.ptr = data};
    int err = epoll_ctl(real->fd, EPOLL_CTL_MOD, fd, &epevent);
    if (err < 0 && errno == ENOENT)
        err = epoll_ctl(real->fd, EPOLL_CTL_ADD, fd, &epevent);
    return err;
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.data.ptr;
}
static int rpe_events(struct real_poll_event *rpe) {
    return rpe->real.events;
}

#elif HAVE_KQUEUE

static int real_poll_init(struct real_poll *real) {
    real->fd = kqueue();
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    struct kevent e[3] = {
        {.filter = EVFILT_READ, .flags = types & (POLL_READ | POLL_HUP) ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_WRITE, .flags = types & POLL_WRITE ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_EXCEPT, .flags = types & POLL_ERR ? EV_ADD : EV_DELETE},
    };
    if (!(types & POLL_READ) && types & POLL_HUP) {
        // Set the low water mark really high so we'll only get woken up on a hangup
        e[0].fflags = NOTE_LOWAT;
        e[0].data = INT_MAX;
    }
    for (int i = 0; i < 3; i++) {
        e[i].ident = fd;
        e[i].udata = data;
        e[i].flags |= EV_RECEIPT;
        // Readiness is level-checked in userspace; native events only wake us.
        e[i].flags |= EV_CLEAR;
    }

    return kevent(real->fd, e, 3, e, 3, NULL);
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    return kevent(real->fd, NULL, 0, (struct kevent *) events, max, timeout);
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.udata;
}
static int rpe_events(struct real_poll_event *rpe) {
    if (rpe->real.filter == EVFILT_READ) {
        int events = 0;
        if (rpe->real.data > 0)
            events |= POLL_READ;
        if (rpe->real.flags & EV_EOF)
            events |= POLL_HUP;
        return events;
    }
    if (rpe->real.filter == EVFILT_WRITE) return POLL_WRITE;
    if (rpe->real.filter == EVFILT_EXCEPT) return POLL_ERR;
    return 0;
}

#endif

static void real_poll_close(struct real_poll *real) {
    owned_close(real->fd);
}

