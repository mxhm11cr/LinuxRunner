#include <string.h>
#include <stdatomic.h>
#include "debug.h"
#include "kernel/calls.h"
#include "fs/dmabuf.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/ipc.h"
#include <errno.h>
extern void (*runtime_memory_failure)(const char *, uint64_t, uint64_t, int, int, int);

#if ANON_MMAP_LIMIT_PAGES > 0
_Atomic long anon_page_count;

bool anon_pages_reserve(long pages) {
    long count = atomic_load(&anon_page_count);
    do {
        if (pages > ANON_MMAP_LIMIT_PAGES || count > ANON_MMAP_LIMIT_PAGES - pages) {
            if (runtime_memory_failure)
                runtime_memory_failure("anonymous page budget", 0, (uint64_t)pages * PAGE_SIZE, 0, 0, ENOMEM);
            printk("MEMORY_BUDGET: tid=%d requested-pages=%ld committed-pages=%ld limit-pages=%d\n", current->pid, pages, count, ANON_MMAP_LIMIT_PAGES);
            return false;
        }
    } while (!atomic_compare_exchange_weak(&anon_page_count, &count, count + pages));
    return true;
}

void anon_pages_unreserve(long pages) {
    atomic_fetch_sub(&anon_page_count, pages);
}
#endif

struct mm *mm_new() {
    struct mm *mm = malloc(sizeof(struct mm));
    if (mm == NULL)
        return NULL;
    mem_init(&mm->mem);
    mm->start_brk = mm->brk = 0; // should get overwritten by exec
    mm->exefile = NULL;
    mm->sysv_shm = NULL;
    atomic_init(&mm->dumpable, 1);
    mm->refcount = 1;
    return mm;
}

struct mm *mm_copy(struct mm *mm) {
    struct mm *new_mm = malloc(sizeof(struct mm));
    if (new_mm == NULL)
        return NULL;
    write_wrlock(&mm->mem.lock);
    *new_mm = *mm;
    new_mm->sysv_shm = NULL;
    // Fix wrlock_init failing because it thinks it's reinitializing the same lock
    memset(&new_mm->mem.lock, 0, sizeof(new_mm->mem.lock));
    new_mm->refcount = 1;
    mem_init(&new_mm->mem);
    fd_retain(new_mm->exefile);
    if (ipc_shm_mm_copy(mm, new_mm) < 0) {
        write_wrunlock(&mm->mem.lock);
        mm_release(new_mm);
        return NULL;
    }
    pt_copy_on_write(&mm->mem, &new_mm->mem, 0, MEM_PAGES);
    write_wrunlock(&mm->mem.lock);
    return new_mm;
}

void mm_set_dumpable(struct mm *mm, unsigned value) {
    if (!mm) return;
    // Serialize the field with the address-space snapshot in fork.
    write_wrlock(&mm->mem.lock);
    atomic_store(&mm->dumpable, value);
    write_wrunlock(&mm->mem.lock);
}

void mm_retain(struct mm *mm) {
    mm->refcount++;
}

void mm_release(struct mm *mm) {
    if (--mm->refcount == 0) {
        ipc_shm_mm_release(mm);
        if (mm->exefile != NULL)
            fd_close(mm->exefile);
        mem_destroy(&mm->mem);
        free(mm);
    }
}

static addr_t do_mmap(addr_t addr, uint64_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    int err;
    if (!len) return _EINVAL;
    if (len > UINT64_MAX - (PAGE_SIZE - 1)) return _ENOMEM;
    pages_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
#ifdef GUEST_ARM64
    if (pages > USER_ADDR_MAX_PAGE + 1) return _ENOMEM;
#endif
    page_t page;
    bool caller_hint_used = false;
    if (addr != 0) {
        if (PGOFFSET(addr) != 0)
            return _EINVAL;
        page = PAGE(addr);
#ifdef GUEST_ARM64
        // ARM64 has a 48-bit guest page table, so high-address hints are
        // legitimate. Bun/JSC in particular reserves heap/cage regions at
        // high random hints and stores pointers derived from the returned
        // address. Falling back to a low address while the runtime still uses
        // high-derived metadata corrupts allocator freelists. Only reject
        // ranges outside the 48-bit user address space, and protect the low
        // 4GB stack gap from low-address hints.
        if (page > USER_ADDR_MAX_PAGE || pages > USER_ADDR_MAX_PAGE - page ||
                (page < 0x100000 && page + pages > STACK_TOP_PAGE)) {
            if (flags & MMAP_FIXED)
                return _ENOMEM;
            addr = 0;
            page = 0;
        }
#endif
        if (addr != 0 && !(flags & MMAP_FIXED) && !pt_is_hole(current->mem, page, pages))
            addr = 0;
#ifdef GUEST_ARM64
        if (addr != 0 && !(flags & MMAP_FIXED) && mem_range_has_reservation(current->mem, page, pages))
            addr = 0;
#endif
        if (addr != 0)
            caller_hint_used = true;
    }
    if (addr == 0) {
#ifdef GUEST_ARM64
        // Large no-reserve arenas (Bun/JSC heap cages, V8 code ranges, etc.)
        // should live in the 48-bit high address space instead of consuming
        // the scarce low 4GB mmap window. They are lazy reservations, so this
        // costs page-table metadata only as pages are touched.
        if ((flags & MMAP_NORESERVE) && pages >= 0x10000)
            page = pt_find_hole_high(current->mem, pages);
        else
#endif
        page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE) {
            struct fd *observed = !(flags & MMAP_ANONYMOUS) ? f_get_retain(fd_no) : NULL;
            fd_report_mapping_failure(observed, fd_no, "guest_address_space", addr, len,
                    offset, prot, flags, 0, _ENOMEM);
            if (observed) fd_close(observed);
            return _ENOMEM;
        }
    }

    if (flags & MMAP_SHARED)
        prot |= P_SHARED;

    if (flags & MMAP_ANONYMOUS) {
#ifdef GUEST_ARM64
        // Reuse demand-zero reservations for large private anonymous maps.
        // Allocators also reserve smaller PROT_NONE arenas, then enable access
        // with mprotect. Enabling access must not eagerly charge their entire
        // untouched virtual range against the materialized-page budget.
        // A shared map must retain one backing across fork, including pages
        // that neither process has touched yet. Private maps can materialize
        // zero pages independently and copy touched pages through normal CoW.
        if ((flags & MMAP_PRIVATE) &&
                (pages >= 0x10000 || (!(prot & P_RWX) && pages >= 0x1000))) {
            if ((flags & MMAP_NORESERVE) && !caller_hint_used) {
                pages_t align_pages = pages;
                if (align_pages > 0x40000) align_pages = 0x40000;
                page_t aligned = (page / align_pages) * align_pages;
                if (aligned >= MMAP_HOLE_END && pt_is_hole(current->mem, aligned, pages) && !mem_range_has_reservation(current->mem, aligned, pages))
                    page = aligned;
            }
            if (flags & MMAP_FIXED) {
                // MAP_FIXED replaces the requested interval, including old
                // lazy reservations. Do not retain their contents or flags.
                if ((err = pt_unmap_always(current->mem, page, pages)) < 0)
                    return err;
            } else if (!pt_is_hole(current->mem, page, pages) || mem_range_has_reservation(current->mem, page, pages)) {
                return _ENOMEM;
            }
            if ((err = pt_map_lazy(current->mem, page, pages, prot)) < 0)
                return err;
            return page << PAGE_BITS;
        }
#endif
        if ((err = pt_map_nothing(current->mem, page, pages, prot)) < 0)
            return err;
    } else {
        // fd must be valid
        // Keep the open description stable across concurrent close/dup/exec.
        struct fd *fd = f_get_retain(fd_no);
        if (fd == NULL)
            return _EBADF;
        bool maywrite = false;
        int access = __atomic_load_n(&fd->flags, __ATOMIC_RELAXED) & O_ACCMODE_;
        if (fd->ops->mmap == NULL)
            err = _ENODEV;
        else if (access == O_WRONLY_ ||
                ((flags & MMAP_SHARED) && (prot & P_WRITE) && access != O_RDWR_))
            err = _EACCES;
        // Regular-file offsets are signed 64-bit byte positions. Do not let
        // host off_t arithmetic wrap after preserving the guest's full offset.
        else if (S_ISREG(fd->type) && (offset > INT64_MAX ||
                pages * PAGE_SIZE > (uint64_t)INT64_MAX - offset))
            err = _EOVERFLOW;
        else {
            err = 0;
            if ((flags & MMAP_SHARED) && access == O_RDONLY_)
                prot |= P_NO_SHARED_WRITE;
            // memfd write seals (Linux shmem): no writable shared mapping, and a
            // read-only shared one can never be upgraded. Count upgradable shared
            // mappings before they exist so a concurrent F_SEAL_WRITE sees them.
            if (fd->memfd && (flags & MMAP_SHARED)) {
                lock(&fd->memfd->lock);
                if (fd->memfd->seals & F_SEAL_ANY_WRITE_) {
                    if (prot & P_WRITE) err = _EPERM;
                    prot |= P_NO_SHARED_WRITE;
                }
                if (err == 0 && !(prot & P_NO_SHARED_WRITE)) {
                    fd->memfd->maywrite_maps++;
                    maywrite = true;
                }
                unlock(&fd->memfd->lock);
            }
            if (err == 0)
                err = fd->ops->mmap(fd, current->mem, page, pages, offset, prot, flags);
            if (err < 0 && maywrite) {
                lock(&fd->memfd->lock);
                fd->memfd->maywrite_maps--;
                unlock(&fd->memfd->lock);
            }
        }
        if (err < 0) {
            fd_report_mapping_failure(fd, fd_no, "file_backend", page << PAGE_BITS,
                    len, offset, prot, flags, 0, err);
            fd_close(fd);
            return err;
        }
        // The mapping keeps the file's identity, not an open host descriptor.
        mem_pt(current->mem, page)->data->fd = fd_mapping_identity(fd);
        mem_pt(current->mem, page)->data->memfd_maywrite = maywrite;
        fd_close(fd);
        mem_pt(current->mem, page)->data->file_offset = offset;
    }
    return page << PAGE_BITS;
}

static addr_t mmap_common(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    STRACE("mmap(0x%llx, 0x%x, 0x%x, 0x%x, %d, 0x%llx)", (unsigned long long)addr, len, prot, flags, fd_no, (unsigned long long)offset);
    if (len == 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))
        return _EINVAL;

    write_wrlock(&current->mem->lock);
    addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    write_wrunlock(&current->mem->lock);
    return res;
}

addr_t sys_mmap2(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    return mmap_common(addr, len, prot, flags, fd_no, (qword_t)offset << PAGE_BITS);
}

#if defined(GUEST_ARM64)
// ARM64 mmap syscall: offset is passed directly (not shifted like mmap2)
// and takes 6 direct arguments.
addr_t sys_mmap64(addr_t addr, addr_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset) {
    STRACE("mmap64(0x%llx, 0x%llx, 0x%x, 0x%x, %d, 0x%llx)", (unsigned long long)addr, (unsigned long long)len, prot, flags, fd_no, (unsigned long long)offset);
    // Linux AArch64 mmap takes a byte offset aligned to the guest page size,
    // not the 16KiB host page size. realfs_mmap handles host alignment itself.
    if (PGOFFSET(offset) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))
        return _EINVAL;

    write_wrlock(&current->mem->lock);
    addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    write_wrunlock(&current->mem->lock);
    return res;
}
#endif

struct mmap_arg_struct {
    dword_t addr, len, prot, flags, fd, offset;
};

addr_t sys_mmap(addr_t args_addr) {
    struct mmap_arg_struct args;
    if (user_get(args_addr, args))
        return _EFAULT;
    return mmap_common(args.addr, args.len, args.prot, args.flags, args.fd, args.offset);
}

int_t sys_munmap(addr_t addr, addr_t len) {
    STRACE("munmap(0x%llx, 0x%llx)", (unsigned long long)addr, (unsigned long long)len);
    pages_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;
    write_wrlock(&current->mem->lock);
    int err = pt_unmap_always(current->mem, PAGE(addr), pages);
    write_wrunlock(&current->mem->lock);
    if (err < 0)
        return _EINVAL;
    return 0;
}

#define MREMAP_MAYMOVE_ 1
#define MREMAP_FIXED_ 2

addr_t sys_mremap(addr_t addr, dword_t old_len, dword_t new_len, dword_t flags) {
    STRACE("mremap(%#x, %#x, %#x, %d)", addr, old_len, new_len, flags);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (old_len == 0 || new_len == 0)
        return _EINVAL;
    if (flags & ~(MREMAP_MAYMOVE_ | MREMAP_FIXED_))
        return _EINVAL;
    if (flags & MREMAP_FIXED_) {
        FIXME("missing MREMAP_FIXED");
        return _EINVAL;
    }

    page_t start = PAGE(addr);
    pages_t old_pages = PAGE_ROUND_UP(old_len);
    pages_t new_pages = PAGE_ROUND_UP(new_len);
    addr_t result = addr;

    write_wrlock(&current->mem->lock);

    if (new_pages <= old_pages) {
        int err = pt_unmap(current->mem, start + new_pages, old_pages - new_pages);
        if (err < 0)
            result = _EFAULT;
        goto out;
    }

    struct pt_entry *entry = mem_pt(current->mem, start);
    if (entry == NULL) {
        result = _EFAULT;
        goto out;
    }
    // Linux udmabuf VMAs are VM_DONTEXPAND. Never replace a device buffer
    // with an anonymous copy when mremap asks to grow it.
    if (entry->data->fd && fd_is_software_dmabuf(entry->data->fd)) {
        result = _EFAULT;
        goto out;
    }
    dword_t pt_flags = entry->flags;
    for (page_t page = start; page < start + old_pages; page++) {
        entry = mem_pt(current->mem, page);
        if (entry == NULL || (entry->flags & ~P_COW) != (pt_flags & ~P_COW)) {
            result = _EFAULT;
            goto out;
        }
    }
    entry = mem_pt(current->mem, start);
    if (!(pt_flags & P_ANONYMOUS) && (pt_flags & P_SHARED) &&
            entry->data->fd && entry->data->fd->memfd && entry->data->fd->ops->mmap) {
        // Wayland shared-memory pools resize a live memfd mapping after the
        // sender extends the file. Re-map the SAME backing, never copy it to
        // anonymous pages: aliases, fork and shared futex identity must survive.
        // Ordinary file identities deliberately own no descriptor; those need
        // a separate reopen/lifetime design and are not silently approximated.
        struct pt_entry *first = mem_pt(current->mem, start);
        struct fd *file = first->data->fd;
        uint64_t offset = first->data->file_offset + first->offset - first->data->mapping_offset;
        if (start > USER_ADDR_MAX_PAGE || new_pages > USER_ADDR_MAX_PAGE + 1 - start ||
                offset > INT64_MAX || (uint64_t)new_pages * PAGE_SIZE > INT64_MAX - offset) {
            result = _ENOMEM;
            goto out;
        }
        for (pages_t i = 0; i < old_pages; i++) {
            struct pt_entry *part = mem_pt(current->mem, start + i);
            if (!part->data->fd || part->data->fd->memfd != file->memfd ||
                    part->data->file_offset + part->offset - part->data->mapping_offset != offset + (uint64_t)i * PAGE_SIZE) {
                result = _EFAULT;
                goto out;
            }
        }
        page_t destination = start;
        if (!pt_is_hole(current->mem, start + old_pages, new_pages - old_pages)) {
            if (!(flags & MREMAP_MAYMOVE_) || (destination = pt_find_hole(current->mem, new_pages)) == BAD_PAGE) {
                result = _ENOMEM;
                goto out;
            }
        }
        // An in-place pt_map releases the old data object. Pin its description
        // first; transfer that reference to the new mapping only on success.
        fd_retain(file);
        // The replacement mapping stays counted for memfd write seals; the
        // released old data object drops its own count.
        bool maywrite = !(pt_flags & P_NO_SHARED_WRITE);
        if (maywrite) { lock(&file->memfd->lock); file->memfd->maywrite_maps++; unlock(&file->memfd->lock); }
        int error = file->ops->mmap(file, current->mem, destination, new_pages,
                                    offset, pt_flags & ~P_COW, MMAP_SHARED);
        if (error < 0) {
            if (maywrite) { lock(&file->memfd->lock); file->memfd->maywrite_maps--; unlock(&file->memfd->lock); }
            fd_close(file);
            result = error;
            goto out;
        }
        struct data *mapped = mem_pt(current->mem, destination)->data;
        mapped->memfd_maywrite = maywrite;
        mapped->fd = file;
        mapped->file_offset = offset;
        if (destination != start) pt_unmap_always(current->mem, start, old_pages);
        result = destination << PAGE_BITS;
        goto out;
    }
    if (!(pt_flags & P_ANONYMOUS)) {
        FIXME("mremap grow on file mappings");
        result = _EFAULT;
        goto out;
    }
    page_t extra_start = start + old_pages;
    pages_t extra_pages = new_pages - old_pages;
    bool is_prot_none = !(pt_flags & (P_READ | P_WRITE | P_EXEC));
    if (pt_is_hole(current->mem, extra_start, extra_pages)) {
        int err = pt_map_nothing(current->mem, extra_start, extra_pages, pt_flags);
        if (err < 0)
            result = err;
        goto out;
    }

    if (!(flags & MREMAP_MAYMOVE_)) {
        result = _ENOMEM;
        goto out;
    }

    page_t new_start = pt_find_hole(current->mem, new_pages);
    if (new_start == BAD_PAGE) {
        result = _ENOMEM;
        goto out;
    }
    int err = pt_map_nothing(current->mem, new_start, new_pages, pt_flags);
    if (err < 0) {
        result = err;
        goto out;
    }
    if (!is_prot_none) {
        for (pages_t i = 0; i < old_pages; i++) {
            struct pt_entry *src = mem_pt(current->mem, start + i);
            struct pt_entry *dst = mem_pt(current->mem, new_start + i);
            memcpy((char *)dst->data->data + dst->offset,
                   (char *)src->data->data + src->offset,
                   PAGE_SIZE);
        }
    }
    pt_unmap_always(current->mem, start, old_pages);
    result = new_start << PAGE_BITS;

out:
    write_wrunlock(&current->mem->lock);
    return result;
}

int_t sys_mprotect(addr_t addr, addr_t len, int_t prot) {
    STRACE("mprotect(0x%llx, 0x%llx, 0x%x)", (unsigned long long)addr, (unsigned long long)len, prot);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    const unsigned down = 0x01000000, up = 0x02000000;
    unsigned grows = prot & (down | up);
    if ((prot & ~(P_RWX | down | up)) || grows == (down | up))
        return _EINVAL;
    prot &= ~(down | up);
    if (!len) return 0;
    const addr_t limit = (USER_ADDR_MAX_PAGE + 1) << PAGE_BITS;
    if (addr >= limit || len > limit - addr ||
            len > UINT64_MAX - (PAGE_SIZE - 1)) return _ENOMEM;
    pages_t pages = PAGE_ROUND_UP(len);
    if (pages > USER_ADDR_MAX_PAGE + 1 - PAGE(addr)) return _ENOMEM;
    write_wrlock(&current->mem->lock);
    page_t start = PAGE(addr), end = start + pages;
    int err = 0;
    if (grows) {
        page_t anchor = start;
        if (grows & down) {
            // find_vma semantics allow the first grow-down VMA to start
            // after addr, but it must intersect the requested interval.
            while (anchor < end && !mem_pt(current->mem, anchor)) mem_next_page(current->mem, &anchor);
        }
        struct pt_entry *entry = anchor < end ? mem_pt(current->mem, anchor) : NULL;
        if (!entry) { err = _ENOMEM; goto out; }
        if ((grows & up) || !(entry->flags & P_GROWSDOWN)) { err = _EINVAL; goto out; }
        start = anchor;
        unsigned signature = entry->flags & (P_RWX | P_GROWSDOWN | P_SHARED | P_NO_SHARED_WRITE);
        while (start && (entry = mem_pt(current->mem, start - 1)) &&
                (entry->flags & (P_RWX | P_GROWSDOWN | P_SHARED | P_NO_SHARED_WRITE)) == signature) start--;
        pages = end - start;
    }
    err = pt_set_flags(current->mem, start, pages, prot);
out:
    write_wrunlock(&current->mem->lock);
    return err;
}

dword_t sys_madvise(addr_t addr, dword_t len, dword_t advice) {
    STRACE("madvise(0x%llx, 0x%x, %d)", (unsigned long long)addr, len, advice);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    // Linux returns ENOMEM if any page in the range is unmapped. Lazy
    // reservations count as mappings because they represent PROT_NONE VMAs.
    read_wrlock(&current->mem->lock);
    for (page_t page = PAGE(addr); page < PAGE(addr) + PAGE_ROUND_UP(len); page++) {
        if (mem_pt(current->mem, page) == NULL &&
                mem_find_reservation(current->mem, page) == NULL) {
            read_wrunlock(&current->mem->lock);
            return _ENOMEM;
        }
    }
    read_wrunlock(&current->mem->lock);

    // MADV_FREE (8) is purely advisory: the kernel MAY reclaim the pages under
    // memory pressure, but until then reads still return the old contents.
    // Eagerly zeroing them is both wrong (a subsequent read that races the
    // reclaim must see either old-or-zero, never a torn mix) and dangerous
    // under threads — another thread aliasing the page via a live TLB entry
    // would observe it turn to zero mid-computation. JSC's scavenger uses
    // MADV_FREE heavily; treat it as a no-op (safe: we just keep the memory).
    if (advice == 8 /* MADV_FREE */)
        return 0;

    if (advice == 4 /* MADV_DONTNEED */) {
        // Anonymous private pages become zero-fill. File-backed private pages
        // must retain their file contents; zeroing those corrupts mapped
        // executables and caches. Walk under a read lock and skip lazy pages so
        // exit cannot hang behind repeated per-page write-lock acquisition.
        addr_t end = addr + len;
        bool any = false;
        read_wrlock(&current->mem->lock);
        for (addr_t p = addr; p < end; p += PAGE_SIZE) {
            struct pt_entry *pt = mem_pt(current->mem, PAGE(p));
            if (pt == NULL || pt->data == NULL)
                continue;
            if (!(pt->flags & P_ANONYMOUS) && pt->data->fd != NULL)
                continue;
            void *ptr = mem_ptr(current->mem, p, MEM_WRITE);
            if (ptr != NULL) {
                memset(ptr, 0, PAGE_SIZE);
                any = true;
            }
        }
        read_wrunlock(&current->mem->lock);
        if (any)
            mem_changed_pub(current->mem);
    }
    return 0;
}

dword_t sys_mbind(addr_t UNUSED(addr), dword_t UNUSED(len), int_t UNUSED(mode),
        addr_t UNUSED(nodemask), dword_t UNUSED(maxnode), uint_t UNUSED(flags)) {
    return 0;
}

int_t sys_mlock(addr_t UNUSED(addr), dword_t UNUSED(len)) {
    return 0;
}

int_t sys_msync(addr_t UNUSED(addr), dword_t UNUSED(len), int_t UNUSED(flags)) {
    return 0;
}

addr_t sys_brk(addr_t new_brk) {
    STRACE("brk(0x%x)", new_brk);
    struct mm *mm = current->mm;
    write_wrlock(&mm->mem.lock);
    if (new_brk < mm->start_brk)
        goto out;
    addr_t old_brk = mm->brk;

    if (new_brk > old_brk) {
        // expand heap: map region from old_brk to new_brk
        // round up because of the definition of brk: "the first location after the end of the uninitialized data segment." (brk(2))
        // if the brk is 0x2000, page 0x2000 shouldn't be mapped, but it should be if the brk is 0x2001.
        page_t start = PAGE_ROUND_UP(old_brk);
        pages_t size = PAGE_ROUND_UP(new_brk) - PAGE_ROUND_UP(old_brk);
        if (!pt_is_hole(&mm->mem, start, size))
            goto out;
        int err = pt_map_nothing(&mm->mem, start, size, P_WRITE);
        if (err < 0)
            goto out;
    } else if (new_brk < old_brk) {
        // shrink heap: unmap pages that are entirely above new_brk
        // PAGE_ROUND_UP(new_brk) is the first page we can safely unmap
        // (the page containing new_brk may still have live data below new_brk)
        page_t first_unmap = PAGE_ROUND_UP(new_brk);
        page_t last_unmap = PAGE_ROUND_UP(old_brk);
        if (first_unmap < last_unmap)
            pt_unmap_always(&mm->mem, first_unmap, last_unmap - first_unmap);
    }

    mm->brk = new_brk;
out:;
    addr_t brk = mm->brk;
    write_wrunlock(&mm->mem.lock);
    return brk;
}
