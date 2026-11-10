#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <limits.h>
#include "fs/real.h"
#include "kernel/signal.h"
extern const struct fd_ops socket_fdops;
#include "debug.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/errqueue.h"
#include "kernel/calls.h"
extern volatile bool g_trace_syscalls;

// Linux poll/select do not allocate a descriptor. In particular they must
// still observe existing sockets when the shared Darwin FD budget is full.
// Retained descriptions keep native handles alive across concurrent close.
static int native_poll_set(struct pollfd_ *set, struct fd **files, unsigned count,
                           const struct timespec *timeout, bool *handled) {
    *handled=false;
    for(unsigned i=0;i<count;i++)if((set[i].events & ~(POLL_READ|POLL_WRITE|POLL_PRI|POLL_ERR|POLL_HUP|POLL_NVAL)) || (files[i] &&
        files[i]->ops->poll!=realfs_poll && files[i]->ops!=&socket_fdops &&
        files[i]->ops!=&epoll_fdops)) return 0;
    *handled=true;
    // Two host slots per entry: the socket and its IP_RECVERR ICMP companion.
    struct pollfd local[128], *host=count<=64?local:calloc(count*2,sizeof(*host));
    if(!host)return _ENOMEM;
    struct timespec begin;clock_gettime(CLOCK_MONOTONIC,&begin);
    int result=0;
    for(;;){
        result=0;unsigned host_count=0;
        for(unsigned i=0;i<count;i++){
            int ready=files[i]?files[i]->ops->poll(files[i]):(set[i].fd<0?0:POLL_NVAL);
            set[i].revents=ready & (set[i].events|POLL_ERR|POLL_HUP|POLL_NVAL);
            result+=set[i].revents!=0;
            if(files[i]){
                unsigned slot=0;while(slot<host_count && host[slot].fd!=files[i]->real_fd)slot++;
                if(slot==host_count)host[host_count++]=(struct pollfd){.fd=files[i]->real_fd};
                host[slot].events|=((set[i].events&POLL_READ)?POLLIN:0)|((set[i].events&POLL_WRITE)?POLLOUT:0)|((set[i].events&POLL_PRI)?POLLPRI:0);
                // Linux reports POLLERR unrequested: wake for queued ICMP errors too.
                int companion=ip_errqueue_host_fd(files[i]);
                if(companion>=0){
                    unsigned c=0;while(c<host_count && host[c].fd!=companion)c++;
                    if(c==host_count)host[host_count++]=(struct pollfd){.fd=companion};
                    host[c].events|=POLLIN;
                }
            }
        }
        if(result)break;
        if(current->group->doing_group_exit){result=_EINTR;break;}
        lock(&current->sighand->lock);bool pending=!!(current->pending&~current->blocked);unlock(&current->sighand->lock);
        if(pending){result=_EINTR;break;}
        int milliseconds=1000;
        if(timeout){
            struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
            long double remaining=(long double)timeout->tv_sec*1000+timeout->tv_nsec/1e6L-
                ((long double)(now.tv_sec-begin.tv_sec)*1000+(now.tv_nsec-begin.tv_nsec)/1e6L);
            if(remaining<=0){result=0;break;}
            if(remaining<1000)milliseconds=(int)remaining+(remaining>(int)remaining);
        }
        for(unsigned i=0;i<count;i++)if(files[i])sockrestart_begin_listen_wait(files[i]);
        current->blocking=true;int n=poll(host,host_count,milliseconds);int error=errno;
        current->blocking=false;
        bool restart=n<0&&error==EINTR&&sockrestart_should_restart_listen_wait();
        for(unsigned i=0;i<count;i++)if(files[i])sockrestart_end_listen_wait(files[i]);
        if(n<0&&!restart){errno=error;result=errno_map();break;}
        // Native events only wake this loop. The description's poll method
        // remains authoritative for Linux half-close and nested epoll state.
    }
    if(result > 0) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
        uint64_t ns=(uint64_t)now.tv_sec*1000000000ULL+now.tv_nsec;
        current->last_unblocked_ns=ns;
        atomic_store_explicit(&current->group->last_progress_ns,ns,memory_order_relaxed);
    }
    if(host!=local)free(host);
    return result;
}

static int user_read_or_zero(addr_t addr, void *data, size_t size) {
    if (addr == 0)
        memset(data, 0, size);
    else if (user_read(addr, data, size))
        return _EFAULT;
    return 0;
}

#define SELECT_READ (POLL_READ | POLL_HUP | POLL_ERR)
#define SELECT_WRITE (POLL_WRITE | POLL_ERR)
#define SELECT_EX (POLL_PRI)
struct select_context {
    char *readfds;
    char *writefds;
    char *exceptfds;
};
static int select_event_callback(void *context, int types, union poll_fd_info info) {
    struct select_context *c = context;
    if (types & SELECT_READ)
        bit_set(info.fd, c->readfds);
    if (types & SELECT_WRITE)
        bit_set(info.fd, c->writefds);
    if (types & SELECT_EX)
        bit_set(info.fd, c->exceptfds);
    if (!(types & (SELECT_READ | SELECT_WRITE | SELECT_EX)))
        return 0;
    return 1;
}

static dword_t select_common(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, struct timespec *timeout, const char *name) {
    (void) name;
    if (nfds < 0 || (rlim_t_) nfds > rlimit(RLIMIT_NOFILE_)) return _EINVAL;
    if (timeout && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000)) return _EINVAL;
    size_t size = BITS_SIZE(nfds);
    char *sets = calloc(3, size ? size : 1);
    struct fd **files = calloc(nfds ? nfds : 1, sizeof(*files));
    if (!sets || !files) { free(sets); free(files); return _ENOMEM; }
    char *readfds = sets, *writefds = sets + size, *exceptfds = sets + 2 * size;
    int result = _EFAULT;
    struct poll *poll = NULL;
    if (user_read_or_zero(readfds_addr, readfds, size) ||
        user_read_or_zero(writefds_addr, writefds, size) ||
        user_read_or_zero(exceptfds_addr, exceptfds, size)) goto out;
    for (fd_t i = 0; i < nfds; i++) {
        int events = (bit_test(i, readfds) ? SELECT_READ : 0) |
            (bit_test(i, writefds) ? SELECT_WRITE : 0) |
            (bit_test(i, exceptfds) ? SELECT_EX : 0);
        if (!events) continue;
        files[i] = f_get_retain(i);
        if (!files[i]) { result = _EBADF; goto out; }
    }
    struct pollfd_ local[64], *native_set=nfds<=64?local:calloc(nfds,sizeof(*native_set));
    if(!native_set){result=_ENOMEM;goto out;}
    for(fd_t i=0;i<nfds;i++)native_set[i]=(struct pollfd_){.fd=files[i]?i:-1,
        .events=(bit_test(i,readfds)?POLL_READ:0)|(bit_test(i,writefds)?POLL_WRITE:0)|(bit_test(i,exceptfds)?POLL_PRI:0)};
    bool handled=false;result=native_poll_set(native_set,files,nfds,timeout,&handled);
    if(handled){
        if(result>=0){result=0;
            for(fd_t i=0;i<nfds;i++){
                int ready=native_set[i].revents;
                if(ready&POLL_NVAL){result=_EBADF;break;}
                if(!(ready&SELECT_READ))bit_clear(i,readfds);
                if(!(ready&SELECT_WRITE))bit_clear(i,writefds);
                if(!(ready&SELECT_EX))bit_clear(i,exceptfds);
                result+=!!bit_test(i,readfds)+!!bit_test(i,writefds)+!!bit_test(i,exceptfds);
            }
            if(result>=0 && ((readfds_addr&&user_write(readfds_addr,readfds,size))||
               (writefds_addr&&user_write(writefds_addr,writefds,size))||
               (exceptfds_addr&&user_write(exceptfds_addr,exceptfds,size))))result=_EFAULT;
        }
        if(native_set!=local)free(native_set);goto out;
    }
    if(native_set!=local)free(native_set);
    poll=poll_create();if(IS_ERR(poll)){result=PTR_ERR(poll);poll=NULL;goto out;}
    for(fd_t i=0;i<nfds;i++)if(files[i]){
        int events=(bit_test(i,readfds)?SELECT_READ:0)|(bit_test(i,writefds)?SELECT_WRITE:0)|(bit_test(i,exceptfds)?SELECT_EX:0);
        result=poll_add_fd(poll,files[i],i,events,(union poll_fd_info)i);if(result<0)goto out;
    }
    // Keep requested sets separately: HUP/ERR must not set unrequested bits.
    char *requested = malloc(3 * (size ? size : 1));
    if (!requested) { result = _ENOMEM; goto out; }
    memcpy(requested, sets, 3 * size);
    memset(sets, 0, 3 * size);
    struct select_context context = {readfds, writefds, exceptfds};
    result = poll_wait(poll, select_event_callback, &context, timeout);
    if (result >= 0) {
        result = 0;
        for (size_t i = 0; i < 3 * size; i++) sets[i] &= requested[i];
        for (fd_t i = 0; i < nfds; i++)
            result += !!bit_test(i, readfds) + !!bit_test(i, writefds) + !!bit_test(i, exceptfds);
        if ((readfds_addr && user_write(readfds_addr, readfds, size)) ||
            (writefds_addr && user_write(writefds_addr, writefds, size)) ||
            (exceptfds_addr && user_write(exceptfds_addr, exceptfds, size))) result = _EFAULT;
    }
    free(requested);
out:
    if (poll) poll_destroy(poll);
    for (fd_t i = 0; i < nfds; i++) if (files[i]) fd_close(files[i]);
    free(files); free(sets);
    return result;
}

dword_t sys_select(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr) {
    struct timespec timeout_ts = {};
    struct timespec *timeout_ts_addr = NULL;
    if (timeout_addr != 0) {
        struct timeval_ timeout_timeval;
        if (user_get(timeout_addr, timeout_timeval))
            return _EFAULT;
        timeout_ts = convert_timeval(timeout_timeval);
        timeout_ts_addr = &timeout_ts;
    }
    // XXX we do not implement the Linux behavior of writing the timeout with remaining time here.
    return select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_addr, "select");
}

struct poll_context {
    struct pollfd_ *polls;
    struct fd **files;
    int nfds;
};
#define POLL_ALWAYS_LISTENING (POLL_ERR|POLL_HUP|POLL_NVAL)
static int poll_event_callback(void *context, int types, union poll_fd_info info) {
    struct poll_context *c = context;
    struct pollfd_ *polls = c->polls;
    int nfds = c->nfds;
    int res = 0;
    for (int i = 0; i < nfds; i++) {
        if (c->files[i] == info.ptr) {
            polls[i].revents = types & (polls[i].events | POLL_ALWAYS_LISTENING);
            res = 1;
        }
    }
    return res;
}
static dword_t poll_common(addr_t fds, dword_t nfds, struct timespec *timeout) {
    if (nfds > rlimit(RLIMIT_NOFILE_)) return _EINVAL;
    // Normal GUI calls stay on a small fixed stack; guest-sized arrays never do.
    struct pollfd_ local_polls[64];
    struct fd *local_files[64];
    struct pollfd_ *polls = nfds <= 64 ? local_polls : calloc(nfds, sizeof(*polls));
    struct fd **files = nfds <= 64 ? local_files : calloc(nfds, sizeof(*files));
    if (!polls || !files) { if (nfds > 64) { free(polls); free(files); } return _ENOMEM; }
    memset(files, 0, nfds * sizeof(*files));
    int result = _EFAULT;
    struct poll *poll = NULL;
    if (nfds && user_read(fds, polls, nfds * sizeof(*polls))) goto out;
    bool invalid = false;
    for (unsigned i = 0; i < nfds; i++) {
        files[i] = f_get_retain(polls[i].fd);
        polls[i].revents = 0;
        if (polls[i].fd >= 0 && !files[i]) invalid = true;
    }
    bool native=false;
    result=native_poll_set(polls,files,nfds,timeout,&native);
    if(native){if(result>=0 && nfds && user_write(fds,polls,nfds*sizeof(*polls)))result=_EFAULT;goto out;}
    poll = poll_create();
    if (IS_ERR(poll)) { result = PTR_ERR(poll); poll = NULL; goto out; }
    for (unsigned i = 0; i < nfds; i++) {
        if (!files[i] || polls[i].revents) continue;
        int events = polls[i].events;
        polls[i].revents = 1;
        for (unsigned j = i + 1; j < nfds; j++) {
            if (files[i] == files[j]) {
                events |= polls[j].events;
                polls[j].revents = 1;
            }
        }
        result = poll_add_fd(poll, files[i], polls[i].fd, events | POLL_ALWAYS_LISTENING,
            (union poll_fd_info) (void *) files[i]);
        if (result < 0) goto out;
    }
    for (unsigned i = 0; i < nfds; i++)
        polls[i].revents = polls[i].fd >= 0 && !files[i] ? POLL_NVAL : 0;
    struct poll_context context = {polls, files, nfds};
    struct timespec immediate = {0};
    result = poll_wait(poll, poll_event_callback, &context, invalid ? &immediate : timeout);
    if (result >= 0) {
        // Count ready array entries, including duplicates and POLLNVAL.
        result = 0;
        for (unsigned i = 0; i < nfds; i++) result += polls[i].revents != 0;
        // Explicit, bounded numeric diagnostics for broker/lifeline waits.
        // Disabled on the normal path; no buffers, strings or input are logged.
        static _Thread_local unsigned trace_ready;
        if (g_trace_syscalls && nfds == 2 && result > 0 && trace_ready++ < 16)
            printk("POLL_READY tid=%d fd0=%d events0=%x returned0=%x fd1=%d events1=%x returned1=%x\n",
                current->pid, polls[0].fd, polls[0].events, polls[0].revents,
                polls[1].fd, polls[1].events, polls[1].revents);
        if (nfds && user_write(fds, polls, nfds * sizeof(*polls))) result = _EFAULT;
    }
out:
    if (poll) poll_destroy(poll);
    for (unsigned i = 0; i < nfds; i++) if (files[i]) fd_close(files[i]);
    if (nfds > 64) { free(polls); free(files); }
    return result;
}

dword_t sys_poll(addr_t fds, dword_t nfds, int_t timeout) {
    struct timespec ts = {0};
    if (timeout >= 0) { ts.tv_sec = timeout / 1000; ts.tv_nsec = (timeout % 1000) * 1000000; }
    return poll_common(fds, nfds, timeout < 0 ? NULL : &ts);
}

dword_t sys_pselect(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr) {
    struct timespec_ timeout_timespec;
    struct timespec timeout_ts;
    struct timespec *timeout_ts_addr = NULL;
    if (timeout_addr != 0) {
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout_ts = convert_timespec(timeout_timespec);
        timeout_ts_addr = &timeout_ts;
    }
    // a system call can only take 6 parameters, so the last two need to be passed as a pointer to a struct
    struct {
        addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {0};
#ifdef GUEST_ARM64
    struct {
        uint64_t mask_addr;
        uint64_t mask_size;
    } sigmask64 = {0};
    if (sigmask_addr && user_get(sigmask_addr, sigmask64))
        return _EFAULT;
    sigmask.mask_addr = (addr_t) sigmask64.mask_addr;
    sigmask.mask_size = (dword_t) sigmask64.mask_size;
#else
    if (sigmask_addr && user_get(sigmask_addr, sigmask))
        return _EFAULT;
#endif
    sigset_t_ mask;

    if (sigmask.mask_addr != 0) {
        int err = user_get_sigset(sigmask.mask_addr, sigmask.mask_size, &mask);
        if (err)
            return err;
        sigmask_set_temp(mask);
    }
    return select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_addr, "pselect");
}

dword_t sys_ppoll(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    struct timespec timeout;
    struct timespec *timeout_ptr = NULL;
    if (timeout_addr) {
        struct timespec_ value;
        if (user_get(timeout_addr, value)) return _EFAULT;
        if (value.sec < 0 || value.nsec < 0 || value.nsec >= 1000000000) return _EINVAL;
        timeout = convert_timespec(value);
        timeout_ptr = &timeout;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        int err = user_get_sigset(sigmask_addr, sigsetsize, &mask);
        if (err)
            return err;
        sigmask_set_temp(mask);
    }

    return poll_common(fds, nfds, timeout_ptr);
}
