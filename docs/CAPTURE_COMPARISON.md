# Socket versus shared X11 image capture

For the repeated measurements with forced-exit cleanup in place, see
[capture responsiveness after cleanup](CAPTURE_POST_CLEANUP.md). This page
retains the earlier pre-cleanup runs and their original qualifications.

Shared images improved measured input/repaint delivery on the physical iPad in
these controlled workloads. These runs used an opt-in policy. A subsequent
[capture-scoped cleanup correction](CAPTURE_CLEANUP.md) addresses forced-exit
lifetime separately; these measurements used the older runtime. This changes
how x11vnc obtains pixels from X11; it does not
replace X11 requests/synchronization or the RFB connection to the native viewer.

## What was measured

Four runs on an iPad Air 13-inch (M4), iPadOS 27.2, used the same instrumented
LinuxRunner Release app binary, ARM64 interpreter and guest fixture ELF. Only
the socket/shared-image launch preference changed within each desktop pair.
Storage scans stayed paused in both modes. The full app process was terminated
after each run, then relaunched without the capture override.

The controlled fullscreen Xlib fixture returned 30 click and 30 keyboard
responses, alternating, with one native RFB input outstanding at a time. Each
repaint carried its own sequence ID. The observer accepted matching pixels,
rather than attributing the next unrelated frame to the input. Latency ends
when that matching frame is submitted to the existing native view. It excludes
UIKit input, actual Magic Keyboard/trackpad delivery, GPU completion and physical
panel response. These are not hardware input-to-screen measurements.

| Desktop / mode | Click median / p95 | Key median / p95 | Distinct repaints submitted, of 120 | Median / p95 submission gap |
| --- | ---: | ---: | ---: | ---: |
| Arch — socket | 35.5 / 60.9 ms | 64.2 / 93.3 ms | 30 | 68.5 / 86.9 ms |
| Arch — shared | 25.0 / 57.4 ms | 55.4 / 66.3 ms | 55 | 38.8 / 44.3 ms |
| SteamOS — socket | 108.4 / 132.4 ms | 88.7 / 131.4 ms | 9 | 251.6 / 273.2 ms |
| SteamOS — shared | 63.8 / 107.4 ms | 69.6 / 104.4 ms | 30 | 66.7 / 77.6 ms |

The active phase generated 120 large striped repaints toward 60 Hz, waiting
for X11 draw completion after each. Actual generation durations were 1.987,
1.987, 1.988 and 1.985 seconds respectively. The counts above measure distinct
numbered images reaching native submission; they are **not desktop/game FPS**.
Arch/shared decoded 56 distinct burst IDs and submitted 55, showing one
coalesced at native submission. The other three runs submitted every decoded
ID. The probe does not isolate which earlier X11/capture/RFB stage coalesced
other generated IDs.

Within these pairs, shared capture delivered more distinct repaints, shorter
submission gaps, and lower median and p95 input response times. The maximum
Arch click time increased slightly, from 63.8 to 64.5 ms; individual events
still vary. No CPU reduction is required for these capture-path improvements.
The earlier quiet SteamOS pair actually used slightly more idle CPU with shared
capture (40.8% versus 38.1% of one core).

The startup framebuffer-read calibrations were 53 → 794 MB/s on Arch and
40 → 763 MB/s on SteamOS. These are x11vnc component calibrations, not sustained
bandwidth or an end-to-end speed multiplier.

## Limits and decision

This is one sequential socket/shared pair per desktop, without randomized
order, independent repeat trials or a sustained real-application workload.
Arch uses Xvfb/Core Graphics at 960 × 540 with default VNC timing; SteamOS uses
accelerated Xorg/Metal presentation at 1560 × 1112 with adaptive timing. Compare
capture modes within a desktop, not Arch speed against SteamOS speed.

The first three runs began and ended at nominal thermal state. SteamOS's
shared run began nominal and ended at **fair** thermal state. Matched-thermal
repeats remain unqualified; this observation does not establish that shared
capture caused heating. The observer itself adds instrumentation overhead.
Earlier instrumentation pilots used smaller markers and/or queued timestamp
association and are excluded. All final runs used larger encoded markers and
receiver timestamps retained under the frame lock, synchronously associated
with the submitted snapshot. No queued observer can arrive too late for that
latency association.

Shared images are promising for capture responsiveness, particularly larger
repaints. These historical runs used an opt-in policy. The measured build could leave
unmarked SysV segments allocated after forced termination until the LinuxRunner
app process exited. These measurements do not qualify the later cleanup fix,
and do not establish battery life, panel latency or universal desktop gains.

[Allowlisted samples and provenance](../tests/linuxrunner/steamos/capture-active-2026-11-02.json)
include each input's timestamps, frame IDs, quantile method, source hashes and
binary/fixture identities. Raw device/account logs and screenshots stay private.
The [measurement harness](../tests/linuxrunner/gui/capture-probe/README.md)
retains the exact temporary viewer patch, fixture and host checks. The shipping
viewer source was restored after measurement; no default was promoted. This
capture experiment is separate from the storage-scan PRs.

The explicit `testReferenceCapturePerformance` XCTest entry is retained in
`app/UITests/CaptureCleanupUITests.m`. Ordinary runs skip it. It requires the
separate measurement-only viewer instrumentation from `capture-probe/` and an
explicit reference bundle/root plus `LR_REFERENCE_CAPTURE_PROBE=YES`; setting
the flag in an ordinary build does not install the probe or qualify a result.
