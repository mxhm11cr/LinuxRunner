# Shared-capture lifetime

Shared image capture defaults on for desktop sessions identified as Arch or
SteamOS in LinuxRunner's catalog (or installation metadata when uncatalogued),
unless an explicit capture preference exists. Other distributions and
standalone graphical applications retain their existing defaults. The capture launcher now requests a
runtime lease so x11vnc's future private SysV buffers are marked for deletion
when its last owning thread exits, including after `SIGKILL`. This is a
LinuxRunner runtime correction, used by SteamOS and the supported distributions;
it does not patch x11vnc or change general Linux SysV persistence.

## Ownership and fallback

`capture-shm-guard.arm64` requests LinuxRunner's versioned `LR_PR_CAPTURE_SHM`
prctl extension before executing the resolved x11vnc executable. The request is
allowed while the calling group leader is its only thread. Only subsequent
`IPC_PRIVATE` allocations belong to this opaque scope. Existing objects and
keyed allocations remain ordinary SysV objects. The lease survives exec and
is retained by `CLONE_THREAD`; forked processes do not inherit it.

The last owning thread marks its segments removed. Attached segments keep their
backing descriptor and valid mappings until the last detach, including an X
server's attachment. Cleanup never frees a mapping another process still uses.
Running lease ownership lives in host thread-local storage, so pthread cleanup
does not need a surviving guest task struct. A host thread that genuinely never
returns retains its lease; freeing memory it may still access would be unsafe.
This does not repair the runtime's broader stuck-host-thread/task lifetime policy.

If the helper is missing or the runtime does not support the capability,
`session.sh` selects the established socket image path. The launcher itself also
prepends `-noshm` on capability failure, covering rejection after a successful
probe. This is a capability fallback; automatic recovery from every later
x11vnc/X11 allocation or attach failure remains unqualified.

`/proc/ish/shm` exposes read-only aggregate segment, descriptor, requested-byte, attachment,
scope and failed-close counters. It exposes no segment IDs, keys, account names
or contents. `capture-process` in the private desktop session directory records
the owned capture PID, parent PID and start time; a consumer must recheck that
identity before signalling it.

## Reproduce the host contract

Build the ARM64 host interpreter and `tools/fakefsify` using the repository's
[build instructions](BUILDING.md). Then run:

```sh
CAPTURE_LD=/path/to/ld.lld python3 tests/linuxrunner/run-capture-shm.py \
  --runtime build/host/ish --fakefsify build/host/tools/fakefsify
```

The runner cross-builds a freestanding ARM64 contract and both supported and
deliberately rejected launchers, checks that the bundled launcher reproduces
byte-for-byte, and imports a disposable minimal root. It needs no downloaded
Linux image, guest compiler or libc. Logs remain under `build/capture-shm-test`.

The contract checks 64 scoped kill cycles and 16 multithreaded group kills,
exec retention and failed exec, last-thread ownership, cross-process attachment
retention, complete resource baselines, ordinary private persistence, keyed
objects, preexisting objects and fork isolation. Capability rejection must
preserve the target arguments while selecting `-noshm`.

## Physical qualification

`testReferenceCaptureForcedTermination` is explicitly gated by
`LR_REFERENCE_CAPTURE_CLEANUP=YES`, an installed root UUID and the test bundle.
Transfer `tests/linuxrunner/ios/capture_cleanup_device.sh` into that root's
Downloads exchange first. The native terminal launches its independent monitor;
the managed desktop request queue is unsuitable because teardown kills that
queue's process group.

The test launches LinuxRunner once, then opens five desktops: four ready-session
capture kills and one normal stop. It
requires real scoped buffers and a launcher capability receipt, checks the
capture PID's parent and start time, and requires every resource counter to
match baseline for five consecutive samples. A single consumed-session registry prevents a new
session from being missed between cycles. Guest init's start time must stay
unchanged across the complete monitor run. The test requires the native
viewer's readiness caption, leaves a bounded settle for termination, then
returns to Home. It tests actual capture
and session recovery rather than uninterrupted on-screen interaction. A separate
startup-kill monitor mode exists but is not part of this five-cycle qualification. Teardown alone restores ordinary launch preferences by
restarting the app, after all measured cycles have finished.

The earlier [responsiveness measurements](CAPTURE_COMPARISON.md) used the older
runtime and full app restarts between modes. They remain evidence for capture
delivery in those runs, not evidence for this cleanup correction or a new
performance claim.

The UI result must be paired with the independent monitor log. Verify both and
emit numeric-only evidence with:

```sh
python3 tests/linuxrunner/ios/verify_capture_cleanup.py \
  --log /path/to/capture-cleanup-all.log \
  --ui-result /path/to/mobilebuildmcp-result.json --output /path/to/verified.json
```

The verifier requires five correctly ordered cycles, real scoped buffers, zero
failed closes, matching resource baselines, runtime continuity, the monitor's
final `ALL_PASS`, and the successful UI readiness run. It rejects UI-only
success, missing/repeated cycles, nonzero residual resources and incomplete logs.
Early device pilots are excluded: an exit notice covered the terminal, a stale
Home state could appear before startup, and a per-cycle directory snapshot could
miss a newly created session. Those pilots cannot qualify five-cycle recovery.

## Qualified result, 2026-11-02

On the physical iPad Air 13-inch (M4), iPadOS 27.2, both SteamOS Plasma/virgl and
the installed Arch reference desktop passed four forced x11vnc kills followed
by a normal stop. Each distribution's five cycles used one uninterrupted app
and guest runtime. Each newly selected capture process had real scoped buffers;
every segment, backing descriptor, requested byte, attachment, capture segment,
scope and failed-close counter returned to its initial zero baseline after
every cycle. The whole-run monitor also verified unchanged guest-init identity.

| Distribution | Each live capture | After each of five stops |
| --- | --- | --- |
| SteamOS | 51 segments/descriptors, 11,064,160 requested bytes, 102 attachments, 1 scope | All counters zero |
| Arch | 32 segments/descriptors, 3,982,080 requested bytes, 64 attachments, 1 scope | All counters zero |

[Allowlisted cycle counters and provenance](../tests/linuxrunner/steamos/capture-cleanup-2026-11-02.json)
retain the selected source hashes, app/test-bundle UUIDs and hashes of the
independent raw monitor/UI receipts. Device runs used the existing SteamOS
integration checkout; they are not standalone upstream-only builds or a new
performance A/B comparison. Private reports and full XCTest artifacts remain
local. The host contract independently passed its 80 kill cycles, and the
launcher reproduced byte-for-byte.

These results qualify the prior forced-exit buffer leak in the exercised paths.
They are not a sustained reliability soak. Real-x11vnc partial initialization,
later allocation/attach failure recovery, external outputs and simultaneous
scenes were not physically qualified. Failed-start and forced-orphan cleanup
were source-reviewed, not fault-injected. The physical runs preceded promotion of the default and explicitly selected
shared capture. The separately tested request policy now selects it for Arch
and SteamOS desktops while preserving explicit YES/NO choices; the launcher
still requires the cleanup capability. X11 control transport is unchanged.

After qualification, an ordinary SteamOS launch without the temporary capture
override passed native desktop readiness, Konsole `aarch64` through synthesized
input and a bounded text repaint. Its session log confirmed socket capture
(`shared-images=off`). This does not qualify physical Magic Keyboard delivery.

## Default and rollback

Open Desktop Settings → Display → Shared Image Capture to select shared or
socket capture for the next desktop session. Restart the desktop to apply it.
The existing global `LinuxRunnerGUISharedImages` preference is honored without
migration: an explicit false stays off, an explicit true stays on, and merely
reading the automatic default writes no preference. The switch reports the
requested policy; `GUI_CAPTURE` in session logs reports the effective mode after
capability fallback. An unavailable guard selects socket capture.

Host preference-policy verification (Foundation, no guest rootfs required):

```sh
xcrun clang -fobjc-arc -Wall -Wextra -Werror -I. -framework Foundation \
  tests/linuxrunner/gui/capture_policy_test.m -o /tmp/lr-capture-policy
/tmp/lr-capture-policy
```

The five-cycle physical test is retained as `CaptureCleanupUITests.m`. Its
manual monitor transfer and explicit environment gating are described above.
This PR excludes the SteamOS importer/GPU integration and the separate storage
scan changes. Physical measurements came from the existing integration app;
they are not physical qualification of a standalone build of this PR.
