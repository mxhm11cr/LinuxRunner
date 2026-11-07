# Firefox in a direct KMS desktop

Firefox uses native Wayland when its session socket exists. It uses X11 when no live Wayland socket exists. Both paths use software application rendering. Wayfire keeps virgl and direct KMS presentation.

## Memory corrections

- Small anonymous and copy-on-write guest pages share host page allocations. A 16 KB iPad host page holds four separate 4 KB guest pages. Each guest page keeps its own backing reference and guest permissions.
- `MADV_DONTNEED` replaces complete, unshared host pages with demand-zero backing. Partial ranges and copy-on-write pages receive separate zero backing. Shared mappings and file mappings keep their contents. The old zero-fill loop kept discarded memory committed.
- On physical iOS devices, each address space keeps at most 32 MB of active translations. Idle translation retention is five seconds. Environment overrides remain available for diagnostics.
- Retired translations have a separate memory counter. A collector stops new execution slices and drains existing readers outside the address-space lock. This prevents repeated nonblocking cleanup attempts from retaining large retired lists. New slices yield while collection runs, so they cannot block a copy-on-write writer while waiting for the collector.
- Translation destruction decrements counters once. It previously decremented them before the block loop and again inside the loop.
- Physical iOS devices report half of the smaller physical or current app memory limit to guest programs. The remaining half is reserved for the interpreter, graphics and native UI. The runtime reads the current app limit from the Apple memory APIs.

`/proc/ish/memory` reports active translation bytes, retired translation bytes and, on Apple hosts, the app footprint and resident memory. Guest mapping counts do not measure physical host memory.

## Checks

The host allocation contract checks 1,024 guest pages, zero fill, independent contents, allocation failure and concurrent reuse. The actual kernel contracts check fork, copy-on-write, permissions, release and discard. The discard contract uses `mincore` to check that discarded backing is no longer resident before it reads the zero pages.

The translation contract holds a competing reader, starts collection, checks that a new execution slice yields, then releases the reader. It checks reclamation, stale cache invalidation and balanced counters.

The physical device test requires the local Firefox page heading and JavaScript result in screenshots. It types text, presses Count and checks the rendered result. It then checks that the app and page remain present for three minutes.

Menu policy tests check live Wayland selection, X11 fallback, literal arguments, display authorization and managed package entries. They do not establish browser rendering.

## Device investigation

The original failure used the Wayfire app menu on an iPad 9th generation, direct KMS, virgl, ANGLE Metal and a 1080 × 808 output. The app reported memory warnings near 1.55 GB. Earlier retry logs also contained renderer errors with error 22. No current iOS crash report established the termination cause.

The first memory candidate rendered the local page and its JavaScript result through native Wayland on the iPad. It still reached memory pressure during the longer run. The discard and translation cleanup corrections require the complete device test before this issue is closed.
