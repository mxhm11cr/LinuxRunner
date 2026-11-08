# Engine benchmarks

## Environment

| | |
|---|---|
| Machine | MacBook Air, Apple M1 (4 performance + 4 efficiency cores), 8 GB RAM, macOS 27.0.1 |
| Runtime | LinuxRunner CLI runtime (`ish`, meson `debugoptimized`, `-O2`, assertions on), same engine sources as the iOS app |
| Guest | Debian 13 ARM64 fakefs (glibc 2.41, gcc 14.2) |
| Native reference | the same C source built with Apple clang `-O2` and run on macOS |

**Not measured on an iPhone or iPad.** The engine code is identical on iOS, but absolute numbers will differ
(device CPU, thermal limits, memory). Relative changes are expected to carry over; this still needs
device confirmation.

## Suites

- **Micro** — `tests/linuxrunner/bench/interp_bench.c`: 13 workloads, each isolating one engine cost
  (ALU, branches, sequential loads/stores, software-TLB misses, calls, indirect calls, FP, NEON,
  libc string routines, atomics, uncontended mutex, syscalls, futex ping-pong). Prints ns per
  iteration and a checksum; checksums must be identical between runtimes (correctness guard).
- **Macro** — `tests/linuxrunner/bench/macro.sh`: real Debian programs (Python startup/compute/JSON,
  gcc compiling a 300-line file, perl regex, gzip/xz of 4 MB, sort of 200k lines, sha256sum,
  `git hash-object`, Node.js startup). Prints wall-clock ms and a hash of the program output.

## Method

1. Build the guest benchmark inside the Debian fixture: `gcc -O2 -static interp_bench.c -lpthread`.
2. `tests/linuxrunner/bench/tools/interleave.py 5 base=OLD_RUNTIME new=NEW_RUNTIME`
   (`LR_BENCH_ROOTFS`, `LR_BENCH_BIN`, `LR_BENCH_DIR` set): alternates the runtimes every round so
   thermal or background drift affects both equally; reports medians of 5 rounds.
3. `compare.py base new native` prints speedups and flags any checksum mismatch.
4. The machine must be otherwise idle. Runs taken while other heavy jobs ran are discarded
   (one such run is recorded in the session log as invalid: every workload, including unchanged ones,
   appeared ~8 % slower).
5. Bottleneck analysis: Instruments "CPU Counters" (CPU Bottlenecks mode) recorded with
   `xctrace`, summarized by `tools/bottleneck.py` (cycle-weighted useful / processing / delivery /
   discarded shares).

## Results — engine changes 1 and 2 (build 895 runtime → the engine-performance branch)

Medians of 5 interleaved rounds. "vs native" is the remaining slowdown factor.

### Micro (ns per iteration, lower is better)

| Workload | 895 | New | Speedup | vs native |
|---|---|---|---|---|
| alu | 11.30 | 11.39 | 0.99 | 4.5× |
| branch | 20.62 | 14.52 | **1.42** | 7.5× |
| mem_seq | 229 000 | 186 750 | **1.23** | 15.1× |
| mem_rand | 77.1 | 77.6 | 0.99 | 1.1× |
| call | 40.8 | 39.2 | 1.04 | 17.3× |
| indirect | 27.4 | 27.4 | 1.00 | 28.6× |
| float | 12.16 | 12.12 | 1.00 | 2.1× |
| neon | 16 725 | 16 820 | 0.99 | 7.3× |
| string | 40 138 | 39 898 | 1.01 | 24.7× |
| atomic | 52.8 | 52.5 | 1.01 | 3.8× |
| mutex | 113.2 | 105.1 | 1.08 | 12.2× |
| syscall | 153.4 | 153.3 | 1.00 | 1.3× |
| pingpong | 7 374 | 7 330 | 1.01 | 2.6× |

### Macro (ms, lower is better)

| Program | 895 | New | Speedup |
|---|---|---|---|
| python3 -c pass | 144 | 142 | 1.01 |
| Python compute | 694 | 646 | 1.07 |
| Python JSON | 883 | 840 | 1.05 |
| gcc -O2 compile | 4 756 | 4 411 | **1.08** |
| perl regex | 178 | 165 | 1.08 |
| gzip -6 (4 MB) | 365 | 326 | **1.12** |
| xz -1 (4 MB) | 714 | 638 | **1.12** |
| sort 200k lines | 449 | 422 | 1.06 |
| sha256sum (4 MB) | 88 | 61 | **1.44** |
| git hash-object | 88 | 57 | **1.54** |
| node -e (startup) | 1 785 | 1 712 | 1.04 |

All output checksums identical. Change 1 alone (loads/stores) gave +20 % on `mem_seq` and 0–4 % on the
macro set; the rest comes from change 2 (specialized ALU / immediate gadgets).

### Measured and rejected

| Experiment | Result | Decision |
|---|---|---|
| Remove the per-access TLB coherence check (3 loads) | ±3 %, no gain | kept (guards a real race) |
| Cycle counter in a host register | ±2 % | kept (neutral, removes a memory RMW) |
| Fused LDP pre/post-index gadget | call-heavy code ~8 % *slower* | not used |

### Bottleneck shares (M1, CPU Counters)

| Workload | Useful | Processing | Delivery | Discarded |
|---|---|---|---|---|
| mem_seq (895) | 81 % | 17 % | 1 % | <1 % |
| call (895) | 49 % | 45 % | 6 % | <1 % |

## Results — 2026-10-31: host-NEON compare/pairwise, STXR fast path, indirect dirty-page filter

Same M1 machine and method, but guest = Arch Linux ARM fixture (glibc, gcc), benchmark built inside the
guest (`gcc -O2`, dynamic). Medians of 5 interleaved rounds, baseline `ish` from `615714f2` vs the change.
All checksums identical between runtimes.

| Workload | Before (ns) | After (ns) | Speedup |
|---|---|---|---|
| strsearch (new: strlen+strchr+memchr+memcmp, 16..256 B) | 547.9 | 467.4 | **1.17** |
| atomic | 53.1 | 49.3 | **1.08** |
| pingpong | 7 801 | 7 404 | 1.05 |
| mutex | 127.8 | 126.3 | 1.01 |
| indirect | 27.7 | 27.6 | 1.00 |
| string (memcpy-dominated) | 30 218 | 29 938 | 1.01 |
| all others (alu, branch, mem_*, call, float, neon, syscall) | | | 0.98–1.02 |

- CMEQ/CMHI/CMHS/ADDP/UMAXP/UMINP were per-lane scalar code; they now use the existing
  `make_threesame_native` host-NEON template. `neon_compare_pairwise_oracle.c` (59 instruction ×
  arrangement hashes, aliasing, Q=0 high-half clearing, reserved encodings) is bit-identical to native
  ARM64; reserved 1D/2D forms now raise SIGILL as on hardware (the old gadgets executed them).
- STXR translates through the software TLB and compare-and-swaps with a host LDAXR/STLXR loop
  (no host LSE: ARMv8.0 devices stay supported); page-crossing accesses keep the C helper.
  `exclusive_atomics_stress.c` passes with 2/4/8 threads.
- The indirect-branch dirty-page check now evaluates `code_seen_maybe` inline and only calls C when
  the page may hold translated code. No measurable change on `indirect`; kept because it removes a
  C call per indirect branch after a store and is exact.
- Mac fixture measurements only; not an iPhone. No real-application timing yet.
