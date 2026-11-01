#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <pthread.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/real.h"
#include "fs/sock.h"
#include <sys/ioctl.h>
#undef fd_create
extern const struct fd_ops socket_fdops;

static atomic_uint live_descriptions;

// Fixed-size origin registry: no per-descriptor allocation and no fd layout
// change (the GPU tools mirror that private layout). Dropped origins are counted.
struct fd_origin { struct fd *fd; const char *site; uint64_t serial; int tid, tgid; };
static struct fd_origin origins[4096];
static lock_t origins_lock = LOCK_INITIALIZER;
static uint64_t origin_serial;
static atomic_uint created_descriptions, destroyed_descriptions, dropped_origins;
static unsigned origin_slot(struct fd *fd) { return ((uintptr_t)fd >> 4) % 4096; }
static void origin_create(struct fd *fd, const char *site) {
    lock(&origins_lock);
    unsigned start=origin_slot(fd), slot=start;
    do {
        if (!origins[slot].fd) {
            origins[slot]=(struct fd_origin){fd,site,++origin_serial,current?current->pid:0,current?current->tgid:0};
            unlock(&origins_lock); return;
        }
        slot=(slot+1)%4096;
    } while(slot!=start);
    atomic_fetch_add_explicit(&dropped_origins,1,memory_order_relaxed);
    unlock(&origins_lock);
}
static struct fd_origin origin_get(struct fd *fd, bool remove) {
    struct fd_origin result={0};
    lock(&origins_lock);
    // A removed slot cannot terminate a lookup: slots are reused independently.
    unsigned start=origin_slot(fd), slot=start;
    do {
        if (origins[slot].fd==fd) { result=origins[slot]; if(remove)origins[slot].fd=NULL; break; }
        slot=(slot+1)%4096;
    } while(slot!=start);
    unlock(&origins_lock); return result;
}
// Failure-only observation of a pinned description. No new descriptor, mapping
// or layout field is introduced; serial/inode correlate aliases across IPC.
void fd_report_mapping_failure(struct fd *fd, int guest_fd, const char *stage,
        uint64_t address, uint64_t length, uint64_t offset, unsigned prot,
        unsigned flags, int native_error, int linux_result) {
    int saved = errno;
    struct stat st = {0};
    struct fd_origin origin = fd ? origin_get(fd, false) : (struct fd_origin){0};
    int stat_ok = fd && fd->real_fd >= 0 && fstat(fd->real_fd, &st) == 0;
    char line[640];
    snprintf(line, sizeof line, "MMAP_FAILURE tid=%d tgid=%d stage=%s guest_fd=%d host_fd=%d serial=%llu creator_tgid=%d memfd=%d refs=%u inode=%llu file_bytes=%lld stat_ok=%d address=%#llx length=%llu offset=%llu host_page=%zu correction=%llu prot=%#x page_flags=%#x flags=%#x native_errno=%d linux_result=%d",
            current->pid, current->tgid, stage, guest_fd, fd ? fd->real_fd : -1,
            (unsigned long long)origin.serial, origin.tgid, fd && fd->memfd,
            fd ? atomic_load(&fd->refcount) : 0, (unsigned long long)st.st_ino,
            (long long)st.st_size, stat_ok, (unsigned long long)address,
            (unsigned long long)length, (unsigned long long)offset,
            (size_t)real_page_size, (unsigned long long)(offset % real_page_size),
            prot & P_RWX, prot, flags, native_error, linux_result);
    runtime_record_mapping_failure(line);
    errno = saved;
}

// ---- Host descriptor limit ---------------------------------------------------
// Every guest process shares this host process's descriptor table, so the host
// soft limit bounds all guests together (iOS starts apps at 2560). Raise it once
// at startup to a fixed target, never above the hard limit or
// kern.maxfilesperproc, never lowering it. The guest RLIMIT_NOFILE is separate
// (kernel/init.c) and unchanged. This adds headroom only: the descriptor census
// (/proc/ish/fds) still reports the real usage, and retention bugs must be fixed
// at their source. ISH_HOST_NOFILE=0 keeps the inherited limit; a number sets
// the target for measurements.
static struct host_nofile_state host_nofile;
static pthread_once_t host_nofile_once = PTHREAD_ONCE_INIT;
static void host_nofile_raise_once(void) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) { host_nofile.error = errno; return; }
    host_nofile.initial_soft = host_nofile.soft = limit.rlim_cur;
    host_nofile.hard = limit.rlim_max;
    rlim_t target = 8192;
    const char *setting = getenv("ISH_HOST_NOFILE");
    if (setting && *setting) {
        char *end; unsigned long long value = strtoull(setting, &end, 10);
        if (*end == '\0') target = (rlim_t) value;
    }
    host_nofile.target = target;
    if (target == 0) { host_nofile.disabled = true; return; }
    rlim_t cap = limit.rlim_max;
#ifdef __APPLE__
    int per_process = 0; size_t size = sizeof(per_process);
    if (sysctlbyname("kern.maxfilesperproc", &per_process, &size, NULL, 0) == 0 && per_process > 0) {
        host_nofile.per_process = (rlim_t) per_process;
        if ((rlim_t) per_process < cap) cap = (rlim_t) per_process;
    }
#endif
    // Try the target, then 4096, each capped; keep the first that verifies.
    rlim_t candidates[] = {target, 4096};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        rlim_t want = candidates[i] < cap ? candidates[i] : cap;
        if (want <= limit.rlim_cur) continue;
        struct rlimit raised = {want, limit.rlim_max};
        if (setrlimit(RLIMIT_NOFILE, &raised) != 0) { host_nofile.error = errno; continue; }
        struct rlimit check;
        if (getrlimit(RLIMIT_NOFILE, &check) == 0) host_nofile.soft = check.rlim_cur;
        host_nofile.error = 0;
        break;
    }
}
const struct host_nofile_state *host_nofile_raise(void) {
    pthread_once(&host_nofile_once, host_nofile_raise_once);
    return &host_nofile;
}
const char *host_nofile_describe(char *buf, size_t size) {
    const struct host_nofile_state *state = host_nofile_raise();
    snprintf(buf, size, "host_nofile_initial=%llu host_nofile_soft=%llu host_nofile_hard=%llu host_nofile_target=%llu host_maxfilesperproc=%llu host_nofile_raise=%s host_nofile_errno=%d",
        (unsigned long long) state->initial_soft, (unsigned long long) state->soft, (unsigned long long) state->hard,
        (unsigned long long) state->target, (unsigned long long) state->per_process,
        state->disabled ? "disabled" : state->soft > state->initial_soft ? "raised" : state->error ? "failed" : "unchanged",
        state->error);
    return buf;
}

// ---- Mapping identities ----------------------------------------------------
// A file mapping must keep its file's identity (name for /proc/<pid>/maps and
// crash reports, inode/dev/ino for shared futex keys, CoW provenance) but, as on
// Linux, it must not keep a descriptor open. realfs/fakefs map eagerly through a
// host mmap or copy, so after mmap the host descriptor is unused. Retaining the
// guest's description pinned one host fd per mapped file per process (~120 per
// Chromium process), exhausting the iOS soft limit (2560) during VS Code use.
// The identity owns no host fd. It keeps a mount reference (a mapped file keeps
// its mount busy, as on Linux) and the mount-relative path, so diagnostics that
// read the file (JIT build IDs, crash symbolization) reopen it on demand and
// verify dev/ino. memfd mappings keep the original description (their
// reopen/seal semantics live on it).
struct mapping_identity { struct mount *mount; char *relpath; char *path; dev_t dev; ino_t ino; bool have_stat; };
static int mapping_identity_close(struct fd *fd) {
    struct mapping_identity *identity = fd->data;
    if (identity) {
        if (identity->mount) mount_release(identity->mount);
        free(identity->relpath); free(identity->path); free(identity);
    }
    fd->data = NULL;
    return 0;
}
static ssize_t mapping_identity_pread(struct fd *fd, void *buf, size_t size, off_t off) {
    struct mapping_identity *identity = fd->data;
    if (!identity || !identity->mount || !identity->relpath || !identity->have_stat) return _EBADF;
    mount_retain(identity->mount);
    struct fd *file = identity->mount->fs->open(identity->mount, identity->relpath, O_RDONLY_, 0);
    if (IS_ERR(file)) { mount_release(identity->mount); return PTR_ERR(file); }
    file->mount = identity->mount;
    struct stat st;
    ssize_t result = _ESTALE;
    if (file->real_fd >= 0 && fstat(file->real_fd, &st) == 0 && st.st_dev == identity->dev && st.st_ino == identity->ino)
        result = file->ops->pread ? file->ops->pread(file, buf, size, off) : _EBADF;
    fd_close(file);
    return result;
}
const struct fd_ops mapping_identity_fdops = { .close = mapping_identity_close, .pread = mapping_identity_pread };
struct fd *fd_mapping_identity(struct fd *source) {
    if (source->memfd || source->ops == &mapping_identity_fdops || source->real_fd < 0 || source->mount == NULL)
        return fd_retain(source);
    struct mapping_identity *identity = calloc(1, sizeof(*identity));
    char *path = malloc(MAX_PATH);
    if (identity == NULL || path == NULL) { free(identity); free(path); return fd_retain(source); }
    if (source->mount->fs->getpath(source, path) == 0) identity->relpath = strdup(path);
    if (generic_getpath(source, path) == 0) identity->path = strdup(path);
    free(path);
    struct stat st;
    if (fstat(source->real_fd, &st) == 0) { identity->dev = st.st_dev; identity->ino = st.st_ino; identity->have_stat = true; }
    struct fd *fd = fd_create_at(&mapping_identity_fdops, "mapping_identity");
    if (fd == NULL) { free(identity->relpath); free(identity->path); free(identity); return fd_retain(source); }
    mount_retain(source->mount);
    identity->mount = source->mount;
    fd->data = identity;
    fd->type = source->type;
    fd->flags = source->flags;
    fd->real_fd = -1;
    if (source->inode) { fd->inode = source->inode; inode_retain(fd->inode); }
    return fd;
}
bool fd_is_mapping_identity(struct fd *fd) { return fd->ops == &mapping_identity_fdops; }
int fd_mapping_identity_path(struct fd *fd, char *buf) {
    struct mapping_identity *identity = fd->data;
    if (!identity || !identity->path) return _ENOENT;
    strcpy(buf, identity->path);
    return 0;
}
bool fd_mapping_identity_stat(struct fd *fd, dev_t *dev, ino_t *ino) {
    struct mapping_identity *identity = fd->data;
    if (!identity || !identity->have_stat) return false;
    *dev = identity->dev; *ino = identity->ino;
    return true;
}

struct fd *fd_create(const struct fd_ops *ops) { return fd_create_at(ops,"external"); }
struct fd *fd_create_at(const struct fd_ops *ops, const char *site) {
    struct fd *fd = malloc(sizeof(struct fd));
    if (fd == NULL)
        return NULL;
    *fd = (struct fd) {};
    fd->ops = ops;
    fd->refcount = 1;
    atomic_fetch_add_explicit(&live_descriptions, 1, memory_order_relaxed);
    fd->flags = 0;
    fd->mount = NULL;
    fd->offset = 0;
    list_init(&fd->poll_fds);
    list_init(&fd->pidfd_links);
    lock_init(&fd->poll_lock);
    lock_init(&fd->lock);
    cond_init(&fd->cond);
    origin_create(fd,site);
    atomic_fetch_add_explicit(&created_descriptions,1,memory_order_relaxed);
    return fd;
}

// The backing open failed before real_fd/inode/mount ownership was attached.
// Do not call fd_close: its backend would close the zero-initialized host fd.
void fd_discard_unopened(struct fd *fd) {
    assert(atomic_load(&fd->refcount) == 1 && !fd->mount && !fd->inode);
    cond_destroy(&fd->cond);
    pthread_mutex_destroy(&fd->lock.m);
    pthread_mutex_destroy(&fd->poll_lock.m);
    origin_get(fd,true);
    atomic_fetch_add_explicit(&destroyed_descriptions,1,memory_order_relaxed);
    atomic_fetch_sub_explicit(&live_descriptions, 1, memory_order_relaxed);
    free(fd);
}

struct fd *fd_retain(struct fd *fd) {
    fd->refcount++;
    return fd;
}

int fd_close(struct fd *fd) {
    int err = 0;
    if (--fd->refcount == 0) {
        if (fd->inode) file_lock_remove_owned_by(fd, fd);
        poll_cleanup_fd(fd);
        if (fd->ops->close)
            err = fd->ops->close(fd);
        // see comment in close in kernel/fs.h
        if (fd->mount && fd->mount->fs->close && fd->mount->fs->close != fd->ops->close) {
            int new_err = fd->mount->fs->close(fd);
            if (new_err < 0)
                err = new_err;
        }

        if (fd->inode)
            inode_release(fd->inode);
        if (fd->mount)
            mount_release(fd->mount);
        if (fd->memfd && atomic_fetch_sub(&fd->memfd->descriptions, 1) == 1) {
            if (close(fd->memfd->backing_fd) < 0 && !err) err = errno_map();
            pthread_mutex_destroy(&fd->memfd->lock.m);
            free(fd->memfd);
        }
        origin_get(fd,true);
        atomic_fetch_add_explicit(&destroyed_descriptions,1,memory_order_relaxed);
        atomic_fetch_sub_explicit(&live_descriptions, 1, memory_order_relaxed);
        free(fd);
    }
    return err;
}

// Error-path only: bounded numeric accounting, no guest paths or contents.
// All guest processes share one host descriptor budget. Do not equate the
// process-local guest table size with available host descriptors.
static void fd_snapshot(unsigned syscall, int result, const char *kind) {
    unsigned count=0, sockets=0, pipes=0, epolls=0, memfds=0, cloexec=0, shared=0;
    struct fdtable *table = current->files;
    lock(&table->lock);
    for (unsigned i=0; i<table->size; i++) {
        struct fd *fd=table->files[i];
        if (!fd) continue;
        count++; sockets += fd->ops == &socket_fdops || fd->type == S_IFSOCK; pipes += fd->type == S_IFIFO;
        epolls += fd->ops == &epoll_fdops; memfds += fd->memfd != NULL;
        cloexec += !!bit_test(i, table->cloexec);
        shared += atomic_load_explicit(&fd->refcount, memory_order_relaxed) > 1;
    }
    unsigned owners = atomic_load_explicit(&table->refcount, memory_order_relaxed);
    extern void (*runtime_fault_context)(const char *);
    if (runtime_fault_context) {
        unsigned emitted=0;
        for(unsigned i=0;i<table->size && emitted<32;i++) {
            struct fd *fd=table->files[i]; if(!fd)continue;
            struct fd_origin origin=origin_get(fd,false);
            bool native=fd->ops->poll==realfs_poll || fd->ops==&socket_fdops || fd->ops==&epoll_fdops;
            char row[320]; snprintf(row,sizeof row,"FD_LIVE tid=%d tgid=%d guest_fd=%u host_fd=%d refs=%u serial=%llu creator_tid=%d creator_tgid=%d site=%s type=%#o cloexec=%d",current->pid,current->tgid,i,native?fd->real_fd:-1,atomic_load(&fd->refcount),(unsigned long long)origin.serial,origin.tid,origin.tgid,origin.site?origin.site:"untracked",fd->type,!!bit_test(i,table->cloexec));
            runtime_fault_context(row); emitted++;
        }
    }
    unlock(&table->lock);
    struct rlimit host={0}; getrlimit(RLIMIT_NOFILE, &host);
    unsigned host_count=0; rlim_t bound=host.rlim_cur;
    if (bound>65536) bound=65536;
    for (rlim_t i=0; i<bound; i++) if (fcntl((int)i,F_GETFD)>=0) host_count++;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    char line[1024];
    snprintf(line, sizeof line, "%s tid=%d tgid=%d nr=%u result=%d mono_s=%lld guest_open=%u guest_limit=%llu table_owners=%u sockets=%u pipes=%u epoll=%u memfd=%u cloexec=%u shared_refs=%u descriptions_all=%u host_open=%u host_scan_bound=%llu host_soft=%llu host_hard=%llu created=%u destroyed=%u dropped_origins=%u",
        kind, current->pid,current->tgid,syscall,result,(long long)now.tv_sec,count,
        (unsigned long long)rlimit(RLIMIT_NOFILE_),owners,sockets,pipes,epolls,memfds,cloexec,shared,
        atomic_load_explicit(&live_descriptions,memory_order_relaxed),host_count,
        (unsigned long long)bound,(unsigned long long)host.rlim_cur,(unsigned long long)host.rlim_max,atomic_load(&created_descriptions),atomic_load(&destroyed_descriptions),atomic_load(&dropped_origins));
    printk("%s\n",line);
    if (runtime_fault_context) runtime_fault_context(line);
}

void fd_diagnostic_snapshot(void) { fd_snapshot(0, 0, "FD_SNAPSHOT"); }
void fd_exhaustion_report(unsigned syscall, int result) {
    static atomic_uint reports;
    if (atomic_fetch_add_explicit(&reports, 1, memory_order_relaxed) < 16)
        fd_snapshot(syscall, result, "FD_PRESSURE");
}

// Explicit diagnostic only, never a polling scheduler. Pin tables while taking
// the task snapshot; release pids_lock before table scans or output callbacks.
void fd_global_census(void (*emit)(const char *,void *),void *context) {
    struct owner { struct fdtable *table; int tgid; } owners[256];unsigned used=0,omitted=0,unix_reported=0;
    lock(&pids_lock);
    for(unsigned pid=1;pid<MAX_PID;pid++) {
        struct task *task=pid_get_task(pid);if(!task||task->exiting||!task->group)continue;
        lock(&task->general_lock);struct fdtable *table=task->files;
        unsigned i=0;while(i<used && !(owners[i].table==table && owners[i].tgid==task->tgid))i++;
        if(table && i==used) {
            if(used<256){atomic_fetch_add(&table->refcount,1);owners[used++]=(struct owner){table,task->tgid};}
            else omitted++;
        }
        unlock(&task->general_lock);
    }
    unlock(&pids_lock);
    for(unsigned i=0;i<used;i++) {
        struct fdtable *table=owners[i].table;unsigned slots=0,sockets=0,pipes=0,epolls=0,regular=0,memfds=0;
        // Unix sockets are pinned here and inspected after the table lock is
        // dropped, so fd->lock/poll_lock are never nested under table->lock.
        struct { struct fd *fd; fd_t number; } unix_fds[64];unsigned unix_used=0;
        lock(&table->lock);
        for(unsigned j=0;j<table->size;j++)if(table->files[j]) {
            struct fd *fd=table->files[j];slots++;sockets+=fd->ops==&socket_fdops || fd->type==S_IFSOCK;pipes+=fd->type==S_IFIFO;
            epolls+=fd->ops==&epoll_fdops;regular+=fd->type==S_IFREG;memfds+=fd->memfd!=NULL;
            if(fd->ops==&socket_fdops && fd->socket.domain==AF_LOCAL_ && unix_used<64)
                unix_fds[unix_used++]=(typeof(unix_fds[0])){fd_retain(fd),(fd_t)j};
        }
        unlock(&table->lock);
        char line[320];snprintf(line,sizeof line,"FD_OWNER tgid=%d slots=%u sockets=%u pipes=%u epoll=%u regular=%u memfd=%u table_refs_including_snapshot=%u",owners[i].tgid,slots,sockets,pipes,epolls,regular,memfds,atomic_load(&table->refcount));
        emit(line,context);fdtable_release(table);
        // Only sockets holding undelivered host bytes or queued SCM batches are
        // reported; an idle reader beside a nonempty queue is a wakeup question.
        for(unsigned j=0;j<unix_used;j++) {
            struct fd *fd=unix_fds[j].fd;int bytes=-1;unsigned scm=0,watchers=0;
            if(ioctl(fd->real_fd,FIONREAD,&bytes)<0)bytes=-errno;
            lock(&fd->lock);struct scm *queued;list_for_each_entry(&fd->socket.unix_scm,queued,queue)scm++;unlock(&fd->lock);
            lock(&fd->poll_lock);struct list *item;list_for_each(&fd->poll_fds,item)watchers++;unlock(&fd->poll_lock);
            if((bytes!=0||scm) && unix_reported++<128) {
                snprintf(line,sizeof line,"FD_UNIX_PENDING tgid=%d fd=%d host_bytes=%d scm_batches=%u poll_registrations=%u",owners[i].tgid,unix_fds[j].number,bytes,scm,watchers);
                emit(line,context);
            }
            fd_close(fd);
        }
    }
    struct group {int creator;const char *site;unsigned count;};
    struct group groups[256]={0};unsigned n=0,extra=0,tracked=0;
    lock(&origins_lock);
    for(unsigned i=0;i<4096;i++)if(origins[i].fd) {
        tracked++;unsigned j=0;
        while(j<n && !(groups[j].creator==origins[i].tgid && groups[j].site==origins[i].site))j++;
        if(j==n){if(n==256){extra++;continue;}groups[n++]=(struct group){origins[i].tgid,origins[i].site,0};}
        groups[j].count++;
    }
    unlock(&origins_lock);
    for(unsigned i=0;i<n;i++){char line[240];snprintf(line,sizeof line,"FD_ORIGIN creator_tgid=%d site=%s descriptions=%u",groups[i].creator,groups[i].site,groups[i].count);emit(line,context);}
    struct rlimit host={0};getrlimit(RLIMIT_NOFILE,&host);unsigned host_open=0;rlim_t bound=host.rlim_cur>65536?65536:host.rlim_cur;
    for(rlim_t f=0;f<bound;f++)if(fcntl((int)f,F_GETFD)>=0)host_open++;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    char limits[320];host_nofile_describe(limits,sizeof limits);
    char line[720];snprintf(line,sizeof line,"FD_TREND mono_ms=%llu host_open=%u host_limit=%llu descriptions=%u created=%u destroyed=%u tracked=%u dropped_origins=%u omitted_owner_rows=%u omitted_origin_descriptions=%u %s",(unsigned long long)now.tv_sec*1000+now.tv_nsec/1000000,host_open,(unsigned long long)host.rlim_cur,atomic_load(&live_descriptions),atomic_load(&created_descriptions),atomic_load(&destroyed_descriptions),tracked,atomic_load(&dropped_origins),omitted,extra,limits);
    emit(line,context);
}

static int fdtable_resize(struct fdtable *table, unsigned size);

struct fdtable *fdtable_new(int size) {
    struct fdtable *fdt = malloc(sizeof(struct fdtable));
    if (fdt == NULL)
        return ERR_PTR(_ENOMEM);
    fdt->refcount = 1;
    fdt->size = 0;
    fdt->files = NULL;
    fdt->cloexec = NULL;
    lock_init(&fdt->lock);
    int err = fdtable_resize(fdt, size);
    if (err < 0) {
        free(fdt);
        return ERR_PTR(err);
    }
    return fdt;
}

static int fdtable_close(struct fdtable *table, fd_t f);

// FIXME this looks like it has the classic refcount UAF
void fdtable_release(struct fdtable *table) {
    lock(&table->lock);
    if (--table->refcount == 0) {
        for (fd_t f = 0; (unsigned) f < table->size; f++)
            fdtable_close(table, f);
        free(table->files);
        free(table->cloexec);
        unlock(&table->lock);
        free(table);
    } else {
        unlock(&table->lock);
    }
}

static int fdtable_resize(struct fdtable *table, unsigned size) {
    // currently the only legitimate use of this is to expand the table
    assert(size > table->size);

    struct fd **files = malloc(sizeof(struct fd *) * size);
    if (files == NULL)
        return _ENOMEM;
    memset(files, 0, sizeof(struct fd *) * size);
    if (table->files)
        memcpy(files, table->files, sizeof(struct fd *) * table->size);

    bits_t *cloexec = malloc(BITS_SIZE(size));
    if (cloexec == NULL) {
        free(files);
        return _ENOMEM;
    }
    memset(cloexec, 0, BITS_SIZE(size));
    if (table->cloexec)
        memcpy(cloexec, table->cloexec, BITS_SIZE(table->size));

    free(table->files);
    table->files = files;
    free(table->cloexec);
    table->cloexec = cloexec;
    table->size = size;
    return 0;
}

struct fdtable *fdtable_copy(struct fdtable *table) {
    lock(&table->lock);
    int size = table->size;
    struct fdtable *new_table = fdtable_new(size);
    if (IS_ERR(new_table)) {
        unlock(&table->lock);
        return new_table;
    }
    memcpy(new_table->files, table->files, sizeof(struct fd *) * size);
    for (fd_t f = 0; f < size; f++)
        if (new_table->files[f])
            new_table->files[f]->refcount++;
    memcpy(new_table->cloexec, table->cloexec, BITS_SIZE(size));
    unlock(&table->lock);
    return new_table;
}

// Detach the calling task's table without duplicating its open descriptions.
// If displaced is supplied, transfer the task's old-table reference to the
// caller so a failed exec can restore sharing. Otherwise release it here.
int fdtable_unshare_current(struct fdtable **displaced) {
    if (displaced) *displaced = NULL;
    struct fdtable *old = current->files;
    if (atomic_load(&old->refcount) == 1) return 0;
    struct fdtable *copy = fdtable_copy(old);
    if (IS_ERR(copy)) return PTR_ERR(copy);
    // Procfs pins a target's table under pids_lock. Publish before releasing
    // the old task reference; never hold the table lock while taking pids_lock.
    lock(&pids_lock);
    current->files = copy;
    unlock(&pids_lock);
    if (displaced) *displaced = old;
    else fdtable_release(old);
    return 0;
}

void fdtable_restore_current(struct fdtable *old) {
    if (!old) return;
    lock(&pids_lock);
    struct fdtable *copy = current->files;
    current->files = old;
    unlock(&pids_lock);
    fdtable_release(copy);
}

static int fdtable_expand(struct fdtable *table, fd_t max) {
    unsigned size = max + 1;
    if (size > rlimit(RLIMIT_NOFILE_))
        return _EMFILE;
    if (table->size >= size)
        return 0;
    return fdtable_resize(table, max + 1);
}

struct fd *fdtable_get(struct fdtable *table, fd_t f) {
    if (f < 0 || (unsigned) f >= table->size)
        return NULL;
    return table->files[f];
}

struct fd *f_get(fd_t f) {
    lock(&current->files->lock);
    struct fd *fd = fdtable_get(current->files, f);
    unlock(&current->files->lock);
    return fd;
}

static fd_t f_install_start(struct fd *fd, fd_t start) {
    assert(start >= 0);
    struct fdtable *table = current->files;
    unsigned size = rlimit(RLIMIT_NOFILE_);
    if (size > table->size)
        size = table->size;

    fd_t f;
    for (f = start; (unsigned) f < size; f++)
        if (table->files[f] == NULL)
            break;
    if ((unsigned) f >= size) {
        int err = fdtable_expand(table, f);
        if (err < 0)
            f = err;
    }

    if (f >= 0) {
        table->files[f] = fd;
        bit_clear(f, table->cloexec);
    } else {
        fd_close(fd);
    }
    return f;
}

fd_t f_install(struct fd *fd, int flags) {
    lock(&current->files->lock);
    fd_t f = f_install_start(fd, 0);
    if (f >= 0) {
        if (flags & O_CLOEXEC_)
            bit_set(f, current->files->cloexec);
        if (flags & O_NONBLOCK_)
            fd_setflags(fd, O_NONBLOCK_);
    }
    unlock(&current->files->lock);
    return f;
}

static int fdtable_close(struct fdtable *table, fd_t f) {
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL)
        return _EBADF;
    if (fd->inode != NULL) // temporary hack for files like sockets that right now don't have inodes but will eventually
        file_lock_remove_owned_by(fd, table);
    int err = fd_close(fd);
    table->files[f] = NULL;
    bit_clear(f, table->cloexec);
    return err;
}

// Roll back an unpublished result without closing an unrelated replacement
// installed by another thread sharing the descriptor table.
void f_close_matching(fd_t f, struct fd *expected) {
    lock(&current->files->lock);
    if (fdtable_get(current->files, f) == expected)
        fdtable_close(current->files, f);
    unlock(&current->files->lock);
}

int f_close(fd_t f) {
    lock(&current->files->lock);
    int err = fdtable_close(current->files, f);
    unlock(&current->files->lock);
    return err;
}

dword_t sys_close(fd_t f) {
    STRACE("close(%d)", f);
    return f_close(f);
}

#define CLOSE_RANGE_UNSHARE_ (1u << 1)
#define CLOSE_RANGE_CLOEXEC_ (1u << 2)

dword_t sys_close_range(uint_t first, uint_t last, uint_t flags) {
    STRACE("close_range(%u, %u, %#x)", first, last, flags);

    if (flags & ~(CLOSE_RANGE_UNSHARE_ | CLOSE_RANGE_CLOEXEC_))
        return _EINVAL;
    if (first > last)
        return _EINVAL;

    if (flags & CLOSE_RANGE_UNSHARE_) {
        int err = fdtable_unshare_current(NULL);
        if (err) return err;
    }
    struct fdtable *table = current->files;
    lock(&table->lock);

    if (table->size == 0) {
        unlock(&table->lock);
        return 0;
    }

    uint_t capped_last = last;
    if (capped_last >= table->size)
        capped_last = table->size - 1;
    if (first >= table->size || first > capped_last) {
        unlock(&table->lock);
        return 0;
    }

    for (uint_t f = first; f <= capped_last; f++) {
        if (flags & CLOSE_RANGE_CLOEXEC_) {
            if (table->files[f] != NULL)
                bit_set(f, table->cloexec);
            continue;
        }
        if (table->files[f] != NULL)
            fdtable_close(table, (fd_t)f);
    }

    unlock(&table->lock);
    return 0;
}

void fdtable_do_cloexec(struct fdtable *table) {
    lock(&table->lock);
    for (fd_t f = 0; (unsigned) f < table->size; f++)
        if (bit_test(f, table->cloexec))
            fdtable_close(table, f);
    unlock(&table->lock);
}

#define F_DUPFD_ 0
#define F_GETFD_ 1
#define F_SETFD_ 2
#define F_GETFL_ 3
#define F_SETFL_ 4

#define F_GETLK_ 5
#define F_SETLK_ 6
#define F_SETLKW_ 7
#define F_GETLK64_ 12
#define F_SETLK64_ 13
#define F_SETLKW64_ 14

#define F_DUPFD_CLOEXEC_ 1030

// Descriptor lookup and reference acquisition must be one table operation.
struct fd *f_get_retain(fd_t f) {
    struct fdtable *table = current->files;
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    if (fd) fd_retain(fd);
    unlock(&table->lock);
    return fd;
}

dword_t sys_dup(fd_t f) {
    struct fd *fd = f_get_retain(f);
    return fd ? f_install(fd, 0) : _EBADF;
}

static dword_t duplicate_to(fd_t f, fd_t new_f, int_t flags, bool same_allowed) {
    if (flags & ~O_CLOEXEC_) return _EINVAL;
    if (!same_allowed && f == new_f) return _EINVAL;
    struct fdtable *table = current->files;
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    int result = _EBADF;
    if (!fd) goto out;
    if (same_allowed && f == new_f) { result = f; goto out; }
    if (new_f < 0 || (rlim_t_) new_f >= rlimit(RLIMIT_NOFILE_)) goto out;
    result = fdtable_expand(table, new_f);
    if (result < 0) goto out;
    // Retain before replacing: the destination may reference the same OFD.
    fd_retain(fd);
    fdtable_close(table, new_f);
    table->files[new_f] = fd;
    bit_clear(new_f, table->cloexec);
    if (flags & O_CLOEXEC_) bit_set(new_f, table->cloexec);
    result = new_f;
out:
    unlock(&table->lock);
    return result;
}

dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags) {
    return duplicate_to(f, new_f, flags, false);
}

dword_t sys_dup2(fd_t f, fd_t new_f) {
    return duplicate_to(f, new_f, 0, true);
}

int fd_getflags(struct fd *fd) {
    if (fd->ops->getflags)
        return fd->ops->getflags(fd);
    return fd->flags;
}

#define FD_ALLOWED_FLAGS (O_APPEND_ | O_NONBLOCK_)
int fd_setflags(struct fd *fd, int flags) {
    if (fd->ops->setflags)
        return fd->ops->setflags(fd, flags);
    fd->flags = (fd->flags & ~FD_ALLOWED_FLAGS) | (flags & FD_ALLOWED_FLAGS);
    return 0;
}

static dword_t fcntl_description(struct fd *fd, fd_t f, dword_t cmd, addr_t arg) {
#ifdef GUEST_ARM64
    if (cmd == 5 || cmd == 6 || cmd == 7 || cmd == 36 || cmd == 37 || cmd == 38) {
        // Linux AArch64 LP64 flock: 8-byte alignment, not i386 packed layout.
        struct { int16_t type,whence; uint32_t pad; int64_t start,len; int32_t pid,pad2; } native;
        if (user_read(arg,&native,sizeof native)) return _EFAULT;
        struct flock_ request={.type=native.type,.whence=native.whence,.start=native.start,.len=native.len,.pid=native.pid};
        bool ofd=cmd>=36;
        if (cmd==5||cmd==36) {
            int result=fcntl_getlk_owned(fd,&request,ofd);if(result<0)return result;
            native.type=request.type;native.whence=request.whence;native.start=request.start;native.len=request.len;native.pid=request.pid;
            if(user_write(arg,&native,sizeof native))return _EFAULT;return 0;
        }
        return fcntl_setlk_owned(fd,&request,cmd==7||cmd==38,ofd);
    }
#endif
    struct flock32_ flock32;
    struct flock_ flock;
    int err;
    switch (cmd) {
        case 1033: // F_ADD_SEALS: only implemented size/lifetime seals.
        case 1034: { // F_GET_SEALS
            if (!fd->memfd) return _EINVAL;
            lock(&fd->memfd->lock);
            int result = fd->memfd->seals;
            if (cmd == 1033) {
                uint_t requested = (uint_t)arg;
                if (requested & ~(F_SEAL_SEAL_ | F_SEAL_SHRINK_ | F_SEAL_GROW_ | F_SEAL_ANY_WRITE_)) {
                    result = _EINVAL;
                    const char *trace = getenv("ISH_DMABUF_TRACE");
                    if (trace && trace[0] == '1') printk("MEMFD_SEAL_REFUSED requested=%#x pid=%d\n", requested, current->pid);
                }
                else if ((fd_getflags(fd) & O_ACCMODE_) == O_RDONLY_ ||
                         (fd->memfd->seals & F_SEAL_SEAL_)) result = _EPERM;
                // Linux mapping_deny_writable: no shared mapping may still write.
                else if ((requested & F_SEAL_WRITE_) && fd->memfd->maywrite_maps) result = _EBUSY;
                else { fd->memfd->seals |= requested; result = 0; }
            }
            unlock(&fd->memfd->lock);
            return result;
        }
        case F_GETFL_:
            STRACE("fcntl(%d, F_GETFL)", f);
            return fd_getflags(fd);
        case F_SETFL_:
            STRACE("fcntl(%d, F_SETFL, %#x)", f, arg);
            return fd_setflags(fd, arg);

        case F_GETLK_:
            STRACE("fcntl(%d, F_GETLK, %#x)", f, arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0) {
                flock32.type = flock.type;
                flock32.whence = flock.whence;
                flock32.start = flock.start;
                flock32.len = flock.len;
                flock32.pid = flock.pid;
                if (user_write(arg, &flock32, sizeof(flock32)))
                    return _EFAULT;
            }
            return err;

        case F_GETLK64_:
            STRACE("fcntl(%d, F_GETLK64, %#x)", f, arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0)
                if (user_write(arg, &flock, sizeof(flock)))
                    return _EFAULT;
            return err;

        case F_SETLK_:
        case F_SETLKW_:
            STRACE("fcntl(%d, F_SETLK%*s, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW64_);

        case F_SETLK64_:
        case F_SETLKW64_:
            STRACE("fcntl(%d, F_SETLK%*s64, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW_);

        default:
            STRACE("fcntl(%d, %d)", f, cmd);
            return _EINVAL;
    }
}

dword_t sys_fcntl(fd_t f, dword_t cmd, addr_t arg) {
    struct fdtable *table = current->files;
    // Commands that inspect/change the descriptor table cannot use borrowed
    // pointers across concurrent close/dup operations.
    if (cmd == F_DUPFD_ || cmd == F_DUPFD_CLOEXEC_ || cmd == F_GETFD_ || cmd == F_SETFD_) {
        lock(&table->lock);
        struct fd *source = fdtable_get(table, f);
        int result = _EBADF;
        if (!source) goto table_out;
        if (cmd == F_GETFD_) result = bit_test(f, table->cloexec);
        else if (cmd == F_SETFD_) {
            if (arg & 1) bit_set(f, table->cloexec);
            else bit_clear(f, table->cloexec);
            result = 0;
        } else {
            // fcntl's third argument for duplication is an int, even on LP64.
            int_t minimum = (int_t) arg;
            if (minimum < 0 || (rlim_t_) minimum >= rlimit(RLIMIT_NOFILE_)) result = _EINVAL;
            else {
                result = f_install_start(fd_retain(source), minimum);
                if (result >= 0 && cmd == F_DUPFD_CLOEXEC_) bit_set(result, table->cloexec);
            }
        }
table_out:
        unlock(&table->lock);
        return result;
    }
    struct fd *fd = f_get_retain(f);
    if (!fd) return _EBADF;
    dword_t result = fcntl_description(fd, f, cmd, arg);
    fd_close(fd);
    return result;
}

dword_t sys_fcntl32(fd_t fd, dword_t cmd, addr_t arg) {
    switch (cmd) {
        case F_GETLK64_:
        case F_SETLK64_:
        case F_SETLKW64_:
            return _EINVAL;
    }
    return sys_fcntl(fd, cmd, arg);
}
