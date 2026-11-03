# Virtual DRM display clock

The virtual 60 Hz CRTC clock advances independently of page flips. Previously,
GET_SEQUENCE counted flips, leaving Xorg/Present's media-stream counter frozen
when idle. Page-flip timestamps now use the same monotonic clock.

DRM_WAIT_VBLANK supports synchronous waits and queued legacy vblank events.
CRTC_QUEUE_SEQUENCE supports absolute/relative requests and next-on-miss.
Each requesting card file owns a sleeping event worker. Pending and ready
events share the existing bounded queue; close wakes and joins the worker
before releasing its fd. Event-requesting page flips reserve capacity before
changing scanout and return EBUSY if full. Linux condition variables and absolute
deadlines use CLOCK_MONOTONIC, avoiding wall-clock adjustments. Unsupported flags and overflowing deadlines fail.
This is a synthetic virtual display clock, not physical-panel synchronization.

Validation on Apple Silicon macOS: a freshly built ARM64 interpreter (JIT off)
with its real ARM64 vDSO ran the current `software_drm_contract.c` as a static musl executable in a
minimal disposable root. The contract checks advancing idle sequence, async
poll/read payloads, synchronous pacing, invalid flags/overflow, queue capacity,
16 close-before-deadline cycles, and existing dumb-buffer/PRIME/KMS behavior.
No physical-device or compositor performance claim is made for this PR.

Repeat after building a host interpreter:

```sh
python3 tests/linuxrunner/run-software-drm.py \
  --runtime build/host/ish --fakefsify build/host/tools/fakefsify
```

The runner requires Zig for static ARM64 Linux fixtures. This qualification
used a retained fakefsify fixture-import tool and a freshly built runtime;
archive importing itself is not the subject of this test.

The Linux monotonic-condition branch was source-reviewed on this macOS host;
a live Linux wall-clock adjustment was not performed. The contract additionally
fills the pending queue, rejects a flip with EBUSY and verifies unchanged scanout.

Synchronous waits now use the kernel guest-signal-aware condition protocol,
recomputing the remaining monotonic deadline after wakeups. A pending guest
signal returns EINTR even if its host notification arrived before sleeping.
Eight guest fork/signal/wait cycles verify interruption of a five-second vblank
deadline and prompt child reaping in the host interpreter.
