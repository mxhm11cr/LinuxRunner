#ifndef ASBESTOS_H
#define ASBESTOS_H
#include "misc.h"
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"

#define FIBER_INITIAL_HASH_SIZE (1 << 10)
#define FIBER_CACHE_SIZE (1 << 12)  // 4096 entries
#define FIBER_PAGE_HASH_SIZE (1 << 10)
#define FIBER_CODE_SEEN_WORDS (1 << 11)

struct asbestos {
    // there is one asbestos per address space
    struct mmu *mmu;
    size_t mem_used;
    size_t num_blocks;
    // Translation memory policy (cpu_run_to_interrupt): monotonic ms of the
    // last block insertion; set when the cache passes the hard cap.
    uint64_t last_insert_ms;
    bool flush_requested;
    unsigned pressure_seen;   // asbestos_pressure_gen when this space last flushed for memory pressure

    struct list *hash;
    size_t hash_size;

    // list of fiber_blocks that should be freed soon (at the next RCU grace
    // period, if we had such a thing)
    struct list jetsam;
    size_t retired_bytes;
    _Atomic bool collecting;

    // A way to look up blocks in a page
    struct {
        struct list blocks[2];
        // Negative exact-page lookup, protected by asbestos->lock. Insertion
        // into this bucket invalidates it; data writes need not rescan every
        // colliding code block on every interpreted block boundary.
        page_t absent_page[2][4];
        unsigned char absent_valid[2], absent_next[2];
    } *page_hash;

    // Exact-page negative cache, protected by lock. Unlike per-bucket FIFO,
    // this stays valid when unrelated colliding code pages are inserted.
    // 9 KiB/address-space; both block endpoint indexes must be absent.
    page_t absent_exact[1024];
    unsigned char absent_exact_valid[1024];

    // Conservative membership filter for pages decoded in this address space.
    // Bits are only added, never removed: collisions/stale bits take the normal
    // locked path; a missing bit proves no decoded code needs invalidation.
    // Bounded at 16 KiB, shared by all CLONE_VM threads, no per-write allocation.
    _Atomic uint64_t code_seen[FIBER_CODE_SEEN_WORDS];

    // Incremented on every block invalidation; used to invalidate persistent
    // per-thread block caches (which may hold pointers to jetsam'd blocks).
    // Atomic because readers check it without asbestos->lock in the hot JIT path.
    _Atomic unsigned invalidate_gen;

    // Number of threads currently inside cpu_run_to_interrupt.
    // When 1, we can skip jetsam_lock (no other thread to synchronize with).
    unsigned active_threads;

    // === RCU-like Optimization ===
    // Atomic counter: number of threads currently executing JIT code.
    // Jetsam cleanup waits until this reaches 0 before freeing blocks.
    // This avoids read lock overhead on every JIT enter/exit.
    _Atomic unsigned jit_active_threads;

    // Generation number for jetsam cleanup. Incremented when jetsam list
    // becomes non-empty. Threads check this to know when cleanup is needed.
    _Atomic unsigned jetsam_gen;

    lock_t lock;
    wrlock_t jetsam_lock;
#ifdef ISH_JIT
    void *jit_ctxs;
#endif
};

// this is roughly the average number of instructions in a basic block according to anonymous sources
// times 4, roughly the average number of gadgets/parameters in an instruction, according to anonymous sources
#define FIBER_BLOCK_INITIAL_CAPACITY 16

// A guest memory fault in the gadget words code[start + 1 .. end] (the code
// stream pointer has already passed the gadget word at start) retries guest
// instruction pc. Lets faults recover the precise PC from the code-stream
// position instead of every memory instruction storing it at run time.
struct fiber_fault_pc {
    uint32_t start, end;
    uint64_t pc;
};

struct fiber_block {
    addr_t addr;
    addr_t end_addr;
    size_t used;

    // pointers to the ip values in the last gadget
    unsigned long *jump_ip[2];
    // original values of *jump_ip[]
    unsigned long old_jump_ip[2];
#ifdef GUEST_ARM64
    // True while jump_ip[i] still contains an ARM64 fake IP. Once patched to a
    // whole-block code pointer, the C dispatcher can skip lock-heavy chaining
    // attempts for that slot.
    bool jump_ip_is_fake[2];
#endif
    // blocks that jump to this block
    struct list jumps_from[2];

    // hashtable bucket links
    struct list chain;
    // list of blocks in a page
    struct list page[2];
    // links for jumps_from
    struct list jumps_from_links[2];
    // links for free list
    struct list jetsam;
    bool is_jetsam;

    // Precise fault PCs, sorted by start (owned; freed with the block).
    struct fiber_fault_pc *fault_pcs;
    uint32_t fault_pc_count;

#ifdef ISH_JIT
    uint32_t *native_link[2];
    uint32_t native_link_orig[2];
    uint32_t *native_loop;
    uintptr_t native_entry;
    void *jit_ctx;
    unsigned jit_idx;
#endif
    unsigned long code[];
};

// High-bit tracing: detect emulation bugs that leave bits 32+ dirty in guest regs
extern volatile bool g_trace_highbits;

// Optional per-guest-PC trace hooks (used for targeted replay debugging).
extern volatile bool g_trace_guest_pc;
void asbestos_set_trace_pcs(const char *spec);
void asbestos_set_trace_gate(const char *pc_spec, const char *x4_spec, const char *budget_spec);
bool asbestos_should_trace_guest_pc(addr_t pc);

// Create a new asbestos
struct asbestos *asbestos_new(struct mmu *mmu);
void asbestos_free(struct asbestos *asbestos);
// Call after releasing the address-space read lock.
void asbestos_collect(struct asbestos *asbestos);

// Invalidate all fiber blocks in pages start (inclusive) to end (exclusive).
// Locks the asbestos. Should only be called by memory.c in conjunction with
// mem_changed.
void asbestos_invalidate_range(struct asbestos *asbestos, page_t start, page_t end);
void asbestos_invalidate_page(struct asbestos *asbestos, page_t page);
bool fiber_block_fault_pc(const struct fiber_block *block, const unsigned long *code_ptr, addr_t *pc);
void asbestos_invalidate_all(struct asbestos *asbestos);

#endif
