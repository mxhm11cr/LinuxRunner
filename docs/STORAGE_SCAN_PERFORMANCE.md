# Pause storage accounting during interactive graphics

`EnvironmentManager.refresh()` walks every environment root and exchange to
update storage totals. Putting this work on a utility task keeps the UI thread
free, but a large rootfs still consumes CPU and filesystem bandwidth while the
Linux desktop is in use. A refresh cannot cancel an already running walk.

The manager now retains and cancels that task when its graphical cover opens,
checks cancellation between files, and resumes measurement when the cover
closes. Cached complete totals remain available. Generation checks reject a
cancelled task's late completion, and partial totals are never published. The
30-second measurement interval starts after a completed scan. Explicit Storage
settings measurements remain available.

## Physical iPad evidence

Measured on 2026-11-02 on an iPad Air 13-inch (M4), iPadOS 27.2, using the ARM64
interpreter. Both runs used the same local SteamOS/Plasma X11 integration build,
1560 × 1112 framebuffer, virgl/ANGLE GPU and Metal framebuffer presenter.
The second run adds the storage-lifecycle change isolated in this PR. The local
SteamOS support and temporary sampling instrumentation are outside this PR.

An Instruments Time Profiler recording before the change attributed **57.5% of
running CPU samples** to the native storage worker. With the matching dSYM
`7AAD93F0-A52E-3F15-A9B4-94D01271185C`, its hot app frames resolve to
`StorageUsage.bytes` and the detached closure in `EnvironmentManager.refresh`.

For the paired comparison, cumulative process user + system CPU time came from
`getrusage(RUSAGE_SELF)` and timestamps from `CLOCK_MONOTONIC`. The three retained
snapshots cover the same 30–60 second interval after session metrics began,
before scripted interaction. Both runs had nominal thermal state and no input
events during that interval.

| Storage policy | Window | CPU seconds | CPU use (100% = one core) | Display updates |
| --- | ---: | ---: | ---: | ---: |
| Background walk continues | 29.998 s | 36.685 s | 122.3% | 1 |
| Cancel/defer while GUI is visible | 29.999 s | 11.170 s | 37.2% | 0 |

This is approximately **69.6% less process CPU time** in this paired quiet
window, freeing about 0.85 CPU core while the old scan would have been active.
It is one paired run on a large imported distribution, not a sustained CPU,
desktop FPS, input-latency or battery-life benchmark. Smaller roots may finish
their scan before the viewer opens and see a smaller effect.

The before and after physical UI tests both opened the desktop, launched
Konsole, synthesized touch/text input, verified `aarch64` output and completed
without an app exit. These are integration-build results. The upstream-based
standalone patch also passes an unsigned Release iOS build and the diagnostics
regression executable.

## Recalculate and reproduce

The retained [allowlisted snapshots](../tests/linuxrunner/diagnostics/storage-scan-ipad-2026-11-02.json)
contain the measured counters and binary UUIDs, with no device IDs or raw logs.
From the repository root:

```sh
python3 - <<'PY'
import json
from pathlib import Path
path = Path('tests/linuxrunner/diagnostics/storage-scan-ipad-2026-11-02.json')
for name, run in json.loads(path.read_text())['runs'].items():
    first, last = run['samples'][0], run['samples'][-1]
    elapsed = last['monotonic_s'] - first['monotonic_s']
    cpu = last['cpu_s'] - first['cpu_s']
    print(f'{name}: {elapsed:.3f}s, {cpu:.3f} CPU s, {100 * cpu / elapsed:.1f}% of one core')
PY
swiftc app/LinuxRunner/Environment.swift app/LinuxRunner/Diagnostics.swift \
  tests/linuxrunner/diagnostics/DiagnosticsTests.swift -o /tmp/storage-diagnostics-tests
/tmp/storage-diagnostics-tests
```

For an independent device run, build the parent and candidate with the same
settings and a sufficiently large installed environment. Attach Xcode Time
Profiler, open the Linux desktop promptly after app launch, and leave it idle
through the first minute. The parent shows the storage walk continuing after
Open Desktop; the candidate cancels it. Return to the environment manager and
verify storage measurement can finish again. The local SteamOS fixture is not
a self-contained public reproduction kit; the storage policy applies to the
existing distributions too.
