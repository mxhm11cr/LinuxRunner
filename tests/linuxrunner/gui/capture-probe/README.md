# Physical-device capture probe

This is measurement tooling, not part of the shipping app. Apply
`instrumentation.patch` to the matching measured integration revision of `GUIViewController.m` and compile the
app normally. The patch imports `DeviceCaptureProbe.h` only in this temporary
instrumented build. Save and restore the existing viewer source; do not replace
unrelated local integration changes. Record the exact source/patch hashes and
binary UUID, and keep all build, graphics, geometry and capture-timing settings
fixed between socket and SysV shared-image runs.

Run `testReferenceCapturePerformance` with the explicitly installed bundle/root,
`LR_REFERENCE_CAPTURE_PROBE=YES`, and `LR_REFERENCE_SHARED_IMAGES=NO` or `YES`.
Start `reference_desktop_broker.py` before the test. Use `--probe-source` with
`fixture.c` to compile it with the guest's GCC/libX11, or `--probe-binary` with
that retained, identified ARM64 ELF for a second environment. The fixture runs
through the existing desktop request queue, with the selected user's display
authorization. Ordinary test runs skip the probe. The test terminates the entire
app after every run and relaunches without the temporary capture override, also
on assertion failure. This establishes paired-run starting conditions;
session crash recovery has its own cleanup qualification.
The original measurements predated that correction. The
post-cleanup repeat uses the same
measurement fixture and observer with the correction present.

The fullscreen override-redirect X11 fixture encodes an exact sequence ID in
three fixed pixel markers. An invertible 24-bit color code makes consecutive
IDs distinct. It answers 30 native RFB click events and 30 keyboard events,
alternating, with one outstanding input. The observer accepts only matching
IDs and timestamps, then waits 120 ms before the next input. Receiver timestamps
are retained under the frame lock and read alongside the submitted snapshot;
a queued observer cannot arrive too late to identify that repaint. It records:

- Native RFB enqueue to the matching pixels decoded by the existing app client.
- The same enqueue to the matching frame submitted to the existing native view.
- Distinct generated IDs decoded/submitted during 120 large striped repaints,
  paced toward 60 Hz and synchronized with the X server after every draw.

The guest logs its actual generation duration. Record both delivered IDs and
frame gaps: requested generation rate alone is not an achieved frame rate.
The fixture then publishes an end marker. Reports are private session-local
`capture-probe.json` files; no account or accessory identifiers are needed.

These timings exclude UIKit input delivery, actual Magic Keyboard input, GPU
completion and physical panel response. A native submission is not proof of
when that image reached the panel. The synthetic solid-strip workload is not
desktop/game FPS, and a single sequential mode pair is not repeated-trial or
sustained-performance evidence. Do not substitute an unrelated next frame for
a matching repaint, or treat duplicate/coalesced IDs as additional repaints.

A bounded Foundation check exercises marker bounds, ID decoding, causal input
matching, timestamp arithmetic, duplicate rejection, JSON serialization and
invalidation without a device:

```sh
xcrun clang -fobjc-arc -Wall -Wextra -Werror -framework Foundation \
  tests/linuxrunner/gui/capture-probe/DeviceCaptureProbeTests.m \
  -o /tmp/linuxrunner-capture-probe-tests
/tmp/linuxrunner-capture-probe-tests
```

The retained instrumentation patch targets the measured SteamOS integration,
which includes presentation/storage hooks absent from the focused capture PR.
Its source hashes and exact fixture are retained for provenance; do not claim
that applying the patch to an arbitrary upstream revision reproduces the
integration app. The PR adds no measurement observer to the ordinary app.
