#include "kernel/signal.h"
#include "task.h"
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "misc.h"
#include "kernel/calls.h"
#include "kernel/ptrace.h"
#include "kernel/futex.h"
#include "kernel/random.h"
#include "kernel/errno.h"
#include "kernel/filecaps.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/elf.h"
#include "kernel/vdso.h"
#include "kernel/mm.h"
#include "kernel/native_offload.h"
#if defined(__APPLE__)
// Declared here: <sys/sysctl.h> pulls in Darwin's <sys/proc.h>, whose P_EXEC
// would replace the runtime's page flag of the same name (kernel/memory.h).
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif

// Linux AT_HWCAP bits the interpreter implements and has checked against a
// native ARM64 oracle (aes-instructions.c, crc_sha_pmull_oracle.c). CRC32 and
// SHA1/SHA2 gadgets execute the host instructions, so those bits are offered
// only when the host CPU reports them; PMULL is a portable helper.
#define LR_HWCAP_FP (1 << 0)
#define LR_HWCAP_ASIMD (1 << 1)
#define LR_HWCAP_AES (1 << 3)
#define LR_HWCAP_PMULL (1 << 4)
#define LR_HWCAP_SHA1 (1 << 5)
#define LR_HWCAP_SHA2 (1 << 6)
#define LR_HWCAP_CRC32 (1 << 7)
static bool host_has(const char *name) {
#if defined(__APPLE__)
    int value = 0;
    size_t size = sizeof(value);
    return sysctlbyname(name, &value, &size, NULL, 0) == 0 && value != 0;
#else
    (void) name;
    return false;
#endif
}
static dword_t guest_hwcap_value;
static void guest_hwcap_compute(void) {
    guest_hwcap_value = LR_HWCAP_FP | LR_HWCAP_ASIMD | LR_HWCAP_AES | LR_HWCAP_PMULL;
    if (host_has("hw.optional.armv8_crc32")) guest_hwcap_value |= LR_HWCAP_CRC32;
    if (host_has("hw.optional.arm.FEAT_SHA1")) guest_hwcap_value |= LR_HWCAP_SHA1;
    if (host_has("hw.optional.arm.FEAT_SHA256")) guest_hwcap_value |= LR_HWCAP_SHA2;
}
static dword_t guest_hwcap(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, guest_hwcap_compute);
    return guest_hwcap_value;
}

#define ARGV_MAX 32 * PAGE_SIZE

typedef struct elf_header64 exec_elf_header;
typedef struct prg_header64 exec_prg_header;

struct exec_args {
    // number of arguments
    size_t count;
    // series of count null-terminated strings, plus an extra null for good measure
    const char *args;
};

struct exec_ids { uid_t_ euid; uid_t_ egid; bool secure; };

static inline addr_t align_stack(addr_t sp);
static inline ssize_t user_strlen(addr_t p);
static inline int user_memset(addr_t start, byte_t val, dword_t len);
static inline addr_t copy_string(addr_t sp, const char *string);
static inline addr_t args_copy(addr_t sp, struct exec_args args);
static size_t args_size(struct exec_args args);

static int read_header(struct fd *fd, exec_elf_header *header) {
    int err;
    if (!fd->ops->pread)
        return _EACCES;
    if ((err = fd->ops->pread(fd, header, sizeof(*header), 0)) != sizeof(*header)) {
        if (err < 0)
            return _EIO;
        return _ENOEXEC;
    }
    if (memcmp(&header->magic, ELF_MAGIC, sizeof(header->magic)) != 0
            || (header->type != ELF_EXECUTABLE && header->type != ELF_DYNAMIC)
            || header->bitness != ELF_CLASS
            || header->endian != ELF_LITTLEENDIAN
            || header->elfversion1 != 1
            || header->machine != ELF_MACHINE) {
        return _ENOEXEC;
    }
    return 0;
}

static int read_prg_headers(struct fd *fd, exec_elf_header header, exec_prg_header **ph_out) {
    ssize_t ph_size = sizeof(exec_prg_header) * header.phent_count;
    exec_prg_header *ph = malloc(ph_size);
    if (ph == NULL)
        return _ENOMEM;

    if (fd->ops->pread(fd, ph, ph_size, header.prghead_off) != ph_size) {
        free(ph);
        if (errno != 0)
            return _EIO;
        return _ENOEXEC;
    }

    *ph_out = ph;
    return 0;
}

static int load_entry(exec_prg_header ph, addr_t bias, struct fd *fd) {
    int err;

    addr_t addr = ph.vaddr + bias;
    addr_t offset = ph.offset;
    addr_t memsize = ph.memsize;
    addr_t filesize = ph.filesize;

    int flags = P_READ;
    if (ph.flags & PH_W) flags |= P_WRITE;
    if (ph.flags & PH_X) flags |= P_EXEC;

    if ((err = fd->ops->mmap(fd, current->mem, PAGE(addr),
                    PAGE_ROUND_UP(filesize + PGOFFSET(addr)),
                    offset - PGOFFSET(addr), flags, MMAP_PRIVATE)) < 0)
        return err;
    // TODO find a better place for these to avoid code duplication
    mem_pt(current->mem, PAGE(addr))->data->fd = fd_mapping_identity(fd);
    mem_pt(current->mem, PAGE(addr))->data->file_offset = offset - PGOFFSET(addr);

    if (memsize > filesize) {
        // put zeroes between addr + filesize and addr + memsize, call that bss
        dword_t bss_size = memsize - filesize;

        // first zero the tail from the end of the file mapping to the end
        // of the load entry or the end of the page, whichever comes first
        addr_t file_end = addr + filesize;
        dword_t tail_size = PAGE_SIZE - PGOFFSET(file_end);
        if (tail_size == PAGE_SIZE)
            // if you can calculate tail_size better and not have to do this please let me know
            tail_size = 0;

        if (tail_size != 0) {
            // Unlock and lock the mem because the user functions must be
            // called without locking mem.
            write_wrunlock(&current->mem->lock);
            user_memset(file_end, 0, tail_size);
            write_wrlock(&current->mem->lock);
        }
        if (tail_size > bss_size)
            tail_size = bss_size;

        // then map the pages from after the file mapping up to and including the end of bss
        if (bss_size - tail_size != 0) {
            if ((err = pt_map_nothing(current->mem, PAGE_ROUND_UP(addr + filesize),
                    PAGE_ROUND_UP(bss_size - tail_size), flags)) < 0)
                return err;
        }
    }
    return 0;
}

static addr_t find_hole_for_elf(exec_elf_header *header, exec_prg_header *ph) {
    exec_prg_header *first = NULL, *last = NULL;
    for (int i = 0; i < header->phent_count; i++) {
        if (ph[i].type == PT_LOAD) {
            if (first == NULL)
                first = &ph[i];
            last = &ph[i];
        }
    }
    pages_t size = 0;
    if (first != NULL) {
        pages_t a = PAGE_ROUND_UP(last->vaddr + last->memsize);
        pages_t b = PAGE(first->vaddr);
        size = a - b;
    }
    return pt_find_hole(current->mem, size) << PAGE_BITS;
}

static int elf_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp, struct exec_ids ids) {
    int err = 0;

    // read the headers
    exec_elf_header header;
    if ((err = read_header(fd, &header)) < 0)
        return err;
    exec_prg_header *ph;
    if ((err = read_prg_headers(fd, header, &ph)) < 0)
        return err;

    // Linux AArch64 defaults to a non-executable stack. PT_GNU_STACK changes
    // guest permissions only; interpreted code never needs host PROT_EXEC.
    int stack_prot = P_READ | P_WRITE | P_GROWSDOWN;
    for (unsigned i = 0; i < header.phent_count; i++)
        if (ph[i].type == PT_GNU_STACK)
            stack_prot = P_READ | P_WRITE | P_GROWSDOWN | ((ph[i].flags & PH_X) ? P_EXEC : 0);

    // look for an interpreter
    char *interp_name = NULL;
    struct fd *interp_fd = NULL;
    exec_elf_header interp_header;
    exec_prg_header *interp_ph = NULL;
    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_INTERP)
            continue;
        if (interp_name) {
            // can't have two interpreters
            err = _EINVAL;
            goto out_free_interp;
        }

        if (ph[i].filesize == 0 || ph[i].filesize > MAX_PATH) {
            err = _ENAMETOOLONG;
            goto out_free_interp;
        }
        interp_name = malloc(ph[i].filesize + 1);
        err = _ENOMEM;
        if (interp_name == NULL)
            goto out_free_ph;

        // read the interpreter name out of the file
        err = _EIO;
        ssize_t interp_read = fd->ops->pread(fd, interp_name, ph[i].filesize, ph[i].offset);
        if (interp_read < 0 || (uint64_t) interp_read != ph[i].filesize)
            goto out_free_interp;
        interp_name[ph[i].filesize] = '\0';

        // open interpreter and read headers
        interp_fd = generic_open_execat(AT_PWD, interp_name, 0);
        if (IS_ERR(interp_fd)) {
            err = PTR_ERR(interp_fd);
            goto out_free_interp;
        }
        if ((err = read_header(interp_fd, &interp_header)) < 0) {
            if (err == _ENOEXEC) err = _ELIBBAD;
            goto out_free_interp;
        }
        if ((err = read_prg_headers(interp_fd, interp_header, &interp_ph)) < 0) {
            if (err == _ENOEXEC) err = _ELIBBAD;
            goto out_free_interp;
        }
    }

    // free the process's memory.
    // from this point on, if any error occurs the process will have to be
    // killed before it even starts. please don't be too sad about it, it's
    // just a process.
    //
    // general_lock protects current->mm. otherwise procfs might read the
    // pointer before it's released and then try to lock it after it's
    // released.
    struct mm *new_mm = mm_new();
    if (new_mm == NULL)
        return _ENOMEM;
    futex_pi_exit();
    futex_robust_exit();
    current->robust_list = 0;
    lock(&current->general_lock);
    struct mm *old_mm = current->mm;
    task_set_mm(current, new_mm);
    mm_release(old_mm);
    unlock(&current->general_lock);
    write_wrlock(&current->mem->lock);

    current->mm->exefile = fd_retain(fd);

    addr_t load_addr = 0; // used for AX_PHDR
    bool load_addr_set = false;
    addr_t bias = 0; // offset for loading shared libraries as executables

    // Map loadable program segments.
    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_LOAD)
            continue;

        if (!load_addr_set && header.type == ELF_DYNAMIC) {
            // see giant comment in linux/fs/binfmt_elf.c, around line 950
            // ARM64: Always use dynamic placement to avoid conflicts with
            // V8's CodeRange hint at 0x574c0000.
            bias = find_hole_for_elf(&header, ph);
        }

        if ((err = load_entry(ph[i], bias, fd)) < 0)
            goto beyond_hope;

        // load_addr is used to get a value for AX_PHDR et al
        if (!load_addr_set) {
            load_addr = bias + ph[i].vaddr - ph[i].offset;
            load_addr_set = true;
        }

        // we have to know where the brk starts
        addr_t brk = bias + ph[i].vaddr + ph[i].memsize;
        if (brk > current->mm->start_brk)
            current->mm->start_brk = current->mm->brk = BYTES_ROUND_UP(brk);
    }

    addr_t entry = bias + header.entry_point;
    addr_t interp_base = 0;

    if (interp_name) {
        // Map the ELF interpreter segments.
        interp_base = find_hole_for_elf(&interp_header, interp_ph);
        for (int i = interp_header.phent_count - 1; i >= 0; i--) {
            if (interp_ph[i].type != PT_LOAD)
                continue;
            if ((err = load_entry(interp_ph[i], interp_base, interp_fd)) < 0)
                goto beyond_hope;
        }
        entry = interp_base + interp_header.entry_point;
    }


    // map vdso
    err = _ENOMEM;
    pages_t vdso_pages = sizeof(vdso_data) >> PAGE_BITS;
    // FIXME disgusting hack: musl's dynamic linker has a one-page hole, and
    // I'd rather not put the vdso in that hole. so find a two-page hole and
    // add one.
    page_t vdso_page = pt_find_hole(current->mem, vdso_pages + 1);
    if (vdso_page == BAD_PAGE)
        goto beyond_hope;
    vdso_page += 1;
    // Read + execute, never writable (GitHub issue #3): handlers return through
    // __kernel_rt_sigreturn in this page, and instruction fetches need P_READ.
    if ((err = pt_map(current->mem, vdso_page, vdso_pages, (void *) vdso_data, 0, P_READ | P_EXEC)) < 0)
        goto beyond_hope;
    mem_pt(current->mem, vdso_page)->data->name = "[vdso]";
    current->mm->vdso = vdso_page << PAGE_BITS;

    // map empty vvar pages alongside the VDSO
    page_t vvar_page = pt_find_hole(current->mem, VVAR_PAGES);
    if (vvar_page == BAD_PAGE)
        goto beyond_hope;
    if ((err = pt_map_nothing(current->mem, vvar_page, VVAR_PAGES, 0)) < 0)
        goto beyond_hope;
    mem_pt(current->mem, vvar_page)->data->name = "[vvar]";

    // STACK TIME!

    // ARM64: stack near top of 48-bit address space
    if ((err = pt_map_nothing(current->mem, STACK_INIT_PAGE, 1, stack_prot)) < 0)
        goto beyond_hope;
    if ((err = pt_map_nothing(current->mem, STACK_TOP_PAGE, 1, P_READ)) < 0)
        goto beyond_hope;
    write_wrunlock(&current->mem->lock);
    addr_t sp = STACK_TOP_ADDR;
    // 64-bit Linux leaves 8 empty bytes at the bottom of the initial stack.
    sp -= sizeof(void *);

    err = _EFAULT;
    // first, copy stuff pointed to by argv/envp/auxv
    // filename, argc, argv
    // Note: lock is already released at this point
    addr_t file_addr = sp = copy_string(sp, file);
    if (sp == 0)
        goto out_free_interp;
    current->mm->env_end = sp;
    addr_t envp_addr = sp = args_copy(sp, envp);
    if (sp == 0)
        goto out_free_interp;
    current->mm->env_start = sp;
    current->mm->argv_end = sp;
    addr_t argv_addr = sp = args_copy(sp, argv);
    if (sp == 0)
        goto out_free_interp;
    current->mm->argv_start = sp;
    sp = align_stack(sp);

    addr_t platform_addr = sp = copy_string(sp, "aarch64");
    if (sp == 0)
        goto out_free_interp;
    // 16 random bytes so no system call is needed to seed a userspace RNG
    char random[16] = {};
    get_random(random, sizeof(random)); // if this fails, eh, no one's really using it
    addr_t random_addr = sp -= sizeof(random);
    if (user_put(sp, random))
        goto out_free_interp;

    // the way linux aligns the stack at this point is kinda funky
    // calculate how much space is needed for argv, envp, and auxv, subtract
    // that from sp, then align, then copy argv/envp/auxv from that down

    // declare elf aux now so we can know how big it is
    struct aux_ent aux[] = {
        // The internal VDSO supplies sigreturn trampolines, but it is not a
        // complete aarch64 Linux VDSO for userspace dynamic loaders to parse.
        {AX_SYSINFO_EHDR, 0},
        // See guest_hwcap(): validated families only; LSE and SHA512 stay
        // hidden until separately validated.
        {AX_HWCAP, guest_hwcap()},
        {AX_PAGESZ, PAGE_SIZE},
        {AX_CLKTCK, 0x64},
        {AX_PHDR, load_addr + header.prghead_off},
        {AX_PHENT, sizeof(exec_prg_header)},
        {AX_PHNUM, header.phent_count},
        {AX_BASE, interp_base},
        {AX_FLAGS, 0},
        {AX_ENTRY, bias + header.entry_point},
        {AX_UID, userns_view_uid(current->userns, current->uid)},
        {AX_EUID, userns_view_uid(current->userns, ids.euid)},
        {AX_GID, userns_view_gid(current->userns, current->gid)},
        {AX_EGID, userns_view_gid(current->userns, ids.egid)},
        {AX_SECURE, ids.secure || current->uid != ids.euid || current->gid != ids.egid},
        {AX_RANDOM, random_addr},
        {AX_HWCAP2, 0}, // suck that too
        {AX_EXECFN, file_addr},
        {AX_PLATFORM, platform_addr},
        {0, 0}
    };
    sp -= ((argv.count + 1) + (envp.count + 1) + 1) * ELF_PTR_SIZE;
    sp -= sizeof(aux);
    sp &=~ 0xf;

    // now copy down, start using p so sp is preserved
    addr_t p = sp;

    // argc
    {uint64_t argc64 = argv.count; if (user_put(p, argc64)) return _EFAULT;}
    p += ELF_PTR_SIZE;

    // argv
    size_t argc = argv.count;
    while (argc-- > 0) {
        {uint64_t ptr64 = argv_addr; if (user_put(p, ptr64)) return _EFAULT;}
        argv_addr += user_strlen(argv_addr) + 1;
        p += ELF_PTR_SIZE;
    }
    p += ELF_PTR_SIZE; // null terminator

    // envp
    size_t envc = envp.count;
    while (envc-- > 0) {
        {uint64_t ptr64 = envp_addr; if (user_put(p, ptr64)) return _EFAULT;}
        envp_addr += user_strlen(envp_addr) + 1;
        p += ELF_PTR_SIZE;
    }
    p += ELF_PTR_SIZE; // null terminator

    // copy auxv
    current->mm->auxv_start = p;
    if (user_put(p, aux))
        goto beyond_hope;
    p += sizeof(aux);
    current->mm->auxv_end = p;

    current->mm->stack_start = sp;
    current->cpu.sp = sp;
    current->cpu.pc = entry;
    // Zero all general-purpose registers
    memset(current->cpu.regs, 0, sizeof(current->cpu.regs));
    current->cpu.nzcv = 0;
    current->cpu.nf = 0;
    current->cpu.zf = 0;
    current->cpu.cf = 0;
    current->cpu.vf = 0;
    current->cpu.fpcr = 0;
    current->cpu.fpsr = 0;

    err = 0;
out_free_interp:
    if (interp_name != NULL)
        free(interp_name);
    if (interp_fd != NULL && !IS_ERR(interp_fd))
        fd_close(interp_fd);
    if (interp_ph != NULL)
        free(interp_ph);
out_free_ph:
    free(ph);
    return err;

beyond_hope:
    // TODO force sigsegv
    write_wrunlock(&current->mem->lock);
    goto out_free_interp;
}

static size_t args_size(struct exec_args args) {
    const char *args_end = args.args;
    for (size_t i = 0; i < args.count; i++) {
        args_end += strlen(args_end) + 1;
    }
    // don't forget the very last null terminator
    assert(args_end[0] == '\0');
    args_end++;
    return args_end - args.args;
}

static inline addr_t align_stack(addr_t sp) {
    return sp &~ (addr_t)0xf;
}

static inline addr_t copy_string(addr_t sp, const char *string) {
    sp -= strlen(string) + 1;
    if (user_write_string(sp, string))
        return 0;
    return sp;
}

static inline addr_t args_copy(addr_t sp, struct exec_args args) {
    size_t size = args_size(args);
    sp -= size;
    if (user_write(sp, args.args, size))
        return 0;
    return sp;
}

static inline ssize_t user_strlen(addr_t p) {
    size_t i = 0;
    char c;
    do {
        if (user_get(p + i, c))
            return -1;
        i++;
    } while (c != '\0');
    return i - 1;
}

static inline int user_memset(addr_t start, byte_t val, dword_t len) {
    while (len--)
        if (user_put(start++, val))
            return 1;
    return 0;
}

static int format_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp, struct exec_ids ids) {
    int err = elf_exec(fd, file, argv, envp, ids);
    if (err != _ENOEXEC)
        return err;
    // other formats would go here
    return _ENOEXEC;
}

// Linux (binfmt_script) lets the interpreter itself be a script, up to
// BINPRM_MAX_RECURSION (4) levels; deeper chains fail with ELOOP. Kali's
// msfconsole is one: #!/usr/share/metasploit-framework/ruby, a /bin/sh wrapper.
#define SHEBANG_MAX_DEPTH 4
static int shebang_exec_depth(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp, bool inaccessible, int depth);
static int shebang_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp, bool inaccessible) {
    return shebang_exec_depth(fd, file, argv, envp, inaccessible, 0);
}
static int shebang_exec_depth(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp, bool inaccessible, int depth) {
    if (depth > SHEBANG_MAX_DEPTH)
        return _ELOOP;
    // read the first 128 bytes to get the shebang line out of
    char header[128];
    int size = fd->ops->pread(fd, header, sizeof(header) - 1, 0);
    if (size < 0)
        return _EIO;
    header[size] = '\0';

    // only look at the first line
    char *newline = strchr(header, '\n');
    if (newline == NULL)
        return _ENOEXEC;
    *newline = '\0';

    // format: #![spaces]interpreter[spaces]argument[spaces]
    char *p = header;
    if (p[0] != '#' || p[1] != '!')
        return _ENOEXEC;
    // A CLOEXEC source descriptor disappears before the script interpreter
    // can open /dev/fd/N[/path]. Linux returns ENOENT before replacing mm.
    if (inaccessible)
        return _ENOENT;
    p += 2;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return _ENOEXEC;

    char *interpreter = p;
    while (*p != ' ' && *p != '\t' && *p != '\0')
        p++;
    if (*p != '\0') {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
    }

    char *argument = p;
    // Strip trailing spaces from the optional interpreter argument without
    // walking back into the interpreter string when no argument is present.
    char *end = argument + strlen(argument);
    while (end > argument && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    if (*argument == '\0')
        argument = NULL;

    struct exec_args argv_rest = {
        .count = argv.count - 1,
        .args = argv.args + strlen(argv.args) + 1,
    };
    size_t args_rest_size = args_size(argv_rest);
    size_t extra_args_size = strlen(interpreter) + 1 + strlen(file) + 1;
    if (argument)
        extra_args_size += strlen(argument) + 1;
    if (args_rest_size + extra_args_size >= ARGV_MAX)
        return _E2BIG;

    // Heap, not stack: a 128 KiB buffer per nested interpreter level overflowed
    // the host thread's stack (5 levels > 512 KiB) and crashed the runtime.
    char *new_argv_buf = malloc(ARGV_MAX);
    if (new_argv_buf == NULL)
        return _ENOMEM;
    struct exec_args new_argv = {.args = new_argv_buf};
    size_t n = 0;
    strcpy(new_argv_buf, interpreter);
    new_argv.count++;
    n += strlen(interpreter) + 1;
    if (argument) {
        strcpy(new_argv_buf + n, argument);
        new_argv.count++;
        n += strlen(argument) + 1;
    }
    strcpy(new_argv_buf + n, file);
    n += strlen(file) + 1;
    new_argv.count++;
    memcpy(new_argv_buf + n, argv_rest.args, args_rest_size);
    new_argv.count += argv_rest.count;

    struct fd *interpreter_fd = generic_open_execat(AT_PWD, interpreter, 0);
    if (IS_ERR(interpreter_fd)) {
        free(new_argv_buf);
        return PTR_ERR(interpreter_fd);
    }
    struct statbuf interpreter_stat;
    int err = interpreter_fd->mount->fs->fstat(interpreter_fd, &interpreter_stat);
    if (!err && (!S_ISREG(interpreter_stat.mode) || !(interpreter_stat.mode & 0111) ||
            interpreter_fd->mount->flags & MS_NOEXEC_))
        err = _EACCES;
    if (!err) err = access_check(&interpreter_stat, AC_X);
    if (!err) err = format_exec(interpreter_fd, interpreter, new_argv, envp,
            (struct exec_ids) {current->euid, current->egid});
    // The interpreter is a script too: run it the same way, one level deeper.
    if (err == _ENOEXEC)
        err = shebang_exec_depth(interpreter_fd, interpreter, new_argv, envp, false, depth + 1);
    fd_close(interpreter_fd);
    free(new_argv_buf);
    return err;
}

// The caller owns fd. Reads use pread so executing an open descriptor does
// not change its shared open-file-description offset (or depend on its path).
static int do_execve_fd(struct fd *fd, const char *file, struct exec_args argv,
        struct exec_args envp, bool script_inaccessible) {
    struct statbuf stat;
    int err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0)
        return err;
    if (!S_ISREG(stat.mode) || !(stat.mode & 0111) || fd->mount->flags & MS_NOEXEC_)
        return _EACCES;
    if ((err = access_check(&stat, AC_X)) < 0)
        return err;

    struct exec_ids ids = {current->euid, current->egid};
    struct file_caps filecaps = {0};
    bool honor_filecaps = !(fd->mount->flags & MS_NOSUID_) && !current->ptrace.traced && !current->userns;
    if (honor_filecaps) {
        err = file_caps_read(fd, &filecaps);
        if (err) return err;
    }
    // Set-ID bits are not honoured inside a user namespace (owners may be unmapped).
    if (!(fd->mount->flags & MS_NOSUID_) && !current->ptrace.traced && !current->no_new_privs && !current->userns) {
        if (stat.mode & S_ISUID) ids.euid = stat.uid;
        if ((stat.mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
            ids.egid = stat.gid;
    }
    uint64_t permitted = (current->cap_inheritable & filecaps.inheritable) |
            (filecaps.permitted & current->cap_bounding);
    bool effective = filecaps.effective;
    // Traditional root semantics, except a setuid-root executable with explicit
    // file capabilities and a non-root real uid receives only those grants.
    if ((current->uid == 0 || ids.euid == 0) &&
            !(filecaps.present && current->uid != 0 && ids.euid == 0)) {
        permitted = current->cap_bounding | current->cap_inheritable;
        effective = ids.euid == 0;
    }
    if (current->no_new_privs || current->ptrace.traced) permitted &= current->cap_permitted;
    // A capability-dumb binary marked effective must receive its full file
    // permitted set, or fail exec instead of running with unexpected authority.
    if (filecaps.effective && !current->no_new_privs &&
            (filecaps.permitted & ~permitted)) return _EPERM;
    ids.secure = (permitted & ~current->cap_permitted) != 0;
    struct fdtable *displaced = NULL;
    err = fdtable_unshare_current(&displaced);
    if (err) return err;
    err = format_exec(fd, file, argv, envp, ids);
    if (err == _ENOEXEC) {
        // Linux ignores set-ID mode bits on scripts.
        ids = (struct exec_ids) {current->euid, current->egid};
        permitted = current->euid == 0 ? current->cap_bounding | current->cap_inheritable : 0;
        if (current->no_new_privs || current->ptrace.traced) permitted &= current->cap_permitted;
        effective = current->euid == 0;
        err = shebang_exec(fd, file, argv, envp, script_inaccessible);
    }
    if (err < 0) {
        fdtable_restore_current(displaced);
        return err;
    }
    if (displaced) fdtable_release(displaced);
    // A successful ordinary exec restores dumpability; privilege-changing or
    // unreadable executable images use the conservative suid_dumpable=0 policy.
    mm_set_dumpable(current->mm, ids.secure || ids.euid != current->euid || ids.egid != current->egid ||
        access_check(&stat, AC_R) < 0 ? 0 : 1);
    // Linux clears the parent-death signal when exec changes credentials.
    if (ids.secure || ids.euid != current->euid || ids.egid != current->egid) current->pdeath_signal = 0;
    // Saved IDs become the effective IDs after successful exec. Auxv was
    // already constructed with these same prospective IDs for the loader.
    current->euid = current->suid = ids.euid;
    current->egid = current->sgid = ids.egid;
    // Recompute guest sets on every exec; these never grant host capabilities.
    if (current->userns) userns_exec_capabilities();
    else {
        current->cap_permitted = permitted;
        current->cap_effective = effective ? permitted : 0;
    }
    current->keep_caps = false;

    // save current->comm
    lock(&current->general_lock);
    const char *basename = strrchr(file, '/');
    if (basename == NULL)
        basename = file;
    else
        basename++;
    strncpy(current->comm, basename, sizeof(current->comm));
    unlock(&current->general_lock);

    update_thread_name();

    // cloexec
    // consider putting this in fd.c?
    fdtable_do_cloexec(current->files);

    // reset signal handlers
    lock(&current->sighand->lock);
    for (int sig = 0; sig < NUM_SIGS; sig++) {
        struct sigaction_ *action = &current->sighand->action[sig];
        if (action->handler != SIG_IGN_)
            action->handler = SIG_DFL_;
    }
    current->altstack = 0;
    current->altstack_size = 0;
    unlock(&current->sighand->lock);

    current->rseq_addr = 0;
    current->rseq_len = 0;
    current->rseq_sig = 0;
    current->rseq_registered = false;
    current->did_exec = true;
    runtime_output_exec_reset();
    runtime_process_event("exec_committed",current->tgid,0);
    vfork_notify(current);

    if (current->ptrace.traced) {
        lock(&current->ptrace.lock);
        bool event = current->ptrace.options & PTRACE_O_TRACEEXEC_;
        unlock(&current->ptrace.lock);
        if (event) {
            // PTRACE_EVENT_EXEC stop; GETEVENTMSG gives the former thread id.
            ptrace_stop_current(SIGTRAP_ | PTRACE_EVENT_EXEC_ << 8, (uint64_t) current->pid);
        } else {
            lock(&pids_lock);
            send_signal(current, SIGTRAP_, (struct siginfo_) {
                .code = SI_USER_,
                .kill.pid = current->pid,
                .kill.uid = current->uid,
            });
            unlock(&pids_lock);
        }
    }

    return 0;
}

int __do_execve(const char *file, struct exec_args argv, struct exec_args envp) {
    struct fd *fd = generic_open_execat(AT_PWD, file, 0);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    int err = do_execve_fd(fd, file, argv, envp, false);
    fd_close(fd);
    return err;
}

int do_execve(const char *file, size_t argc, const char *argv_p, const char *envp_p) {
    struct exec_args argv = {.count = argc, .args = argv_p};
    struct exec_args envp = {.args = envp_p};
    while (*envp_p != '\0') {
        envp_p += strlen(envp_p) + 1;
        envp.count++;
    }
    return __do_execve(file, argv, envp);
}

static ssize_t user_read_string_array(addr_t addr, char *buf, size_t max) {
    size_t i = 0;
    size_t p = 0;
    for (;;) {
        uint64_t str_addr64;
        if (user_get(addr + i * ELF_PTR_SIZE, str_addr64))
            return _EFAULT;
        addr_t str_addr = (addr_t) str_addr64;
        if (str_addr == 0)
            break;
        size_t str_p = 0;
        for (;;) {
            if (p >= max)
                return _E2BIG;
            if (user_get(str_addr + str_p, buf[p]))
                return _EFAULT;
            str_p++;
            p++;
            if (buf[p - 1] == '\0')
                break;
        }
        i++;
    }
    if (p >= max)
        return _E2BIG;
    buf[p] = '\0';
    return i;
}

static dword_t exec_user_args(const char *filename, struct fd *executable,
        bool script_inaccessible, addr_t argv_addr, addr_t envp_addr) {
    int err = _ENOMEM;
    char *argv = malloc(ARGV_MAX);
    if (argv == NULL)
        goto err_free_argv;
    ssize_t argc = argv_addr ? user_read_string_array(argv_addr, argv, ARGV_MAX) : 0;
    if (argc < 0) {
        err = argc;
        goto err_free_argv;
    }

    if (argc == 0) {
        // Match Linux empty-argv normalization and keep shebang argv_rest
        // from underflowing when the caller supplied NULL/an empty vector.
        argv[0] = argv[1] = '\0';
        argc = 1;
    }

    char *envp = malloc(ARGV_MAX);
    if (envp == NULL)
        goto err_free_envp;
    if (envp_addr != 0) {
        err = user_read_string_array(envp_addr, envp, ARGV_MAX);
        if (err < 0)
            goto err_free_envp;
    } else {
        // Do not take advantage of this nonstandard and nonportable misfeature!
        // - Michael Kerrisk, execve(2)
        envp[0] = envp[1] = '\0';
    }

    STRACE("execve(\"%.1000s\", {", filename);
    const char *args = argv;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, {");
    args = envp;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("})");

    // Force-inject environment variables into every execve'd process.
    // mode=0: inject only if not present, mode=1: replace existing value
    static const struct { const char *kv; size_t prefix_len; int mode; } inject_envs[] = {
        { "PYTHONMALLOC=malloc", 13, 1 },      // FORCE bypass pymalloc arenas (mode=1: always replace)
        { "NO_COLOR=1", 9, 0 },                // Disable color output
        { "PIP_PROGRESS_BAR=off", 17, 0 },     // Disable pip progress bar
        { "PYTHONDONTWRITEBYTECODE=1", 25, 0 }, // Skip .pyc generation to reduce allocs
        // Go async preemption sends SIGURG and reads/modifies mcontext at a
        // fixed offset. Our JIT delivers signals at gadget boundaries, not at
        // the exact interrupted instruction, so the PC in the signal frame is
        // imprecise. cpu_poke reduces latency but can't fix PC precision.
        // Cooperative preemption (function-call checkpoints) works correctly.
        { "GODEBUG=asyncpreemptoff=1", 8, 0 }, // Disable Go async preemption
        // Limit Go threads to reduce TLB stale-pointer crashes from concurrent
        // page table modifications. GOMAXPROCS=2 is sufficient for most Go CLI
        // tools and eliminates nearly all multi-thread race conditions.
        { "GOMAXPROCS=2", 11, 0 },             // Limit Go thread count
        // JavaScriptCore's concurrent/parallel GC uses timer-driven background
        // work plus signal-suspend handshakes for marker threads. iSH delivers
        // guest signals at syscall/gadget boundaries, so marker suspension can
        // spin forever when a target thread is parked in futex/syscall context;
        // timer-driven Bun event-loop callbacks can also stall behind concurrent
        // GC. Keep GC enabled, but make it serial/non-concurrent in the guest.
        { "JSC_numberOfGCMarkers=1", 22, 0 },  // Avoid multi-marker GC suspend hangs
        { "JSC_useConcurrentGC=0", 19, 0 },     // Keep Bun timers/server progress reliable
        // New Bun releases disable JSC_* environment options and instead
        // apply BUN_JSC_* during engine initialisation. Keep both versions safe.
        { "BUN_JSC_numberOfGCMarkers=1", 26, 0 },
        { "BUN_JSC_useConcurrentGC=0", 23, 0 },
    };
    for (size_t vi = 0; vi < sizeof(inject_envs)/sizeof(inject_envs[0]); vi++) {
        char *e = envp;
        char *found_at = NULL;
        while (*e != '\0') {
            if (strncmp(e, inject_envs[vi].kv, inject_envs[vi].prefix_len) == 0)
                found_at = e;
            e += strlen(e) + 1;
        }
        size_t total_used = (e - envp) + 1; // includes final \0
        if (found_at != NULL && inject_envs[vi].mode == 1) {
            // Replace: remove old value, then append new one
            size_t old_len = strlen(found_at) + 1;
            size_t tail = total_used - (found_at - envp) - old_len;
            memmove(found_at, found_at + old_len, tail);
            total_used -= old_len;
            found_at = NULL; // treat as not found so it gets appended
        }
        if (found_at == NULL) {
            size_t inject_len = strlen(inject_envs[vi].kv) + 1;
            if (total_used + inject_len + 1 <= ARGV_MAX) {
                memcpy(envp + total_used - 1, inject_envs[vi].kv, inject_len);
                envp[total_used - 1 + inject_len] = '\0';
            }
        }
    }

    // Inject V8 flags for Node.js to work around scope corruption in emulation.
    // --jitless: disable JIT (avoids V8 code generation incompatible with our JIT)
    // --predictable: disable concurrent GC/compilation (avoids race conditions)
    // --no-lazy: eager compilation (avoids Zone reuse patterns that corrupt scopes)
    // --single-generation: skip young generation (reduces GC-triggered Zone resets)
    // --lite-mode: reduces Zone churn by disabling feedback vectors & optimizer
    // --no-compilation-cache: prevent stale cached compilations from reusing Zones
    // --no-flush-bytecode: keep compiled bytecode alive, avoid recompilation churn
    // --no-lazy-compile-dispatcher: single-thread parsing to reduce concurrent Zones
    // --no-parallel-compile-tasks: single-thread compilation to reduce concurrent Zones
    // --no-concurrent-recompilation: disable background recompilation
    {
        const char *base = strrchr(filename, '/');
        base = base ? base + 1 : filename;
        if (strcmp(base, "node") == 0) {
            bool skip_node_arg_injection = false;
            for (char *e = envp; *e != '\0'; e += strlen(e) + 1) {
                if (strcmp(e, "ISH_NODE_NO_ARG_INJECTION=1") == 0) {
                    skip_node_arg_injection = true;
                    break;
                }
            }
            if (skip_node_arg_injection)
                goto node_arg_injection_done;

            static const char *inject_args_base[] = {
                "--jitless",
                "--no-lazy",
                "--max-old-space-size=512",
            };
            // Conditionally inject --require for polyfill files that exist in guest fs
            static const char *optional_requires[] = {
                "--require=/lib/wasm-polyfill.js",    // WebAssembly shim (must load first)
                "--require=/lib/fetch-polyfill.js",   // fetch() via native http/https
            };
            const char *inject_args[8]; // base args + optional requires
            size_t inject_count = 0;
            for (size_t i = 0; i < sizeof(inject_args_base)/sizeof(inject_args_base[0]); i++)
                inject_args[inject_count++] = inject_args_base[i];
            for (size_t i = 0; i < sizeof(optional_requires)/sizeof(optional_requires[0]); i++) {
                // Extract path after "--require="
                const char *path = optional_requires[i] + 10; // strlen("--require=")
                struct fd *fd = generic_open(path, O_RDONLY_, 0);
                if (!IS_ERR(fd)) {
                    fd_close(fd);
                    inject_args[inject_count++] = optional_requires[i];
                }
            }
            for (size_t ai = 0; ai < inject_count; ai++) {
                const char *arg = inject_args[ai];
                size_t arg_len = strlen(arg) + 1; // includes NUL

                // Find end of argv buffer
                char *p = argv;
                while (*p != '\0')
                    p += strlen(p) + 1;
                size_t total_used = (p - argv) + 1;

                // Check if already present
                bool found = false;
                char *q = argv;
                while (*q != '\0') {
                    if (strcmp(q, arg) == 0) { found = true; break; }
                    q += strlen(q) + 1;
                }

                if (!found && total_used + arg_len + 1 <= ARGV_MAX) {
                    char *after_argv0 = argv + strlen(argv) + 1;
                    size_t tail = total_used - (after_argv0 - argv);
                    memmove(after_argv0 + arg_len, after_argv0, tail);
                    memcpy(after_argv0, arg, arg_len);
                    argc++;
                }
            }
node_arg_injection_done: ;
        }

        // Inject LD_PRELOAD for zero_malloc.so to zero large malloc/free.
        // V8's Zone allocator reuses freed memory without zeroing, causing
        // stale pointer dereferences. This library zeros blocks >= 4096 bytes
        // on both malloc and free (Zone segments are >= 8KB).
        if (strcmp(base, "node") == 0) {
            struct fd *zero_free_fd = generic_open("/lib/zero_free.so", O_RDONLY_, 0);
            if (!IS_ERR(zero_free_fd)) {
                fd_close(zero_free_fd);
                static const char *ld_preload = "LD_PRELOAD=/lib/zero_free.so";
                size_t env_len = strlen(ld_preload) + 1;

                // Check if LD_PRELOAD is already set
                bool found = false;
                char *q = envp;
                while (*q != '\0') {
                    if (strncmp(q, "LD_PRELOAD=", 11) == 0) { found = true; break; }
                    q += strlen(q) + 1;
                }
                if (!found) {
                    char *p = envp;
                    while (*p != '\0')
                        p += strlen(p) + 1;
                    size_t total_used = (p - envp) + 1;
                    if (total_used + env_len + 1 <= ARGV_MAX) {
                        memcpy(p, ld_preload, env_len);
                        p[env_len] = '\0';
                    }
                }
            }
        }
    }

    // Native offload: check if this binary should run natively on the host
    bool offload_is_generic = false;
    // Descriptor-based execution must execute that object, never a pathname
    // substituted by native offload (the object may have been unlinked).
    const char *native_path = executable ? NULL :
        native_offload_lookup_exec(filename, envp, &offload_is_generic);
    if (native_path && offload_is_generic) {
        // A system-path shell wrapper is still the user's program. Missing
        // files may offload: the app's built-in command need not exist on disk.
        struct fd *probe = generic_open(filename, O_RDONLY_ | O_NONBLOCK_, 0);
        if (!IS_ERR(probe)) {
            struct statbuf stat;
            char magic[2];
            if (probe->mount->fs->fstat(probe, &stat) == 0 && S_ISREG(stat.mode) &&
                    probe->ops->read && probe->ops->read(probe, magic, sizeof(magic)) == 2 &&
                    magic[0] == '#' && magic[1] == '!')
                native_path = NULL;
            fd_close(probe);
        }
    }
    if (native_path) {
        // native_offload_exec calls do_exit() on success, which unwinds via
        // pthread_exit() and skips the err_free_{argv,envp} labels below.
        // Register pthread cleanup handlers so the ARGV_MAX-sized buffers are
        // freed on that unwind path; without them each native-offloaded exec
        // (ffmpeg, minis-open, …) leaks ~2 × ARGV_MAX per process.
        pthread_cleanup_push(free, envp);
        pthread_cleanup_push(free, argv);
        err = native_offload_exec(native_path, filename, argc, argv, envp);
        // Only reach here on fallback. Do not execute cleanups — the normal
        // err_free_{argv,envp} path below will free them.
        pthread_cleanup_pop(0);
        pthread_cleanup_pop(0);
        if (err == 0) {
            // Should not reach here (do_exit doesn't return), but just in case
            goto err_free_envp;
        }
        // Fall through to emulated exec on failure
        printk("native_offload: fallback to emulated exec for %s\n", filename);
    }

    if (executable) {
        struct exec_args argv_args = {.count = argc, .args = argv};
        struct exec_args envp_args = {.args = envp};
        for (const char *p = envp; *p; p += strlen(p) + 1) envp_args.count++;
        err = do_execve_fd(executable, filename, argv_args, envp_args, script_inaccessible);
    } else {
        err = do_execve(filename, argc, argv, envp);
    }

err_free_envp:
    free(envp);
err_free_argv:
    free(argv);
    return err;
}


dword_t sys_execve(addr_t filename_addr, addr_t argv_addr, addr_t envp_addr) {
    char filename[MAX_PATH];
    if (user_read_string(filename_addr, filename, sizeof(filename)))
        return _EFAULT;
    return exec_user_args(filename, NULL, false, argv_addr, envp_addr);
}

dword_t sys_execveat(fd_t dirfd, addr_t filename_addr, addr_t argv_addr,
        addr_t envp_addr, int_t flags) {
    if (flags & ~(AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_))
        return _EINVAL;
    char filename[MAX_PATH];
    if (user_read_string(filename_addr, filename, sizeof(filename)))
        return _EFAULT;
    if (!filename[0] && !(flags & AT_EMPTY_PATH_))
        return _ENOENT;

    struct fd *base = AT_PWD;
    bool source_cloexec = false;
    bool use_descriptor = filename[0] != '/' && dirfd != AT_FDCWD_;
    if (use_descriptor) {
        lock(&current->files->lock);
        base = fdtable_get(current->files, dirfd);
        if (base) {
            fd_retain(base);
            source_cloexec = bit_test(dirfd, current->files->cloexec);
        }
        unlock(&current->files->lock);
        if (!base) return _EBADF;
    }
    struct fd *fd;
    if (!filename[0]) {
        // AT_EMPTY_PATH with AT_FDCWD refers to the cwd, not an executable.
        if (!use_descriptor) return _EACCES;
        fd = fd_retain(base);
    } else if (use_descriptor && !S_ISDIR(base->type)) {
        fd_close(base);
        return _ENOTDIR;
    } else {
        fd = generic_open_execat(base, filename,
                (flags & AT_SYMLINK_NOFOLLOW_) ? O_NOFOLLOW_ : 0);
    }
    if (use_descriptor) fd_close(base);
    if (IS_ERR(fd)) return PTR_ERR(fd);

    char exec_name[MAX_PATH];
    int n = use_descriptor ? (filename[0] ?
        snprintf(exec_name, sizeof(exec_name), "/dev/fd/%d/%s", dirfd, filename) :
        snprintf(exec_name, sizeof(exec_name), "/dev/fd/%d", dirfd)) :
        snprintf(exec_name, sizeof(exec_name), "%s", filename);
    if (n < 0 || (size_t)n >= sizeof(exec_name)) {
        fd_close(fd);
        return _ENAMETOOLONG;
    }
    int err = exec_user_args(exec_name, fd, use_descriptor && source_cloexec,
            argv_addr, envp_addr);
    fd_close(fd);
    return err;
}
