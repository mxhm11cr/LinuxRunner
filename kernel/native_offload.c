// Native offload: run registered guest binaries as native host processes.
// Mappings are configured via CLI: -n ffmpeg -n ffprobe=/usr/local/bin/ffprobe
// Phase 1: macOS only via posix_spawn.
//
// Path redirection is handled transparently by libfakefs_redirect.dylib,
// injected into the native process via DYLD_INSERT_LIBRARIES. This intercepts
// all filesystem calls (open, stat, access, etc.) and redirects absolute guest
// paths to the fakefs data directory. No argv-level path rewriting needed.

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <spawn.h>
#include <mach-o/dyld.h>
#define HAS_POSIX_SPAWN 1
#endif
#endif
#if defined(ISH_NATIVE_OFFLOAD_TEST_SPAWN) && !defined(__APPLE__)
#include <spawn.h>
#define HAS_POSIX_SPAWN 1
#define posix_spawn_file_actions_addchdir posix_spawn_file_actions_addchdir_np
#endif

#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/native_offload.h"
#include "kernel/native_offload_internal.h"
#include "kernel/native_offload_policy.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/fake-db.h"
#ifdef ISH_INTERNAL
#include "fs/fake.h"
#else
#define ISH_INTERNAL
#include "fs/fake.h"
#undef ISH_INTERNAL
#endif

// Startup registration is single-threaded; freeze before any guest can exec.
static atomic_bool registry_frozen;
void native_offload_freeze_registry(void) {
    atomic_store_explicit(&registry_frozen, true, memory_order_release);
}

// The Linux adapter compiles the real portable path only in test fixtures.
#if !defined(__APPLE__) && !defined(ISH_NATIVE_OFFLOAD_TEST_HANDLERS)
int native_offload_add_handler(const char *name, native_handler_func handler) {
    (void)name; (void)handler; return -1;
}
int native_offload_add_cooperative_handler(const char *name,
        native_cooperative_handler_func handler) {
    (void)name; (void)handler; return -1;
}
int native_offload_add(const char *spec) { (void)spec; return -1; }
const char *native_offload_lookup_exec(const char *guest_path, const char *envp, bool *generic_out) {
    (void)guest_path; (void)envp;
    if (generic_out) *generic_out = false;
    return NULL;
}
const char *native_offload_lookup(const char *guest_path) {
    return native_offload_lookup_exec(guest_path, NULL, NULL);
}
int native_offload_exec(const char *native_path, const char *guest_file,
                        size_t argc, const char *argv, const char *envp) {
    (void)native_path; (void)guest_file; (void)argc; (void)argv; (void)envp;
    return _ENOSYS;
}
bool native_offload_forward_signal(struct task *task, int sig) {
    (void)task; (void)sig; return false;
}
#else

// --- Dynamic registry ---

struct offload_entry {
    char *guest_name;
    char *native_path;           // NULL if using handler
    native_handler_func handler; // legacy in-process execution
    native_cooperative_handler_func cooperative;
};

static struct offload_entry offload_entries[NATIVE_OFFLOAD_MAX];
static int offload_count = 0;

#ifdef HAS_POSIX_SPAWN
// Path to the redirect dylib (resolved once at startup)
static char dylib_path[PATH_MAX];
static bool dylib_found = false;

static char *auto_detect_host_path(const char *name) {
    static const char *search_dirs[] = {
        "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", NULL
    };
    char path[PATH_MAX];
    for (const char **dir = search_dirs; *dir; dir++) {
        snprintf(path, sizeof(path), "%s/%s", *dir, name);
        if (access(path, X_OK) == 0)
            return strdup(path);
    }
    return NULL;
}

// Find the redirect dylib next to the ish binary
static void resolve_dylib_path(void) {
#ifdef __APPLE__
    char exe[PATH_MAX];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return;
    char *real = realpath(exe, NULL);
    if (!real) return;
    char *dir = dirname(real);
    snprintf(dylib_path, sizeof(dylib_path), "%s/libfakefs_redirect.dylib", dir);
    free(real);
    if (access(dylib_path, F_OK) == 0)
        dylib_found = true;
    else
        fprintf(stderr, "native_offload: warning: %s not found, path redirection disabled\n",
                dylib_path);
#endif // Apple executable discovery is not modelled by the Linux spawn fixture.
}

int native_offload_add(const char *spec) {
    if (atomic_load_explicit(&registry_frozen, memory_order_acquire) ||
            !spec || !*spec) return -1;
    if (offload_count >= NATIVE_OFFLOAD_MAX) {
        fprintf(stderr, "native_offload: too many entries (max %d)\n", NATIVE_OFFLOAD_MAX);
        return -1;
    }

    // Resolve dylib path on first call
    if (offload_count == 0)
        resolve_dylib_path();

    const char *eq = strchr(spec, '=');
    char *guest_name, *native_path;

    if (eq) {
        guest_name = strndup(spec, eq - spec);
        const char *path = eq + 1;
        if (access(path, X_OK) != 0) {
            fprintf(stderr, "native_offload: %s: not found or not executable\n", path);
            free(guest_name);
            return -1;
        }
        native_path = strdup(path);
    } else {
        guest_name = strdup(spec);
        native_path = auto_detect_host_path(spec);
        if (!native_path) {
            fprintf(stderr, "native_offload: %s: not found in host PATH\n", spec);
            free(guest_name);
            return -1;
        }
    }

    if (!guest_name || !*guest_name || !native_path) {
        free(guest_name); free(native_path); return -1;
    }
    struct offload_entry *e = &offload_entries[offload_count++];
    e->guest_name = guest_name;
    e->native_path = native_path;
    fprintf(stderr, "native_offload: %s → %s\n", guest_name, native_path);
    return 0;
}
#else
int native_offload_add(const char *spec) {
    (void)spec;
    fprintf(stderr, "native_offload: posix_spawn not available on this platform\n");
    return -1;
}
#endif // HAS_POSIX_SPAWN

int native_offload_add_handler(const char *guest_name, native_handler_func handler) {
    if (atomic_load_explicit(&registry_frozen, memory_order_acquire) ||
            !guest_name || !*guest_name || !handler || offload_count >= NATIVE_OFFLOAD_MAX)
        return -1;
    char *name = strdup(guest_name);
    if (!name) return -1;
    offload_entries[offload_count++] = (struct offload_entry) {
        .guest_name = name, .handler = handler,
    };
    fprintf(stderr, "native_offload: %s → [builtin]\n", guest_name);
    return 0;
}

int native_offload_add_cooperative_handler(const char *name,
        native_cooperative_handler_func handler) {
    if (atomic_load_explicit(&registry_frozen, memory_order_acquire) ||
            !name || !*name || !handler || offload_count >= NATIVE_OFFLOAD_MAX) return -1;
    char *copy = strdup(name);
    if (!copy) return -1;
    offload_entries[offload_count++] = (struct offload_entry) {
        .guest_name = copy, .cooperative = handler,
    };
    return 0;
}

// Internal: find entry by guest binary basename
static struct offload_entry *offload_find(const char *guest_path) {
    if (offload_count == 0)
        return NULL;
    const char *base = strrchr(guest_path, '/');
    base = base ? base + 1 : guest_path;
    for (int i = 0; i < offload_count; i++) {
        if (strcmp(base, offload_entries[i].guest_name) == 0)
            return &offload_entries[i];
    }
    return NULL;
}

const char *native_offload_lookup_exec(const char *guest_path, const char *envp, bool *generic_out) {
    if (generic_out) *generic_out = false;
    if (guest_path == NULL) return NULL;
    struct offload_entry *e = offload_find(guest_path);
    if (!e || !native_offload_path_allowed(e->guest_name, guest_path) ||
            native_offload_env_disabled(e->guest_name, envp))
        return NULL;
    if (generic_out) *generic_out = native_offload_name_is_generic(e->guest_name);
    // Return non-NULL to signal "offload this". For handler-only entries
    // (no native_path), return a sentinel so the caller proceeds to exec.
    return e->native_path ? e->native_path : "[builtin]";
}

const char *native_offload_lookup(const char *guest_path) {
    return native_offload_lookup_exec(guest_path, NULL, NULL);
}

// --- Shared helpers ---

extern struct mount *g_fakefs_mount;

// Get the root mount source path (works for both realfs and fakefs)
static const char *get_root_source(void) {
    if (g_fakefs_mount && g_fakefs_mount->source)
        return g_fakefs_mount->source;
    // realfs: get source from the root fd's mount
    if (current->fs && current->fs->root && current->fs->root->mount)
        return current->fs->root->mount->source;
    return NULL;
}

// Translate a guest absolute path to the real host path, honoring bind
// mounts. iSH bind-mounts guest directories (e.g. /var/minis/workspace) to
// real host locations outside the fakefs data/ tree; native offload
// handlers that execute on the host must follow those bind mounts, or
// they'll read/write the wrong files and the guest will see a ghost
// filesystem divergence. Falls back to fakefs->source + guest_path when
// no bind-mount rule matches. Returns true on success.
static bool build_host_path_for_guest(const char *guest_path,
                                      char *out, size_t out_size) {
    if (!guest_path || !out || out_size == 0) return false;
    // 1) bind-mount wins: /var/minis/... → /Users/.../Library/MinisChat/...
    if (guest_path[0] == '/' &&
        fakefs_bind_mount_translate_path(guest_path, out, out_size))
        return true;
    // 2) fall back to fakefs source directory
    const char *root = get_root_source();
    if (!root) return false;
    const char *rel = guest_path;
    if (rel[0] == '/') rel++;
    int n = snprintf(out, out_size, "%s/%s", root, rel);
    return n > 0 && (size_t)n < out_size;
}

static void native_free_string_array(char **arr) {
    if (!arr) return;
    for (char **p = arr; *p; p++) free(*p);
    free(arr);
}

#ifdef HAS_POSIX_SPAWN
static char **build_native_argv(const char *native_path, size_t argc,
                                const char *packed_argv) {
    char **argv = calloc(argc + 1, sizeof(char *));
    if (!argv) return NULL;

    argv[0] = strdup(native_path);
    if (!argv[0]) { free(argv); return NULL; }
    const char *p = packed_argv;
    p += strlen(p) + 1; // skip guest argv[0]
    for (size_t i = 1; i < argc; i++) {
        argv[i] = strdup(p);
        if (!argv[i]) { native_free_string_array(argv); return NULL; }
        p += strlen(p) + 1;
    }
    argv[argc] = NULL;
    return argv;
}

static char **build_native_envp(const char *packed_envp) {
    // Count original entries
    size_t count = 0;
    const char *p = packed_envp;
    while (*p) { count++; p += strlen(p) + 1; }

    // +2 for DYLD_INSERT_LIBRARIES and FAKEFS_ROOT
    char **envp = calloc(count + 3, sizeof(char *));
    if (!envp) return NULL;

    static const char *skip_prefixes[] = {
        "OPENSSL_armcap=", "PYTHONMALLOC=", "PYTHONDONTWRITEBYTECODE=",
        "DYLD_INSERT_LIBRARIES=", "FAKEFS_ROOT=", // don't pass guest values
        NULL
    };

    size_t j = 0;
    p = packed_envp;
    while (*p) {
        bool skip = false;
        for (const char **sp = skip_prefixes; *sp; sp++) {
            if (strncmp(p, *sp, strlen(*sp)) == 0) { skip = true; break; }
        }
        if (!skip) {
            envp[j] = strdup(p);
            if (!envp[j]) { native_free_string_array(envp); return NULL; }
            j++;
        }
        p += strlen(p) + 1;
    }

    // Inject DYLD_INSERT_LIBRARIES + FAKEFS_ROOT for path redirection.
    // Both fakefs and realfs need this: guest absolute paths must be
    // redirected to the rootfs directory on host.
    const char *root_source = get_root_source();
    if (root_source && dylib_found) {
        char buf[PATH_MAX + 32];
        snprintf(buf, sizeof(buf), "DYLD_INSERT_LIBRARIES=%s", dylib_path);
        envp[j] = strdup(buf);
        if (!envp[j]) { native_free_string_array(envp); return NULL; }
        j++;
        snprintf(buf, sizeof(buf), "FAKEFS_ROOT=%s", root_source);
        envp[j] = strdup(buf);
        if (!envp[j]) { native_free_string_array(envp); return NULL; }
        j++;
    }

    envp[j] = NULL;
    return envp;
}
#endif // HAS_POSIX_SPAWN

// --- Signal mapping ---

static int guest_to_host_signal(int guest_sig) {
    switch (guest_sig) {
        case 1:  return SIGHUP;    case 2:  return SIGINT;
        case 3:  return SIGQUIT;   case 6:  return SIGABRT;
        case 9:  return SIGKILL;   case 13: return SIGPIPE;
        case 14: return SIGALRM;   case 15: return SIGTERM;
        case 17: return SIGCHLD;   case 18: return SIGCONT;
        case 19: return SIGSTOP;   case 20: return SIGTSTP;
        default: return guest_sig;
    }
}

bool native_offload_forward_signal(struct task *task, int sig) {
    if (!task->is_native_proxy || task->native_pid <= 0)
        return false;
    int host_sig = guest_to_host_signal(sig);
    if (host_sig > 0)
        kill(task->native_pid, host_sig);
    return true;
}

// --- Post-exec: scan for new files and register in fakefs DB ---
// Since the dylib redirects all paths transparently, we don't know exactly
// which files were created. We scan directories mentioned in argv for any
// files not yet in the DB.

static void register_file_if_new(struct fakefs_db *fs, const char *guest_path,
                                 const char *host_path) {
    struct stat st;
    if (stat(host_path, &st) != 0)
        return;
    if (!S_ISREG(st.st_mode))
        return;

    db_begin_write(fs);
    inode_t existing = path_get_inode(fs, guest_path);
    if (existing != 0) {
        db_commit(fs);
        return;
    }

    struct ish_stat ishstat = {
        .mode = S_IFREG | (st.st_mode & 0777),
        .uid  = current->euid,
        .gid  = current->egid,
        .rdev = 0,
    };
    path_create(fs, guest_path, &ishstat);
    db_commit(fs);
    printk("native_offload: registered %s in fakefs\n", guest_path);
}

// Scan a single directory (non-recursive) for unregistered files
static void scan_dir_for_new_files(struct fakefs_db *fs, const char *guest_dir) {
    if (!g_fakefs_mount || !g_fakefs_mount->source)
        return;

    // Follow bind mounts: guest_dir may live under a bind-mounted prefix
    // whose real content is outside the fakefs data/ tree.
    char host_dir[PATH_MAX];
    if (!build_host_path_for_guest(guest_dir, host_dir, sizeof(host_dir)))
        return;

    DIR *d = opendir(host_dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; // skip . and ..
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;

        char host_path[PATH_MAX], guest_path[PATH_MAX];
        snprintf(host_path, sizeof(host_path), "%s/%s", host_dir, ent->d_name);
        snprintf(guest_path, sizeof(guest_path), "%s/%s", guest_dir, ent->d_name);
        register_file_if_new(fs, guest_path, host_path);
    }
    closedir(d);
}

// Get guest CWD path (thread-local legacy scan buffer or NULL)
static const char *get_guest_cwd(void) {
    if (!current->fs || !current->fs->pwd)
        return NULL;
    static __thread char guest_cwd[MAX_PATH];
    int err = current->fs->pwd->mount->fs->getpath(current->fs->pwd, guest_cwd);
    if (err < 0) return NULL;
    return guest_cwd;
}

// Extract directories from argv that might contain new output files,
// plus always scan CWD (native processes often write to CWD with relative paths).
static void register_new_files(size_t argc, const char *packed_argv) {
    if (!g_fakefs_mount) return;
    struct fakefs_db *fs = &g_fakefs_mount->fakefs;

    // Always scan CWD — native processes commonly write to CWD via relative paths
    // (e.g., yt-dlp calls ffmpeg with relative output filename)
    const char *guest_cwd = get_guest_cwd();
    if (guest_cwd)
        scan_dir_for_new_files(fs, guest_cwd);

    const char *p = packed_argv;
    p += strlen(p) + 1; // skip argv[0]

    for (size_t i = 1; i < argc; i++) {
        if ((p[0] == '/' || (p[0] == '.' && p[1] == '/')) && !strstr(p, "://")) {
            // It's a path — check both as file and scan its parent dir.
            // Use bind-mount-aware translation so we register the file at
            // the real host location the handler actually wrote to.
            char host_path[PATH_MAX];
            if (build_host_path_for_guest(p, host_path, sizeof(host_path)))
                register_file_if_new(fs, p, host_path);

            // Also scan parent directory for pattern-generated files
            char parent[PATH_MAX];
            strncpy(parent, p, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            char *slash = strrchr(parent, '/');
            if (slash && slash != parent) {
                *slash = '\0';
                // Skip if same as CWD (already scanned)
                if (!guest_cwd || strcmp(parent, guest_cwd) != 0)
                    scan_dir_for_new_files(fs, parent);
            }
        }
        p += strlen(p) + 1;
    }
}

// --- CWD resolution ---

static char *get_host_cwd(int *error) {
    *error = _ENOENT;
    if (!current->fs || !current->fs->pwd)
        return NULL;
    char guest_cwd[MAX_PATH];
    int err = current->fs->pwd->mount->fs->getpath(current->fs->pwd, guest_cwd);
    if (err < 0) { *error = err; return NULL; }

    char *host_cwd = malloc(PATH_MAX);
    if (!host_cwd) { *error = _ENOMEM; return NULL; }

    // Handle empty guest cwd ("/" after stripping leading slash) by falling
    // back directly to the fakefs source — bind-mount translation is only
    // meaningful for subpaths of a mounted prefix.
    if (guest_cwd[0] == '\0' || (guest_cwd[0] == '/' && guest_cwd[1] == '\0')) {
        const char *root = get_root_source();
        if (!root) { free(host_cwd); return NULL; }
        if (snprintf(host_cwd, PATH_MAX, "%s", root) >= PATH_MAX) {
            *error = _ENAMETOOLONG; free(host_cwd); return NULL;
        }
        *error = 0;
        return host_cwd;
    }

    if (!build_host_path_for_guest(guest_cwd, host_cwd, PATH_MAX)) {
        *error = _ENAMETOOLONG;
        free(host_cwd);
        return NULL;
    }
    *error = 0;
    return host_cwd;
}

// --- Pipe forwarding for stdout/stderr ---
// Two modes:
// 1. Guest fd mode (iOS): reads from pipe, writes via fd->ops->write (TTY driver)
// 2. Host fd mode (macOS posix_spawn): reads from pipe, writes to host real_fd

struct pipe_fwd {
    int pipe_rd;
    int dest_fd;        // host fd (macOS posix_spawn path)
    struct fd *guest_fd; // guest fd (iOS handler path), NULL if using dest_fd
};

static void *pipe_forward_thread(void *arg) {
    struct pipe_fwd *fwd = arg;
    char buf[4096];
    ssize_t n;
    while ((n = read(fwd->pipe_rd, buf, sizeof(buf))) > 0) {
        // Replace \r not followed by \n with \n (progress bar lines)
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\r') {
                if (i + 1 < n && buf[i + 1] == '\n')
                    continue; // \r\n is fine, keep both
                buf[i] = '\n';
            }
        }
        if (fwd->guest_fd && fwd->guest_fd->ops->write) {
            // iOS path: write through guest fd ops (TTY driver → Terminal UI)
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = fwd->guest_fd->ops->write(fwd->guest_fd,
                                                       buf + written, n - written);
                if (w <= 0) break;
                written += w;
            }
        } else {
            // macOS path: write to host real_fd
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(fwd->dest_fd, buf + written, n - written);
                if (w <= 0) {
                    if (w < 0 && errno == EINTR) continue;
                    break;
                }
                written += w;
            }
        }
    }
    close(fwd->pipe_rd);
    if (fwd->guest_fd)
        fd_close(fwd->guest_fd);
    free(fwd);
    return NULL;
}

// Transfer pipe_rd ownership only on successful thread creation. The caller
// still owns it on every failure, including a failed pthread_create.
static int start_pipe_forward(int pipe_rd, int dest_fd, struct fd *guest_fd,
        pthread_t *tid) {
    struct pipe_fwd *fwd = malloc(sizeof(*fwd));
    if (!fwd) return _ENOMEM;
    *fwd = (struct pipe_fwd) {
        .pipe_rd = pipe_rd, .dest_fd = dest_fd,
        .guest_fd = guest_fd ? fd_retain(guest_fd) : NULL,
    };
    int err = pthread_create(tid, NULL, pipe_forward_thread, fwd);
    if (err) {
        if (fwd->guest_fd) fd_close(fwd->guest_fd);
        free(fwd);
        errno = err; // pthread APIs return errno values rather than setting it.
        return errno_map();
    }
    return 0;
}

// Snapshot only stdio that survives exec, with references held until cleanup.
// An undersized fdtable and CLOEXEC stdio must not become stale borrowed fds.
static void retain_exec_stdio(struct fd *stdio[3]) {
    lock(&current->files->lock);
    for (unsigned i = 0; i < 3; i++) {
        struct fd *fd = fdtable_get(current->files, i);
        if (fd && !bit_test(i, current->files->cloexec)) stdio[i] = fd_retain(fd);
    }
    unlock(&current->files->lock);
}

struct offload_pipe {
    int ends[2];
    pthread_t thread;
    bool started;
};

static void finish_offload_pipe(struct offload_pipe *pipe) {
    if (pipe->ends[1] >= 0) close(pipe->ends[1]);
    if (pipe->started) {
        // No output was produced on a setup failure, so EOF releases the reader.
        // Successful handlers still require a separate bounded-sink contract.
        int err = pthread_join(pipe->thread, NULL);
        if (err) die("native_offload: cannot join owned forwarder: %s", strerror(err));
    } else if (pipe->ends[0] >= 0) {
        close(pipe->ends[0]);
    }
    pipe->ends[0] = pipe->ends[1] = -1;
    pipe->started = false;
}

static int prepare_offload_pipe(struct offload_pipe *out, struct fd *guest,
        bool use_guest_ops) {
    if (!guest) return 0;
    if (pipe(out->ends) < 0) return errno_map();
    for (unsigned i = 0; i < 2; i++) {
        // Reserve stdio numbers for spawn actions, even if host 0/1/2 were
        // closed. Internal endpoints must never survive an unrelated exec.
        if (out->ends[i] < 3) {
            int moved = fcntl(out->ends[i], F_DUPFD_CLOEXEC, 3);
            if (moved < 0) return errno_map();
            close(out->ends[i]);
            out->ends[i] = moved;
        } else if (fcntl(out->ends[i], F_SETFD, FD_CLOEXEC) < 0) {
            return errno_map();
        }
    }
    int err = start_pipe_forward(out->ends[0],
        use_guest_ops ? -1 : guest->real_fd, use_guest_ops ? guest : NULL, &out->thread);
    if (err < 0) return err;
    out->started = true;
    return 0;
}

// --- Exec semantics (shared by posix_spawn and in-process handler) ---

static void apply_exec_semantics(const char *guest_file) {
    lock(&current->general_lock);
    const char *base = strrchr(guest_file, '/');
    base = base ? base + 1 : guest_file;
    strncpy(current->comm, base, sizeof(current->comm));
    unlock(&current->general_lock);
    update_thread_name();

    current->did_exec = true;
    vfork_notify(current);
    fdtable_do_cloexec(current->files);

    if (current->sighand) {
        lock(&current->sighand->lock);
        for (int sig = 0; sig < NUM_SIGS; sig++) {
            struct sigaction_ *action = &current->sighand->action[sig];
            if (action->handler != SIG_IGN_)
                action->handler = SIG_DFL_;
        }
        current->altstack = 0;
        current->altstack_size = 0;
        unlock(&current->sighand->lock);
    }
}

// --- In-process handler execution (works on both macOS and iOS) ---

static int exec_handler(native_handler_func handler, const char *guest_file,
                        size_t argc, const char *argv, const char *envp) {
    (void)envp;
    if (!handler || !guest_file || !argv || argc == 0 || argc > INT_MAX ||
            argc > SIZE_MAX / sizeof(char *) - 1) return _EINVAL;
    const char *root_source = get_root_source();
    char **handler_argv = calloc(argc + 1, sizeof(char *));
    if (!handler_argv) return _ENOMEM;
    struct fd *stdio[3] = {0};
    struct offload_pipe output[2] = {
        {.ends = {-1, -1}}, {.ends = {-1, -1}},
    };
    char *host_cwd = NULL;
    int saved_cwd = -1;
    bool cwd_changed = false;
    int err = _ENOMEM;
    const char *p = argv;
    for (size_t i = 0; i < argc; i++) {
        if (root_source && p[0] == '/' && !strstr(p, "://")) {
            char host_path[PATH_MAX];
            if (!build_host_path_for_guest(p, host_path, sizeof(host_path))) {
                err = _ENAMETOOLONG;
                goto rollback;
            }
            handler_argv[i] = strdup(host_path);
        } else {
            handler_argv[i] = strdup(p);
        }
        if (!handler_argv[i]) goto rollback;
        p += strlen(p) + 1;
    }
    // Resolve legacy CWD before starting workers/committing exec. Explicit
    // per-handler filesystem context will replace chdir for cooperative callers.
    if (current->fs && current->fs->pwd) {
        host_cwd = get_host_cwd(&err);
        if (!host_cwd) goto rollback;
        saved_cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (saved_cwd < 0) { err = errno_map(); goto rollback; }
    }
    retain_exec_stdio(stdio);
    if (stdio[1] && !stdio[1]->ops->write) { err = _EBADF; goto rollback; }
    if (stdio[2] && !stdio[2]->ops->write) { err = _EBADF; goto rollback; }
    err = prepare_offload_pipe(&output[0], stdio[1], true);
    if (err < 0) goto rollback;
    err = prepare_offload_pipe(&output[1], stdio[2], true);
    if (err < 0) goto rollback;
    if (host_cwd) {
        if (chdir(host_cwd) < 0) { err = errno_map(); goto rollback; }
        cwd_changed = true;
    }
    // No fallible setup remains. CLOEXEC stdio was excluded from the snapshot.
    apply_exec_semantics(guest_file);
    int ret = handler((int)argc, handler_argv,
        stdio[0] && stdio[0]->real_fd >= 0 ? stdio[0]->real_fd : -1,
        output[0].ends[1], output[1].ends[1]);
    finish_offload_pipe(&output[0]);
    finish_offload_pipe(&output[1]);
    if (cwd_changed && fchdir(saved_cwd) < 0)
        die("native_offload: cannot restore legacy cwd: %s", strerror(errno));
    if (saved_cwd >= 0) close(saved_cwd);
    free(host_cwd);
    for (unsigned i = 0; i < 3; i++) if (stdio[i]) fd_close(stdio[i]);
    native_free_string_array(handler_argv);
    register_new_files(argc, argv);
    do_exit((ret & 0xff) << 8);
    __builtin_unreachable();

rollback:
    // Forwarders have no producer yet; closing writes releases any started
    // reader with EOF. Unstarted pipe ends and retained references stay ours.
    finish_offload_pipe(&output[0]);
    finish_offload_pipe(&output[1]);
    if (saved_cwd >= 0) close(saved_cwd);
    free(host_cwd);
    for (unsigned i = 0; i < 3; i++) if (stdio[i]) fd_close(stdio[i]);
    native_free_string_array(handler_argv);
    return err;
}

// --- Explicit guest-context execution; isolated from all legacy host paths ---
static int exec_cooperative(native_cooperative_handler_func handler,
        const char *guest_file, size_t argc, const char *argv) {
    if (!handler || !guest_file || !argv || argc == 0 || argc > INT_MAX ||
            argc > SIZE_MAX / sizeof(char *) - 1) return _EINVAL;
    // A sibling exit_group could detach resources from an in-process caller.
    lock(&pids_lock);
    lock(&current->group->lock);
    bool alone = !current->group->doing_group_exit;
    struct task *peer;
    list_for_each_entry(&current->group->threads, peer, group_links)
        if (peer != current) alone = false;
    unlock(&current->group->lock);
    unlock(&pids_lock);
    if (!alone) return _EBUSY;

    char **args = calloc(argc + 1, sizeof(char *));
    if (!args) return _ENOMEM;
    struct native_handler_context context = {
        .owner = current, .thread = pthread_self(),
    };
    int err = _ENOMEM;
    const char *p = argv;
    for (size_t i = 0; i < argc; i++) {
        args[i] = strdup(p);
        if (!args[i]) goto rollback;
        p += strlen(p) + 1;
    }
    context.fs = native_fs_context_create();
    if (IS_ERR(context.fs)) { err = PTR_ERR(context.fs); goto rollback; }
    retain_exec_stdio(context.stdio);
    for (unsigned i = 0; i < 3; i++) {
        err = native_io_admit(context.stdio[i], i != 0);
        if (err < 0) goto rollback;
    }
    err = native_io_init(&context);
    if (err < 0) goto rollback;
    // Publish before exec action reset so ignored/blocked pending signals keep
    // their original admission meaning. No further fallible setup remains.
    if (!native_cancel_begin(current, &context.cancel)) { err = _EBUSY; goto rollback; }
    apply_exec_semantics(guest_file);
    int ret = native_handler_check(&context) ? 0 : handler((int)argc, args, &context);
    // A callback that forgot its final checkpoint must not report success after
    // the deadline. This still cannot preempt a callback that never returns.
    native_handler_check(&context);
    // Token stays published through cleanup; no forwarding workers to abandon.
    for (unsigned i = 0; i < 3; i++) if (context.stdio[i]) fd_close(context.stdio[i]);
    native_fs_context_destroy(context.fs);
    native_free_string_array(args);
    int sig = native_cancel_finish(current, &context.cancel);
    int status = (ret & 0xff) << 8;
    if (context.io_error && !status) status = 1 << 8;
    if (sig) status = sig;
    do_exit(status);
    __builtin_unreachable();
rollback:
    for (unsigned i = 0; i < 3; i++) if (context.stdio[i]) fd_close(context.stdio[i]);
    native_fs_context_destroy(context.fs);
    native_free_string_array(args);
    return err;
}

// --- posix_spawn execution (macOS only) ---

#ifdef HAS_POSIX_SPAWN
static int exec_posix_spawn(const char *native_path, const char *guest_file,
                            size_t argc, const char *argv, const char *envp) {
    if (!native_path || !guest_file || !argv || !envp || argc == 0 ||
            argc > INT_MAX || argc > SIZE_MAX / sizeof(char *) - 1) return _EINVAL;
    char **native_argv = build_native_argv(native_path, argc, argv);
    if (!native_argv) return _ENOMEM;
    char **native_envp = build_native_envp(envp);
    if (!native_envp) { native_free_string_array(native_argv); return _ENOMEM; }
    struct fd *stdio[3] = {0};
    struct offload_pipe output[2] = {
        {.ends = {-1, -1}}, {.ends = {-1, -1}},
    };
    char *host_cwd = NULL;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    bool actions_live = false, attrs_live = false;
    int err = _ENOMEM, host_err;
    retain_exec_stdio(stdio);
    if (current->fs && current->fs->pwd) {
        host_cwd = get_host_cwd(&err);
        if (!host_cwd) goto spawn_rollback;
    }
    // Refuse non-host-backed output, rather than silently redirecting it to
    // inherited host stdio. The portable handler path serves guest-only sinks.
    for (unsigned i = 0; i < 3; i++)
        if (stdio[i] && stdio[i]->real_fd < 0) { err = _EBADF; goto spawn_rollback; }
    err = prepare_offload_pipe(&output[0], stdio[1], false);
    if (err < 0) goto spawn_rollback;
    err = prepare_offload_pipe(&output[1], stdio[2], false);
    if (err < 0) goto spawn_rollback;
#define SPAWN_CHECK(call) do { \
    host_err = (call); \
    if (host_err) { errno = host_err; err = errno_map(); goto spawn_rollback; } \
} while (0)
    SPAWN_CHECK(posix_spawn_file_actions_init(&actions));
    actions_live = true;
    SPAWN_CHECK(posix_spawnattr_init(&attrs));
    attrs_live = true;
    if (stdio[0]) SPAWN_CHECK(posix_spawn_file_actions_adddup2(&actions, stdio[0]->real_fd, 0));
    else SPAWN_CHECK(posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0));
    for (unsigned i = 0; i < 2; i++) {
        if (output[i].started) {
            SPAWN_CHECK(posix_spawn_file_actions_adddup2(&actions, output[i].ends[1], i + 1));
            SPAWN_CHECK(posix_spawn_file_actions_addclose(&actions, output[i].ends[0]));
            SPAWN_CHECK(posix_spawn_file_actions_addclose(&actions, output[i].ends[1]));
        } else {
            SPAWN_CHECK(posix_spawn_file_actions_addopen(&actions, i + 1, "/dev/null", O_WRONLY, 0));
        }
    }
    if (host_cwd) SPAWN_CHECK(posix_spawn_file_actions_addchdir(&actions, host_cwd));
    pid_t native_pid;
    SPAWN_CHECK(posix_spawn(&native_pid, native_path, &actions, &attrs, native_argv, native_envp));
#undef SPAWN_CHECK
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attrs);
    native_free_string_array(native_argv);
    native_free_string_array(native_envp);
    free(host_cwd);
    // All forwarders are live before the child exists; no fallible setup can
    // strand a child or force a return after guest exec commitment.
    for (unsigned i = 0; i < 2; i++) {
        if (output[i].ends[1] >= 0) close(output[i].ends[1]);
        output[i].ends[1] = -1;
    }
    current->native_pid = native_pid;
    current->is_native_proxy = true;
    apply_exec_semantics(guest_file);
    int status;
    while (true) {
        pid_t ret = waitpid(native_pid, &status, 0);
        if (ret == native_pid) break;
        if (ret < 0 && errno == EINTR) continue;
        // Returning with a live child would violate ownership. An unexpected
        // wait failure is fatal to this execution, never a setup rollback.
        if (ret < 0) die("native_offload: cannot reap child: %s", strerror(errno));
    }
    finish_offload_pipe(&output[0]);
    finish_offload_pipe(&output[1]);
    for (unsigned i = 0; i < 3; i++) if (stdio[i]) fd_close(stdio[i]);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) << 8 :
        WIFSIGNALED(status) ? WTERMSIG(status) : 1 << 8;
    register_new_files(argc, argv);
    current->is_native_proxy = false;
    current->native_pid = 0;
    do_exit(exit_code);
    __builtin_unreachable();

spawn_rollback:
    if (attrs_live) posix_spawnattr_destroy(&attrs);
    if (actions_live) posix_spawn_file_actions_destroy(&actions);
    finish_offload_pipe(&output[0]);
    finish_offload_pipe(&output[1]);
    for (unsigned i = 0; i < 3; i++) if (stdio[i]) fd_close(stdio[i]);
    free(host_cwd);
    native_free_string_array(native_argv);
    native_free_string_array(native_envp);
    return err;
}
#endif // HAS_POSIX_SPAWN

// --- Main exec dispatcher ---

int native_offload_exec(const char *native_path,
                        const char *guest_file,
                        size_t argc, const char *argv,
                        const char *envp) {
    // Native entrypoints borrow host offsets/access modes. Memfd descriptions
    // virtualize these independently; use the ordinary interpreter exec path
    // before any native side effects rather than bypassing guest permissions.
    struct fd *stdio[3] = {0};
    retain_exec_stdio(stdio);
    bool virtual_file = false;
    for (unsigned i = 0; i < 3; i++) if (stdio[i]) {
        virtual_file |= stdio[i]->memfd != NULL;
        fd_close(stdio[i]);
    }
    if (virtual_file) return _EOPNOTSUPP;
    // Check for in-process handler first (works on iOS and macOS)
    struct offload_entry *entry = offload_find(guest_file);
    if (entry && entry->cooperative)
        return exec_cooperative(entry->cooperative, guest_file, argc, argv);
    if (entry && entry->handler)
        return exec_handler(entry->handler, guest_file, argc, argv, envp);

#ifdef HAS_POSIX_SPAWN
    // posix_spawn path (macOS only, native_path must be valid)
    if (!native_path || strcmp(native_path, "[builtin]") == 0)
        return _ENOEXEC;

    return exec_posix_spawn(native_path, guest_file, argc, argv, envp);
#else
    (void)native_path;
    return _ENOEXEC;
#endif
}

#endif // __APPLE__
