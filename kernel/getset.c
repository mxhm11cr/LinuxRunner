#include <string.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/personality.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

// ---- Linux user namespaces -------------------------------------------------
// One-extent uid/gid maps (the unprivileged form Chromium/Electron use), parent
// chaining for nested namespaces, and setgroups deny. Ids are stored globally on
// the task and translated at the guest boundary; unmapped ids read as 65534.
struct user_ns {
    atomic_uint refcount;
    struct user_ns *parent;
    uid_t_ owner_uid, owner_gid;
    unsigned long long inum;
    lock_t lock;
    bool uid_mapped, gid_mapped, setgroups_deny;
    uid_t_ uid_first, uid_lower, uid_count;
    uid_t_ gid_first, gid_lower, gid_count;
};
#define OVERFLOW_ID_ 65534
#define INIT_USERNS_INUM_ 4026531837ull
static atomic_ullong next_userns_inum = 4026532100ull;

// User and PID namespaces are on by default; ISH_USERNS=0 restores the
// previous behaviour (clone/unshare namespace flags fail with EINVAL/ENOSYS).
bool userns_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *v = getenv("ISH_USERNS"); enabled = !(v && !strcmp(v, "0")); }
    return enabled;
}
struct user_ns *userns_create(struct user_ns *parent, uid_t_ owner_uid, uid_t_ owner_gid) {
    struct user_ns *ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    atomic_init(&ns->refcount, 1);
    ns->parent = parent; if (parent) userns_retain(parent);
    ns->owner_uid = owner_uid; ns->owner_gid = owner_gid;
    ns->inum = atomic_fetch_add(&next_userns_inum, 1);
    lock_init(&ns->lock);
    return ns;
}
void userns_retain(struct user_ns *ns) { if (ns) atomic_fetch_add(&ns->refcount, 1); }
void userns_release(struct user_ns *ns) {
    while (ns && atomic_fetch_sub(&ns->refcount, 1) == 1) {
        struct user_ns *parent = ns->parent; free(ns); ns = parent;
    }
}
unsigned long long userns_inum(struct user_ns *ns) { return ns ? ns->inum : INIT_USERNS_INUM_; }
// Global id -> the id as seen inside ns (through every ancestor).
static uid_t_ view(struct user_ns *ns, uid_t_ id, bool gid) {
    if (!ns) return id;
    uid_t_ parent_view = view(ns->parent, id, gid);
    if (parent_view == OVERFLOW_ID_ && ns->parent) return OVERFLOW_ID_;
    lock(&ns->lock);
    bool mapped = gid ? ns->gid_mapped : ns->uid_mapped;
    uid_t_ first = gid ? ns->gid_first : ns->uid_first, lower = gid ? ns->gid_lower : ns->uid_lower;
    uid_t_ count = gid ? ns->gid_count : ns->uid_count;
    unlock(&ns->lock);
    if (!mapped || parent_view < lower || parent_view - lower >= count) return OVERFLOW_ID_;
    return first + (parent_view - lower);
}
uid_t_ userns_view_uid(struct user_ns *ns, uid_t_ global) { return view(ns, global, false); }
uid_t_ userns_view_gid(struct user_ns *ns, uid_t_ global) { return view(ns, global, true); }
#define VIEW_UID(x) userns_view_uid(current->userns, (x))
#define VIEW_GID(x) userns_view_gid(current->userns, (x))

// Capability in the caller's own guest namespace. File capabilities may grant
// a non-root process a subset; capset must also genuinely narrow root's sets.
bool ns_capable(int cap) {
    return cap >= 0 && cap < 41 && ((current->cap_effective >> cap) & 1);
}
// File grants are not applied across user namespaces. In-namespace root still
// obeys bounding-set and no_new_privs restrictions on exec gains.
void userns_exec_capabilities(void) {
    if (!current->userns) return;
    bool root = VIEW_UID(current->euid) == 0;
    uint64_t next = root ? current->cap_bounding | current->cap_inheritable : 0;
    if (current->no_new_privs || current->ptrace.traced) next &= current->cap_permitted;
    current->cap_permitted = current->cap_effective = next;
}

int userns_show_map(struct task *task, int kind, char *buf, size_t size) {
    struct user_ns *ns = task->userns;
    if (!ns) {
        if (kind == 2) return snprintf(buf, size, "allow\n");
        return snprintf(buf, size, "%10u %10u %10u\n", 0u, 0u, 4294967295u);
    }
    lock(&ns->lock);
    int n = 0;
    if (kind == 2) n = snprintf(buf, size, "%s\n", ns->setgroups_deny ? "deny" : "allow");
    else if (kind == 0 && ns->uid_mapped) n = snprintf(buf, size, "%10u %10u %10u\n", ns->uid_first, ns->uid_lower, ns->uid_count);
    else if (kind == 1 && ns->gid_mapped) n = snprintf(buf, size, "%10u %10u %10u\n", ns->gid_first, ns->gid_lower, ns->gid_count);
    unlock(&ns->lock);
    return n;
}
// Linux user_namespaces(7) write rules, single-extent form. Writers in the
// parent namespace without CAP_SETUID/CAP_SETGID there may only map their own
// euid/egid (gid_map additionally requires setgroups "deny").
int userns_write_map(struct task *task, int kind, const char *data, size_t size) {
    struct user_ns *ns = task->userns;
    if (!ns) return _EPERM;
    char text[64]; if (size >= sizeof(text)) return _EINVAL;
    memcpy(text, data, size); text[size] = 0;
    bool writer_in_parent = current->userns == ns->parent, writer_inside = current->userns == ns;
    if (!writer_in_parent && !writer_inside) return _EPERM;
    if (kind == 2) {
        bool deny = !strncmp(text, "deny", 4), allow = !strncmp(text, "allow", 5);
        if (!deny && !allow) return _EINVAL;
        lock(&ns->lock);
        int err = 0;
        if (ns->gid_mapped) err = _EPERM; else ns->setgroups_deny = deny;
        unlock(&ns->lock);
        return err ? err : (int) size;
    }
    unsigned first, lower, count; char extra;
    if (sscanf(text, "%u %u %u %c", &first, &lower, &count, &extra) != 3 || count == 0) return _EINVAL;
    // The writer's own credential as seen from the namespace's parent.
    uid_t_ own = kind == 0 ? userns_view_uid(ns->parent, current->euid) : userns_view_gid(ns->parent, current->egid);
    bool privileged = ns->parent == NULL ? superuser() : false;
    if (writer_in_parent && current->userns) privileged = (current->cap_effective >> (kind == 0 ? CAP_SETUID_ : 6)) & 1;
    lock(&ns->lock);
    int err = 0;
    if (kind == 0 ? ns->uid_mapped : ns->gid_mapped) err = _EPERM;
    else if (!privileged && (count != 1 || lower != own)) err = _EPERM;
    else if (!privileged && kind == 1 && !ns->setgroups_deny) err = _EPERM;
    else if (kind == 0) { ns->uid_first = first; ns->uid_lower = lower; ns->uid_count = count; ns->uid_mapped = true; }
    else { ns->gid_first = first; ns->gid_lower = lower; ns->gid_count = count; ns->gid_mapped = true; }
    unlock(&ns->lock);
    return err ? err : (int) size;
}

static void reset_dumpable_on_credential_change(uid_t_ old_uid, uid_t_ old_gid) {
    if (current->euid != old_uid || current->egid != old_gid)
        mm_set_dumpable(current->mm, 0);
}

static void capabilities_uid_changed(uid_t_ old_r, uid_t_ old_e, uid_t_ old_s) {
    if ((old_r == 0 || old_e == 0 || old_s == 0) &&
            current->uid != 0 && current->euid != 0 && current->suid != 0 && !current->keep_caps)
        current->cap_permitted = current->cap_effective = 0;
    if (old_e == 0 && current->euid != 0) current->cap_effective = 0;
    if (old_e != 0 && current->euid == 0) current->cap_effective = current->cap_permitted;
}

// PID-namespace views: identity unless the caller is in a new PID namespace.
pid_t_ sys_getpid() {
    STRACE("getpid()");
    if (!current->pidns) return current->tgid;
    lock(&pids_lock);
    pid_t_ pid = task_vnr(current->group->leader, current->pidns);
    unlock(&pids_lock);
    return pid;
}
pid_t_ sys_gettid() {
    STRACE("gettid()");
    if (!current->pidns) return current->pid;
    lock(&pids_lock);
    pid_t_ tid = task_vnr(current, current->pidns);
    unlock(&pids_lock);
    return tid;
}
pid_t_ sys_getppid() {
    STRACE("getppid()");
    pid_t_ ppid;
    lock(&pids_lock);
    ppid = task_parent_pid(current);
    // A parent outside the caller's PID namespace reads as 0, as on Linux.
    if (current->pidns) ppid = pid_view(ppid, current->pidns);
    unlock(&pids_lock);
    return ppid;
}

dword_t sys_getuid32() {
    STRACE("getuid32()");
    return VIEW_UID(current->uid);
}
dword_t sys_getuid() {
    STRACE("getuid()");
    return VIEW_UID(current->uid) & 0xffff;
}

dword_t sys_geteuid32() {
    STRACE("geteuid32()");
    return VIEW_UID(current->euid);
}
dword_t sys_geteuid() {
    STRACE("geteuid()");
    return VIEW_UID(current->euid) & 0xffff;
}

int_t sys_setuid(uid_t_ uid) {
    uid_t_ old_ruid = current->uid, old_suid = current->suid;
    uid_t_ old_euid = current->euid;
    uid_t_ old_egid = current->egid;
    STRACE("setuid(%d)", uid);
    if (uid == (uid_t_) -1) return _EINVAL;
    if (superuser()) {
        current->uid = current->suid = uid;
    } else {
        if (uid != current->uid && uid != current->suid)
            return _EPERM;
    }
    current->euid = uid;
    capabilities_uid_changed(old_ruid, old_euid, old_suid);
    reset_dumpable_on_credential_change(old_euid, old_egid);
    return 0;
}

dword_t sys_setresuid(uid_t_ ruid, uid_t_ euid, uid_t_ suid) {
    uid_t_ old_ruid = current->uid, old_suid = current->suid;
    uid_t_ old_euid = current->euid;
    uid_t_ old_egid = current->egid;
    STRACE("setresuid(%d, %d, %d)", ruid, euid, suid);
    if (!superuser()) {
        if (ruid != (uid_t) -1 && ruid != current->uid && ruid != current->euid && ruid != current->suid)
            return _EPERM;
        if (euid != (uid_t) -1 && euid != current->uid && euid != current->euid && euid != current->suid)
            return _EPERM;
        if (suid != (uid_t) -1 && suid != current->uid && suid != current->euid && suid != current->suid)
            return _EPERM;
    }

    if (ruid != (uid_t) -1)
        current->uid = ruid;
    if (euid != (uid_t) -1)
        current->euid = euid;
    if (suid != (uid_t) -1)
        current->suid = suid;
    capabilities_uid_changed(old_ruid, old_euid, old_suid);
    reset_dumpable_on_credential_change(old_euid, old_egid);
    return 0;
}

int_t sys_getresuid(addr_t ruid_addr, addr_t euid_addr, addr_t suid_addr) {
    STRACE("getresuid(%#x, %#x, %#x)", ruid_addr, euid_addr, suid_addr);
    uid_t_ view_ruid = VIEW_UID(current->uid), view_euid = VIEW_UID(current->euid), view_suid = VIEW_UID(current->suid);
    if (user_put(ruid_addr, view_ruid))
        return _EFAULT;
    if (user_put(euid_addr, view_euid))
        return _EFAULT;
    if (user_put(suid_addr, view_suid))
        return _EFAULT;
    return 0;
}

int_t sys_setreuid(uid_t_ ruid, uid_t_ euid) {
    uid_t_ old_suid = current->suid;
    uid_t_ old_euid = current->euid;
    uid_t_ old_egid = current->egid;
    uid_t_ old_ruid = current->uid;
    if (!superuser()) {
        if (ruid != (uid_t_) -1 && ruid != current->uid && ruid != current->euid)
            return _EPERM;
        if (euid != (uid_t_) -1 && euid != current->uid && euid != current->euid && euid != current->suid)
            return _EPERM;
    }
    if (ruid != (uid_t_) -1) current->uid = ruid;
    if (euid != (uid_t_) -1) current->euid = euid;
    if (ruid != (uid_t_) -1 || (euid != (uid_t_) -1 && euid != old_ruid))
        current->suid = current->euid;
    capabilities_uid_changed(old_ruid, old_euid, old_suid);
    reset_dumpable_on_credential_change(old_euid, old_egid);
    return 0;
}

dword_t sys_getgid32() {
    STRACE("getgid32()");
    return VIEW_GID(current->gid);
}
dword_t sys_getgid() {
    STRACE("getgid()");
    return VIEW_GID(current->gid) & 0xffff;
}

dword_t sys_getegid32() {
    STRACE("getegid32()");
    return VIEW_GID(current->egid);
}
dword_t sys_getegid() {
    STRACE("getegid()");
    return VIEW_GID(current->egid) & 0xffff;
}

int_t sys_setgid(uid_t_ gid) {
    uid_t_ old_euid = current->euid;
    uid_t_ old_egid = current->egid;
    STRACE("setgid(%d)", gid);
    if (gid == (uid_t_) -1) return _EINVAL;
    if (superuser()) {
        current->gid = current->sgid = gid;
    } else {
        if (gid != current->gid && gid != current->sgid)
            return _EPERM;
    }
    current->egid = gid;
    reset_dumpable_on_credential_change(old_euid, old_egid);
    return 0;
}

dword_t sys_setresgid(uid_t_ rgid, uid_t_ egid, uid_t_ sgid) {
    uid_t_ old_euid = current->euid;
    uid_t_ old_egid = current->egid;
    STRACE("setresgid(%d, %d, %d)", rgid, egid, sgid);
    if (!superuser()) {
        if (rgid != (uid_t) -1 && rgid != current->gid && rgid != current->egid && rgid != current->sgid)
            return _EPERM;
        if (egid != (uid_t) -1 && egid != current->gid && egid != current->egid && egid != current->sgid)
            return _EPERM;
        if (sgid != (uid_t) -1 && sgid != current->gid && sgid != current->egid && sgid != current->sgid)
            return _EPERM;
    }

    if (rgid != (uid_t) -1)
        current->gid = rgid;
    if (egid != (uid_t) -1)
        current->egid = egid;
    if (sgid != (uid_t) -1)
        current->sgid = sgid;
    reset_dumpable_on_credential_change(old_euid, old_egid);
    return 0;
}

int_t sys_getresgid(addr_t rgid_addr, addr_t egid_addr, addr_t sgid_addr) {
    STRACE("getresgid(%#x, %#x, %#x)", rgid_addr, egid_addr, sgid_addr);
    uid_t_ view_rgid = VIEW_GID(current->gid), view_egid = VIEW_GID(current->egid), view_sgid = VIEW_GID(current->sgid);
    if (user_put(rgid_addr, view_rgid))
        return _EFAULT;
    if (user_put(egid_addr, view_egid))
        return _EFAULT;
    if (user_put(sgid_addr, view_sgid))
        return _EFAULT;
    return 0;
}

int_t sys_setregid(uid_t_ rgid, uid_t_ egid) {
    uid_t_ old_euid = current->euid;
    uid_t_ old_egid = current->egid;
    uid_t_ old_rgid = current->gid;
    if (!superuser()) {
        if (rgid != (uid_t_) -1 && rgid != current->gid && rgid != current->egid)
            return _EPERM;
        if (egid != (uid_t_) -1 && egid != current->gid && egid != current->egid && egid != current->sgid)
            return _EPERM;
    }
    if (rgid != (uid_t_) -1) current->gid = rgid;
    if (egid != (uid_t_) -1) current->egid = egid;
    if (rgid != (uid_t_) -1 || (egid != (uid_t_) -1 && egid != old_rgid))
        current->sgid = current->egid;
    reset_dumpable_on_credential_change(old_euid, old_egid);
    return 0;
}

int_t sys_getgroups(dword_t size, addr_t list) {
    STRACE("getgroups(%d, %#x)", size, list);
    if ((int_t) size < 0) return _EINVAL;
    if (size == 0)
        return current->ngroups;
    if (size < current->ngroups)
        return _EINVAL;
    for (unsigned i = 0; i < current->ngroups; i++)
        STRACE(" %d", current->groups[i]);
    if (user_write(list, current->groups, current->ngroups * sizeof(uid_t_)))
        return _EFAULT;
    return current->ngroups;
}

int_t sys_setgroups(dword_t size, addr_t list) {
    STRACE("setgroups(%d, %#x)", size, list);
    if (!superuser()) return _EPERM;
    if (size > MAX_GROUPS) return _EINVAL;
    // A bad pointer must not partially change the existing credentials.
    uid_t_ groups[MAX_GROUPS];
    if (user_read(list, groups, size * sizeof(uid_t_))) return _EFAULT;
    memcpy(current->groups, groups, size * sizeof(uid_t_));
    for (unsigned i = 0; i < size; i++)
        STRACE(" %d", current->groups[i]);
    current->ngroups = size;
    return 0;
}

// Linux capget/capset (v1 32-bit, v2/v3 64-bit). Inside a user namespace the
// stored sets describe guest authority, never privileges outside the host/iOS
// sandbox. Non-root file grants are visible and capset can only narrow them.
#define CAP_V1_ 0x19980330u
#define CAP_V2_ 0x20071026u
#define CAP_V3_ 0x20080522u
struct cap_header_ { uint32_t version; int32_t pid; };
struct cap_data_ { uint32_t effective, permitted, inheritable; };
static void current_caps(uint64_t *eff, uint64_t *perm, uint64_t *inh) {
    *eff = current->cap_effective; *perm = current->cap_permitted; *inh = current->cap_inheritable;
}
static int cap_header(addr_t header_addr, unsigned *words) {
    struct cap_header_ h;
    if (user_get(header_addr, h)) return _EFAULT;
    if (h.version == CAP_V1_) *words = 1;
    else if (h.version == CAP_V2_ || h.version == CAP_V3_) *words = 2;
    else { h.version = CAP_V3_; if (user_put(header_addr, h)) return _EFAULT; return _EINVAL; }
    if (h.pid < 0) return _EINVAL;
    if (h.pid != 0 && h.pid != (int32_t) current->pid) return _EPERM; // other tasks: not supported
    return 0;
}
int_t sys_capget(addr_t header_addr, addr_t data_addr) {
    STRACE("capget(%#x, %#x)", header_addr, data_addr);
    unsigned words; int err = cap_header(header_addr, &words);
    if (err == _EINVAL && !data_addr) return 0;
    if (err) return err;
    if (!data_addr) return 0;
    uint64_t eff, perm, inh; current_caps(&eff, &perm, &inh);
    struct cap_data_ d[2] = {{(uint32_t) eff, (uint32_t) perm, (uint32_t) inh},
                             {(uint32_t) (eff >> 32), (uint32_t) (perm >> 32), (uint32_t) (inh >> 32)}};
    if (user_write(data_addr, d, words * sizeof(d[0]))) return _EFAULT;
    return 0;
}
int_t sys_capset(addr_t header_addr, addr_t data_addr) {
    STRACE("capset(%#x, %#x)", header_addr, data_addr);
    unsigned words; int err = cap_header(header_addr, &words);
    if (err) return err;
    struct cap_data_ d[2] = {0};
    if (user_read(data_addr, d, words * sizeof(d[0]))) return _EFAULT;
    uint64_t eff = d[0].effective | (uint64_t) d[1].effective << 32;
    uint64_t perm = d[0].permitted | (uint64_t) d[1].permitted << 32;
    uint64_t inh = d[0].inheritable | (uint64_t) d[1].inheritable << 32;
    uint64_t old_eff, old_perm, old_inh; current_caps(&old_eff, &old_perm, &old_inh);
    if ((perm & ~old_perm) || (eff & ~perm)) return _EPERM;
    if (!ns_capable(8) && (inh & ~(old_inh | old_perm))) return _EPERM;
    if (inh & ~(old_inh | current->cap_bounding)) return _EPERM;
    current->cap_permitted = perm; current->cap_effective = eff; current->cap_inheritable = inh;
    return 0;
}

// minimal version according to Linux sys/personality.h
int_t sys_personality(dword_t persona) {
    STRACE("personality(%#x)", persona);
    // Get the personality
    if (persona == 0xffffffff)
        return current->group->personality;

    // ADDR_NO_RANDOMIZE is the only thing we support, and you can't turn it off
    if (persona != ADDR_NO_RANDOMIZE_)
        return _EINVAL;

    return current->group->personality;
}
