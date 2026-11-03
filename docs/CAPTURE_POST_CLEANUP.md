# Capture responsiveness after the cleanup correction

Shared image capture was remeasured with the
[forced-exit cleanup correction](CAPTURE_CLEANUP.md) in place. In these bounded
physical iPad runs it retained lower median input-response times, more distinct
numbered repaints reaching the native view, and shorter repaint handoff gaps.
The measured runs explicitly selected each mode. This PR now enables guarded
shared capture by default for Arch and SteamOS desktops, preserving explicit
choices; the measurements themselves predate that policy change.

Two mode pairs per desktop used one instrumented Release app binary and the
same retained ARM64 fixture. Arch ran socket/shared then shared/socket;
SteamOS ran shared/socket then socket/shared. Each run completed 30 synthetic
clicks, 30 keyboard inputs and 120 numbered repaints. A full app restart
separated runs, with a 30-second gap between completed test runs. Geometry,
renderer and VNC timing matched within each desktop. Storage scans stayed
paused during graphical presentation.

Latency ends after the matching frame's `presentPatch` call returns: **handoff
to the native view**. It excludes UIKit input delivery, actual Magic Keyboard
input, GPU command submission/completion and physical panel visibility.

| Desktop / capture | Click median / p95 | Keyboard median / p95 | Numbered repaints handed off, each run / 120 | Repaint gap p95, each run |
| --- | ---: | ---: | ---: | ---: |
| Arch socket | 38.1 / 55.3 ms | 64.8 / 75.6 ms | 30, 30 | 87.4, 87.0 ms |
| Arch shared | 26.1 / 46.0 ms | 55.0 / 76.3 ms | 57, 56 | 58.5, 50.8 ms |
| SteamOS socket | 107.1 / 134.5 ms | 92.5 / 132.2 ms | 9, 9 | 275.7, 275.3 ms |
| SteamOS shared | 63.5 / 109.7 ms | 71.6 / 114.6 ms | 30, 31 | 83.4, 82.4 ms |

Input quantiles pool 60 events of each kind per mode; p95 is nearest rank.
Pooling describes these samples, not 60 independent experimental trials.
SteamOS median click response was 40.7% lower with shared capture and median
keyboard response 22.5% lower. Arch's reductions were 31.7% and 15.1%.
Arch keyboard p95 increased slightly; the result does not show that every
latency percentile improves. The fixture's distinct images are not desktop or
game FPS. Actual generation times and decoded/handoff IDs remain in the evidence.

All Arch runs began and ended at nominal thermal state. All SteamOS runs began
nominal; both socket runs and the second shared run ended fair, while the first
shared run ended nominal. Temperature was observed rather than controlled
throughout. No heat, battery or CPU benefit is inferred. Two reversed-order
pairs are still a small, synthetic workload, not a sustained application test
or a significance result.

Arch used Xvfb/Core Graphics at 960 × 540 with default VNC timing. SteamOS used
Xorg virgl/Metal at 1560 × 1112 with adaptive timing. Compare capture modes
within a desktop, not Arch against SteamOS. This measures the fixed build in
both modes; the older measurements are not a matched before/after experiment
for the cleanup patch itself.

[Allowlisted samples and provenance](../tests/linuxrunner/steamos/capture-post-cleanup-2026-11-02.json)
retain every event, repaint ID, per-run quantile, thermal endpoint, source hash,
fixture hash and app UUID. Full device/account logs stay private. The retained
[measurement harness](../tests/linuxrunner/gui/capture-probe/README.md) was
unchanged. Temporary instrumentation was removed after collection.
