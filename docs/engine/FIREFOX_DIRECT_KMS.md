# Firefox in a direct KMS desktop

Firefox uses native Wayland when its session socket exists. It uses X11 when no live Wayland socket exists. Both paths use software application rendering. Wayfire keeps virgl and direct KMS presentation.

## Memory corrections

- Small anonymous and copy-on-write guest pages share host page allocations. A 16 KB iPad host page holds four separate 4 KB guest pages. Each guest page keeps its own backing reference and guest permissions.
- `MADV_DONTNEED` replaces complete, unshared host pages with demand-zero backing. Partial ranges and copy-on-write pages receive separate zero backing. Shared mappings and file mappings keep their contents. The old zero-fill loop kept discarded memory committed.
- On physical iOS devices, each address space has a 32 MB target for active translation blocks. Lookup tables and fault metadata use additional memory. Idle translation retention is five seconds. Environment overrides remain available for diagnostics.
- Retired translations have a separate memory counter. A collector stops new execution slices and drains existing readers outside the address-space lock. This prevents repeated nonblocking cleanup attempts from retaining large retired lists. New slices yield while collection runs, so they cannot block a copy-on-write writer while waiting for the collector.
- Translation destruction decrements counters once. It previously decremented them before the block loop and again inside the loop.
- Physical iOS devices report half of the smaller physical or current app memory limit to guest programs. The remaining half is reserved for the interpreter, graphics and native UI. The runtime reads the current app limit from the Apple memory APIs.

`/proc/ish/memory` reports active translation bytes, retired translation bytes and, on Apple hosts, the app footprint and resident memory. Guest mapping counts do not measure physical host memory.

## Checks

The host allocation contract checks 1,024 guest pages, zero fill, independent contents, allocation failure and concurrent reuse. The actual kernel contracts check fork, copy-on-write, permissions, release and discard. The discard contract uses `mincore` to check that discarded backing is no longer resident before it reads the zero pages.

The translation contract holds a competing reader, starts collection, checks that a new execution slice yields, then releases the reader. It checks reclamation, stale cache invalidation and balanced counters.

The physical device test requires the local Firefox page heading and JavaScript result in screenshots. It types text and activates Count with the keyboard, then checks the rendered result. It then checks that the app and page remain present for three minutes.

Menu policy tests check live Wayland selection, X11 fallback, literal arguments, display authorization and managed package entries. They do not establish browser rendering.

## Device investigation

The original failure used the Wayfire app menu on an iPad 9th generation, direct KMS, virgl, ANGLE Metal and a 1080 × 808 output. The app reported memory warnings near 1.55 GB. Earlier retry logs also contained renderer errors with error 22. No current iOS crash report established the termination cause.

The first memory candidate rendered the local page and its JavaScript result through native Wayland on the iPad. It still reached memory pressure during the longer run. The final product code passed the complete physical device test at 1024 × 768. Screenshots show the local heading, JavaScript result, entered text and Count result. The result remained visible through three minutes of checks after input. The test completed in 285 seconds, with no app crash.

The five-second memory sample series contained 38 samples during that run. The maximum sampled app footprint was 1,390,532,144 bytes. The memory-pressure flush counter stayed at zero. Retired code reached 36,225,008 bytes during collection and returned to zero.

A separate check opened the Wayfire app menu, searched for Firefox and activated its normal package entry. The launched process used `MOZ_ENABLE_WAYLAND=1`, `GDK_BACKEND=wayland` and `WAYLAND_DISPLAY=wayland-1`. This check uses the existing guest account and normal Firefox profile.

The menu-launched browser rendered the same local page and JavaScript result at 1080 × 808. Its session log confirmed direct KMS. The run recorded zero native memory warnings and zero renderer errors. The 23 five-second samples reached 1,339,593,928 bytes of app footprint. The normal profile remained intact. Nine temporary test profiles were removed after checking that they were neither registered nor open.
