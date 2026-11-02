# Pause storage accounting during interactive graphics

`EnvironmentManager.refresh()` walks every environment root and exchange to
update storage totals. Putting this work on a utility task keeps the UI thread
free, but a large rootfs still consumes CPU and filesystem bandwidth while the
Linux desktop is in use. Originally, a refresh could not cancel an already
running walk.

Each manager retains and cancels that task while any native graphical viewer
is presented or a retained desktop transport is serving external output.
Native viewers publish process-wide activity using independent owner tokens,
so File Sharing and other application scenes observe the same pause. Dismissing
one viewer does not resume scans while another viewer or external output is
active. Detaching the last external transport, disabling/disconnecting its
output, or dismissing the last local viewer permits measurement again.

Scans check cancellation between files. Cached complete totals remain available.
Generation checks reject a cancelled task's late completion, and partial totals
are never published. The 30-second measurement interval starts after a completed
scan. Explicit Storage settings measurements remain available.

## Physical iPad evidence

Measured on 2026-11-02 on an iPad Air 13-inch (M4), iPadOS 27.2, using the ARM64
interpreter. The fresh before/after pair used preserved local SteamOS/Plasma X11
integration-source snapshots, a 1560 × 1112 framebuffer, virgl/ANGLE GPU and Metal
framebuffer presenter. The before variant uses the original automatic walk;
the after variant applies the current shared-activity storage runtime patch to
the same integration sources. Graphics, runtime bridge and UI-test sources were
held fixed. The local SteamOS support and temporary sampling instrumentation
are outside this PR.

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
| Background walk continues | 29.999 s | 37.630 s | 125.4% | 0 |
| Shared activity pauses the walk | 30.000 s | 12.220 s | 40.7% | 1 |

CPU use sums all process threads: 100% represents one CPU core's worth of time,
and 200% represents two. LinuxRunner can execute guest tasks on multiple host
threads; these percentages do not imply a one-core execution limit.

This is approximately **67.5% less process CPU time** in this paired quiet
window, freeing about 0.85 CPU core while the old scan would have been active.
It is one paired run on a large imported distribution, not a sustained CPU,
desktop FPS, input-latency or battery-life benchmark. Smaller roots may finish
their scan before the viewer opens and see a smaller effect.

The before and after physical UI tests both opened the desktop, launched
Konsole, synthesized touch/text input, verified `aarch64` output and completed
without an app exit. These are integration-build results. The upstream-based
standalone patch also passes an unsigned Release iOS build and the diagnostics
and lifecycle regression executables. External-monitor and multi-scene behavior
has host policy coverage; it has not been qualified on connected monitor
hardware or simultaneous physical iPad scenes.

The [initial comparison](../tests/linuxrunner/diagnostics/storage-scan-ipad-2026-11-02.json)
is retained as historical evidence for the original cover-based change at
`554949d1a001699d4af8237ff8a03a8c75dcdf15` (122.3% to 37.2%, 69.6% less CPU time).
It does not measure the subsequent shared-activity implementation. The fresh
pair above measures that implementation and records the changed integration
source hashes, unchanged control hashes, standalone runtime source hashes and
applied runtime patch hash, together with both binary UUIDs.

## Supported Arch reference

A second paired physical-device run used the installed **Arch Linux ARM**
reference on the same iPad. Its supported default desktop is Openbox, LXQt
panel and PCManFM-Qt, using Xvfb/RFB at **960 × 540**, the Core Graphics
presenter, socket image capture and default VNC timing. Both runs passed
desktop readiness, a managed xterm launch, synthesized touch/text input and
visible `aarch64` output. The Arch terminal separately reported `ID=archarm`,
glibc `2.43+r22+g8362e8ce10b2-2`, Bash `5.3.20-1` and Pacman
`7.1.0.r9.g54d9411-3`. No test password or regular account was created.

The before build reverses the three-file storage runtime diff from `d141978`
to `0d015d3`; the after build restores it. The same integration sources and
build settings hold the native Unix-socket path fix, graphics code and test
instrumentation fixed. That socket fix was required for Arch's D-Bus desktop
startup and is outside this storage PR. Comparing 676 source/control file
hashes found differences only in the three storage runtime files. This
checks source identity; it is not a binary-equivalence proof.

The second through fourth fifteen-second native samples cover approximately
30–60 seconds after sampling starts. The test holds interaction for 70 seconds
after readiness, and its xterm broker waits 75 seconds. The selected windows
contain no pointer/key events, no display updates and nominal thermal state.

| Storage policy | Window | CPU seconds | CPU use (100% = one core) |
| --- | ---: | ---: | ---: |
| Background walk continues | 30.000 s | 34.377 s | 114.6% |
| Shared activity pauses the walk | 30.004 s | 9.893 s | 33.0% |

This is **71.2% less process CPU time** in one sequential paired quiet window.
The same Arch, SteamOS and Alpine installations remained present in both runs.
Since storage accounting walks every environment, this measures Arch desktop
use with those coinstalled roots, **not an Arch-only filesystem**. It does not
establish an FPS, interaction-latency, sustained-performance, battery-life or
hardware-keyboard improvement. The different desktops and graphics settings
also prevent treating the Arch and SteamOS numbers as an apples-to-apples
graphics comparison. There were no randomized or repeated trials.

[Allowlisted Arch counters and provenance](../tests/linuxrunner/diagnostics/storage-scan-arch-ipad-2026-11-02.json)
record the binary UUIDs, selected raw counters, changed/runtime control hashes,
source-manifest fingerprints and applied runtime patch hash. Raw device logs
and integration source snapshots remain private. This PR is not a standalone
public integration reproduction kit. Runtime code remains unchanged from
`0d015d3`; this addition records supported-distribution evidence.

## Recalculate and reproduce

The fresh [allowlisted snapshots](../tests/linuxrunner/diagnostics/storage-scan-ipad-shared-activity-2026-11-02.json)
contain the measured counters and binary UUIDs, with no device IDs or raw logs.
From the repository root:

```sh
python3 - <<'PY'
import json
from pathlib import Path
# Use storage-scan-arch-ipad-2026-11-02.json for the supported Arch pair.
path = Path('tests/linuxrunner/diagnostics/storage-scan-ipad-shared-activity-2026-11-02.json')
for name, run in json.loads(path.read_text())['runs'].items():
    first, last = run['samples'][0], run['samples'][-1]
    elapsed = last['monotonic_s'] - first['monotonic_s']
    cpu = last['cpu_s'] - first['cpu_s']
    print(f'{name}: {elapsed:.3f}s, {cpu:.3f} CPU s, {100 * cpu / elapsed:.1f}% of one core')
PY
swiftc app/LinuxRunner/Environment.swift app/LinuxRunner/Diagnostics.swift \
  tests/linuxrunner/diagnostics/DiagnosticsTests.swift -o /tmp/storage-diagnostics-tests
/tmp/storage-diagnostics-tests
scripts/test-storage-lifecycle.sh
```

The lifecycle runner compiles the production manager, scanner and activity gate
against a disposable host runtime fixture. It verifies cancellation of existing
managers, creation/refresh of a manager while paused, overlapping viewer owners,
resumption after the last owner releases, generation races, persisted complete
totals, throttling and manager release. It does not exercise UIKit or connected
monitor hardware. A native host fixture additionally compiles the production
appearance/disappearance hooks, storage-activity predicate and deallocation
cleanup against the real Swift gate, covering the settings-cover early return,
external-only transport, disconnect/disable and detach. UIKit superclass and
presentation/client behavior are replaced by minimal Foundation host stubs;
this verifies policy and ownership, not UIKit lifecycle delivery.

For an independent device run, build the parent and candidate with the same
settings and a sufficiently large installed environment. Attach Xcode Time
Profiler, open the Linux desktop promptly after app launch, and leave it idle
through the first minute. The parent shows the storage walk continuing after
Open Desktop; the candidate cancels it. Return to the environment manager and
verify storage measurement can finish again. With external output connected,
close the local viewer and confirm scans remain paused until the external
transport detaches or output is disabled/disconnected. Also open desktop File
Sharing and a second application scene while graphics are active; neither
manager should start an automatic scan. The local SteamOS fixture is not
a self-contained public reproduction kit; the storage policy applies to the
existing distributions too.
