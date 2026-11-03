# SteamOS ARM64 userspace experiment

> Scope: the consolidated upstream SteamOS PR combines reviewed fork PRs #5
> (verified importing), #8 (installer) and #10 (guest graphics). It is based on
> upstream main. Full accelerated desktop use additionally needs the separate
> grouped desktop and GPU compatibility PRs; storage remains upstream PR #18.
> All final component heads passed external Codex review before merging into
> the fork. Prior physical observations below describe the integration, not an
> independent installation of this grouped branch. Historical capture opt-in
> and leak discussions predate the separately reviewed guarded default and
> cleanup implementation. Their original evidence and limits remain retained.

The supplied SteamFrame repair image runs a terminal and an X11 KDE Plasma
desktop in LinuxRunner on a physical iPad running iPadOS 27.2, after initial
validation in the macOS ARM64 host runtime. The app offers an experimental
installer for the specifically prepared image, with a bundled archive checksum,
storage checks, cancellation and regular Linux account setup. The user confirmed
KDE's wallpaper and panel, screen taps, Magic Keyboard trackpad clicks and
hardware typing into Konsole, where `uname -m` printed `aarch64`.

## Observed results

Tested on 2026-11-02 against LinuxRunner source `d141978`, using the interpreter
with JIT disabled and a real ARM64 guest vDSO:

- Bash 5.3.3, `uname -m` (`aarch64`), user identity, file write/read and
  fork/wait passed.
- Pacman 6.1.0 ran and queried the image's existing package database:
  `bash 5.3.3-3`. Package downloads, installation and upgrades were not tested.
- Xvfb at 960 × 540, KWin X11, Plasma and Konsole rendered under a dedicated
  regular guest account. The desktop probe checked managed window classes for
  both `plasmashell` and `konsole` and captured the framebuffer.
- Graphics used Qt software rendering with KWin compositing disabled. This
  does not establish Metal, Vulkan, gamescope, Steam client or game support.
- The managed app desktop profile passed its offline preparation check, started
  Plasma as a regular user through the normal session launcher, published
  `ready=1` and answered the RFB 3.8 handshake through x11vnc on the host.
- Native importer tests passed checksum rejection, source-pin rejection,
  symlink rejection, cancellation during copying and extraction, and source
  preservation. Swift manifest tests exercise size bounds and overflow.
- The first iPad import exposed PAX UTF-8 filename conversion under iOS's C
  locale. The importer now preserves the original name bytes supplied by
  libarchive's PAX fallback for that specific conversion warning, after checking
  that the fields contain valid UTF-8. It requires no installed UTF-8 locale and
  changes no thread/process locale. Other parse/codec warnings remain fatal.
  A regression linked against the bundled libarchive 3.4.3 reproduced the old
  failure and tests non-ASCII filenames, symlink targets, malformed UTF-8 and
  truncated gzip rejection under the C locale. Archive integrity is checked
  separately against the pinned SHA-256 before extraction. Failed/cancelled
  imports also release their archive, database and file handles.
- The first installed iPad session reached X11 and KWin but its Plasma panel
  and desktop surface did not become ready within four minutes. Discover's
  Flatpak notifier crashed, and iPadOS rejected several host memory mappings.
  The SteamOS session now defaults Discover, Baloo indexing, the Python firewall
  applet and IBus out of autostart, preserving explicit per-account overrides.
  Baloo attempted a 256 GiB shared mapping; these services are outside the
  desktop/terminal milestone. The updated test app was signed with expanded
  virtual addressing through `SteamOSExperiment.entitlements`, then installed
  and launched on the iPad. This does not enable JIT or change the anonymous
  memory budget. The updated host session passed panel/surface readiness and
  ran an ARM64 shell through Konsole with those applets absent. The iPad then
  reported `ready=1` with panel and surface present after 15 seconds, and a user
  screenshot confirmed KDE's wallpaper and panel. A `tags:/` error prompted
  explicitly disabling indexing in KDE's configuration as well as autostart.
- Physical mouse testing exposed a generic input-source transition bug: the
  first GCMouse press could be accepted before UIKit identified an indirect
  pointer, after which its release was ignored and the button remained held.
  GCMouse now consistently owns physical buttons while UIKit supplies absolute
  pointer positions. Duplicate UIKit button events are suppressed; direct
  finger/Pencil clicks and UIKit-only pointers remain supported. Policy tests
  cover the transition, and input diagnostics record forwarded button edges.
  The updated app was installed and launched on the iPad. Direct screen taps
  worked before this fix, and the user confirmed Magic Keyboard trackpad clicks
  afterward. The native RFB transport probe preserved all button,
  key and wheel transitions during 9,051 queued input events, while coalescing
  redundant pointer motion.
- Subsequent iPad interaction exposed a Plasma crash in Qt's raster painter
  called by `breeze6.so`. The SteamOS session selects Qt's Fusion widget style,
  including applications activated through its session bus. Hardware typing
  also requires the native framebuffer view to become first responder: it now
  requests that focus on a desktop click when a hardware keyboard is present.
  Focus and key-event counters omit typed content. A host probe sent a command
  through the native RFB client into Konsole and verified the file written by
  that shell; this establishes transport/terminal input separately from UIKit
  hardware-keyboard focus.
- The final updated app was built, installed and launched on the same iPad.
  Its trace recorded panel/surface readiness after 18 seconds, successful native
  keyboard focus and hardware key-down/key-up delivery. The user confirmed that
  typing into Konsole worked and `uname -m` printed `aarch64`. This completes the
  initial physical desktop/terminal milestone. Longer interactive sessions,
  restarts across OS updates and Steam/game compatibility are not qualified.

The image's default Wayland session invokes
`start-gamescope-session plasma-session.target`. The experiment instead uses
`dbus-run-session -- startplasma-x11` and sets `systemdBoot=false` in the test
user's `startkderc`. Steam autostart is disabled for that account. Auxiliary
services still report missing system bus, FUSE, PipeWire and polkit integration;
the desktop result does not qualify those services or long-running interactive
use.

## Software-renderer notification

The verified V7 iPad build uses software rendering, so Plasma's **Software
Renderer In Use** notification is expected there. The new GPU candidate adds
an opt-in path under **Settings → Graphics → Linux GPU Device → GPU → Metal
GPU · Experimental**. Force-close and reopen LinuxRunner after changing it,
then open the existing desktop. No rootfs reinstall is needed.

The candidate uses Xorg modesetting/glamor and Qt Quick OpenGL on virgl/Metal.
KWin window effects remain disabled: its compositor does not yet finish
initializing reliably in this environment. This does not qualify games,
Vulkan, arbitrary OpenGL applications or every border-color mode. The Linux
application renderer picker and Metal framebuffer presentation are separate
controls; neither replaces the Linux GPU Device setting.

### Reusing the Arch GPU stack

SteamOS reuses the repository's virtual DRM/KMS device, Mesa virgl driver,
host virglrenderer and ANGLE Metal backend. The current Arch Linux ARM Mesa
package (`1:26.2.6-1`) requires `GLIBC_2.43`; this image supplies glibc 2.39.
Installing that package or replacing glibc is not a compatible driver update.

Instead, Mesa 26.3 development revision
`590bf21d918c86908d96d1f4590ecd25b9657171` was cross-compiled against the
image's headers and libraries, with prefix `/opt/linuxrunner/steamos-gpu`.
System Mesa remains unchanged. The Meson options are:

```text
--prefix=/opt/linuxrunner/steamos-gpu --libdir=lib --buildtype=release
-Dgallium-drivers=virgl -Dvulkan-drivers= -Dplatforms=x11,wayland
-Dglx=dri -Dglvnd=enabled -Degl=enabled -Dgbm=enabled -Dllvm=disabled
-Dgallium-va=disabled -Dgallium-rusticl=false -Dgles1=disabled -Dgles2=enabled
-Dbuild-tests=false -Dtools= -Dvideo-codecs= -Dvalgrind=disabled -Dlibunwind=disabled
```

Use an AArch64 Linux cross toolchain, matching Wayland 1.26 scanner, LLVM
archiver and the image's glibc 2.39/GCC 15.1.1 sysroot. Apple `ar` does not
produce suitable ELF archive indexes. The local experiment's generated
cross file and compiler wrappers are under `build/steamos-gpu-deps`; its
configure arguments are retained in `build/steamos-device/mesa-configure-input.json`.
Never build against the host macOS SDK or newer Arch libraries.

The host bridge implements transparent-zero sampler borders using Metal's
clamp-to-zero. ANGLE keeps the full GLES border-color extension unadvertised;
virglrenderer explicitly rejects unsupported nonzero borders. The host also
resets its GL context before embedder operations to protect guest framebuffer
state. Virtual CRTC time advances at 60 Hz independently of page flips, and
modern/legacy vblank events wake Xorg/Present asynchronously. Event queues are
bounded, and closing a file cancels and joins its event worker.

### Package and validate an update

Build the pinned host/iOS dependencies as described in `BUILDING.md`. Build
`tests/linuxrunner/gpu/virgl_border_contract.c` against the image's sysroot
with `-lEGL -lGLESv2 -lgbm`, then package Mesa's staged prefix:

```sh
python3 tools/steamos/package-gpu-overlay.py \
  --stage build/steamos-device/gpu-stage/opt/linuxrunner/steamos-gpu \
  --mesa-source build/steamos-gpu-deps/mesa \
  --probe build/steamos-device/gpu-probe/virgl-border-contract \
  --output build/steamos-device/gpu-package --bundle
```

The packager uses the standalone probe source when the host GPU prerequisite
is present. In this scoped checkout it can recover the same preferred source
from the checksum-verified bundled overlay, checking its BUILD.json identity.
A refusal to modify custom tags-worker paths is nonfatal to desktop startup.

The output directory must be new. The packager checks architecture and
symlinks, retains licenses/source identity, normalizes archive metadata, and
regenerates the GUI resource manifest. `steamos-gpu.sh` verifies and stages
this overlay before publishing the isolated prefix. Existing installations
receive it on the next GPU desktop start. Missing GPU support or a failed
startup pixel check selects the established software desktop.

Each GPU desktop start records the renderer and seven RGBA border/filtering
comparisons in `graphics.log` before starting Xorg. `xorg.log` records the
actual glamor driver. GUI diagnostic exports include both files. Select
Software Rendering and restart LinuxRunner to return to the software path.

The macOS ARM64 host checks pass for:

- All four existing `virgl_render_contract.c` pixel comparisons.
- Seven transparent-border pixel comparisons, including filtered edges and
  mixed wrap modes, plus rejection of unsupported nonzero sampler borders.
- CRTC time without flips, asynchronous modern/legacy events, queue bounds,
  cancellation on close and the existing KMS/PRIME contract.
- The packaged driver installation, Xorg/Qt renderer identity, visible KDE
  panel and desktop, Konsole launch and fresh RFB keyboard output.
- Shift punctuation/case (`1!Aa?A`) through Konsole, including releasing Shift
  before the letter key. `keyboard_policy_test.m` also checks layout-aware
  text, Caps Lock, Control/Shift and Option shortcut mapping.

The GPU-capable V8 app was built, signed, installed and launched on the
connected M4 iPad using MobileBuildMCP 2.7.1, with JIT disabled. The physical
startup log identifies virgl/ANGLE on the Apple M4 GPU, all seven startup pixel
comparisons pass, and Xorg reports the accelerated backend. The operator
confirmed the desktop looks correct and Magic Keyboard Shift produces `!`
and `A`. KWin effects remain disabled. The previously verified V7 software
app is retained locally for rollback.

### Resolution and performance follow-up

The same iPad reported a 1566 × 975 point viewer with a fixed 960 × 540 Linux
desktop. SteamOS on iPad now defaults to Fit Screen, measured from the actual
viewer after immersive layout. The old Wide default is migrated once for this
environment; subsequent explicit choices and other distributions are retained.
Fit uses logical points with the existing 1600 × 1200 cap, not Retina pixels.
Changing guest resolution still requires stopping and starting the desktop.

For SteamOS with the Linux GPU active, the viewer defaults to its existing
Metal presenter unless the user has explicitly chosen Core Graphics. It
uploads accumulated damage to reusable textures rather than taking a complete
Core Graphics framebuffer snapshot for each update. The existing single-thread
adaptive x11vnc policy reduces capture delays during recent input while retaining
25 ms idle polling. The threaded input mode remains opt-in because it previously
failed browser text-integrity checks.

A physical Time Profiler recording also identified `StorageUsage.bytes` under
`EnvironmentManager.refresh` as 57.5% of running CPU samples in its captured
interval. The retained dSYM UUID is `7AAD93F0-A52E-3F15-A9B4-94D01271185C`;
the profile's main return address establishes image base `0x104888000`.
`atos` resolves the hot native frames to the storage walk and manager refresh.
The manager now cancels a running storage walk when a graphical viewer opens,
defers automatic storage refresh until it closes. Complete cached storage totals remain visible; cancelled
walks never publish partial or zero totals. Cancellation is checked between
files, and generation checks prevent an old task overwriting a newer scan.
The full Storage settings measurement remains available on request.

The prepared SteamOS profile also defaults Linux PowerDevil and the polkit
agent out of autostart, preserving explicit account overrides. iPadOS provides
the host's power/brightness/authentication; this environment lacks the Linux
system bus those agents repeatedly query. These changes are candidates for
physical performance measurement, not a claim of a particular FPS improvement.

KIO 6.14 unconditionally enumerates `tags:/` when its worker is installed, even
with Baloo indexing disabled. Disabling the daemon/config alone therefore did
not prevent the recurring error. For accounts using the managed Baloo-off
default, `steamos-desktop.sh` retains `tags.so` under
`/opt/linuxrunner/steamos-disabled-plugins`, outside Qt's plugin search path.
It does not delete the plugin or an index. An explicit account autostart
override restores the saved worker at next desktop start. To restore it manually,
temporarily run `sh steamos-desktop.sh restore-tags` as root from the managed support directory.
Unrecognized replacement copies and symlinks are left untouched.

The opt-in `LinuxRunnerDesktop.performanceDiagnostics` launch preference writes
bounded session-local `performance.jsonl`: monotonic time, process CPU time,
physical memory footprint, thermal category, received updates, presenter counters
and aggregate input latency. It records no typed content. The focused XCTest
`testSteamOSDeviceDesktopPerformance` requires explicit bundle/root environment
variables and exercises the existing desktop through real device UI automation.
It does not import or reinstall Linux.

The V9 and storage-only V11 physical UI runs both passed desktop readiness, real touch/text
input into Konsole, and visible `LR_PERF_aarch64` output. The final iPad session
used a 1560 × 1112 guest framebuffer for a 1566 × 1119 point viewer, selected
Metal presentation and adaptive capture, passed all seven GPU pixel checks,
and had no `tags:/` errors in its Plasma log.

In the paired native metrics snapshots 30–60 seconds after session metrics
began, before scripted interaction, process CPU use was 122.3% of one core
with the background storage scan and 37.2% after cancellation. The paired 30-second windows
had nominal thermal state and at most one display update. This is one paired
quiet-window measurement of process CPU, not a desktop FPS or sustained
performance claim. The earlier CPU trace independently identifies the storage
walk. The final app's framebuffer upload CPU time remains a component counter,
not an end-to-end display measurement.

The controlled macOS Xlib fixture also completed 30 click/repaint samples per
policy: legacy median 91.9 ms, adaptive 26.7 ms (p95 94.8 and 79.7 ms). This
measures Linux input through native RFB decode at 640 × 480; it excludes physical
UIKit and GPU completion and does not establish SteamOS desktop FPS.

### Verified GPU, capture and text follow-up

A fresh physical iPad audit identifies `ANGLE Metal Renderer: Apple M4 GPU` in
both Xorg's enabled glamor backend and Plasma's QRhi/OpenGL context. All seven
startup GPU pixel checks pass. This establishes accelerated rendering for those
components; KWin effects remain off and guest CPU instructions are interpreted.

The panel is **2732 × 2048**, while this session renders a **1560 × 1112** Linux
framebuffer in a **1566 × 1119 point** viewer. iPadOS reports backing scale 2 and
native scale 1.7181208053691275. The app's screenshot/backing resolution therefore
differs from the physical panel. Fit Screen's logical-point size/cap is a
LinuxRunner policy, not a headset display setting. Full native-resolution/DPI
rendering is not implemented by this follow-up; it requires a larger guest
framebuffer and more capture/rendering work.

The same-binary socket/shared-image comparison passed physical desktop readiness,
Konsole, `aarch64`, synthesized input and a bounded terminal repaint workload.
x11vnc's startup framebuffer-read calibration increased from **31 to 1226 MB/s**
with shared images. That is a component check, not a desktop FPS or sustained
bandwidth result. Shared capture alone used **40.8% versus 38.1% of one CPU core**
in the retained paired quiet window. Only two touch events were measured per run,
so the result does not establish an interaction-latency improvement.

SysV shared-image capture remains explicitly opt-in through the existing X11
shared-images switch. The independent review found that x11vnc marks successful
segments for deletion during cleanup; a forced `SIGKILL` skips that cleanup and
can retain their backing resources until the LinuxRunner app process exits.
Session-owned crash cleanup has not been qualified, so no distribution receives
an automatic shared-capture default. Existing explicit choices are preserved.
After a forcibly terminated shared-capture session, fully restart LinuxRunner
before retrying; restarting the desktop alone does not reclaim orphan segments.

The established adaptive timing is retained; the slower idle policy did not
establish better interaction. A short device profile still found substantial
Xorg/x11vnc interpreter activity; native Metal presentation upload CPU time was
small in these runs. The evidence does not isolate X11 control-message costs or
justify replacing their transport with a custom shared-memory command queue.
Such a queue would still need ordering, wakeups, ownership and crash recovery.

The font audit found Noto Sans/Noto Sans Mono and standard RGB subpixel
antialiasing at 96 DPI. The managed SteamOS default now chooses grayscale
antialiasing (`Xft.rgba: none`) for this scaled/rotatable virtual display.
`steamos-fonts.sh` uses KConfig, preserves explicit account subpixel preferences
and custom configuration paths, and cannot prevent desktop startup on failure.
This avoids assuming a fixed RGB stripe layout through scaling; subjective text
clarity and a full native-resolution desktop remain separate questions.

[Allowlisted measurements and limits](../tests/linuxrunner/steamos/graphics-followup-2026-11-02.json)
retain the paired counters and binary UUID without raw device/account logs.
`test_steamos_font_defaults.py` verifies defaults and preservation behavior;
`test_desktop_configuration.py` checks existing configurations and shell syntax.
The focused physical UI test has explicit capture overrides for comparisons and
an optional short smoke mode, which omits the quiet-window measurement.
The pre-review build passed that physical smoke test through completed 30-line
terminal output, with its then-default shared capture, adaptive timing, grayscale
fonts, and Xorg/Plasma M4 renderer identity confirmed again. Those receipts are
retained as historical validation of that candidate, not of a default promotion.
The review correction restores opt-in capture; a subsequent physical SteamOS
startup reached desktop readiness with `shared-images=off`, adaptive timing and
grayscale fonts. The final post-Arch short test also passed Konsole input,
`aarch64` and the completed 30-line repaint workload with capture off and no
launch override. Xorg/Plasma again identified the M4, and all seven GPU pixel
checks passed. The font audit found `Xft.rgba: none`; this existing account's
explicit key was preserved. Six font-default checks and thirteen existing
desktop-configuration/shell-syntax checks passed on the host. The installed
root filesystem was reused throughout.

The [supported Arch reference](ARCH_REFERENCE.md) is now installed and passes
basic desktop/xterm input qualification too. Its D-Bus startup exposed a native
Unix-socket backing-path limit, corrected independently of the graphics policy.
Arch uses its existing lightweight software desktop at 960 × 540, so this is a
regression reference rather than a matched Plasma graphics benchmark.

A subsequent [controlled active capture comparison](CAPTURE_COMPARISON.md)
measured 30 click and 30 keyboard responses per mode, plus numbered full-screen
repaints, on both Arch and SteamOS. Shared capture improved those native
submission timings and delivered more distinct repaints. These are synthetic
capture-path measurements, with a recorded thermal-state difference in the
SteamOS shared run; GPU completion and physical panel latency remain unmeasured.
The known forced-exit resource lifetime still keeps shared capture opt-in.

Further performance work should measure sustained real-application workloads
and native/panel presentation separately. The host GPU bridge currently waits
for completion after each command submission; changing that requires real
asynchronous fence and DRM wait semantics, rather than removing correctness
barriers. KWin effects and game compatibility remain outside this result.

## Networking

LinuxRunner translates guest Linux sockets to iPadOS sockets. Wi-Fi and
Ethernet hardware remain managed by iPadOS; Linux Wi-Fi kernel modules and
firmware from Arch or SteamOS are not used in this userspace environment.
The app's existing DNS setup supplies the iPadOS resolver to every distro,
including SteamOS.

The maintained read-only check is `tests/linuxrunner/steamos/network.sh`. It
checks IPv4 name resolution and an HTTPS request with certificate verification
and HTTPS-only redirects. Both checks passed in the disposable host root.
On 2026-11-02 the operator also ran the equivalent `getent` and `curl` commands
in Konsole on the connected iPad and reported `DNS_OK` and `HTTPS_OK`. This
confirms DNS and outbound HTTPS on that iPad's current connection; Steam login,
downloads and other network services have not been qualified.

## Source image

Local filename: `SteamFrame OOBE Repair 2026-09-22.img.bz2`.

Local compressed-image SHA-256:

```text
3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676
```

This records the supplied bytes; it is not an independently authenticated Valve
checksum. The initial image probes were offline. Device-image preparation adds
two pinned Arch Linux ARM display packages; it does not use the SteamOS image's
package repositories.

The image is a 7 GiB GPT disk containing a 5 GiB Btrfs `rootfs-A` partition.
Its `/etc/os-release` identifies SteamOS `0.3.0`, variant `vr`, build
`20260922.5152327`. Bash, Pacman, Xvfb, KWin and Plasma are ARM64 ELF binaries.
The root partition's type GUID advertises x86-64 despite those ARM64 contents;
the exporter checks the binaries themselves.

## Inspect and export locally

Use Python 3.10–3.14; the pinned Btrfs parser is incompatible with Python 3.15.
The tool reads a local raw `.img` or `.img.bz2`, validates GPT checksums and
SteamOS/ARM64 identity, and optionally writes an uncompressed userspace tar.
It never runs image scripts or extracts guest paths onto the host filesystem.
Existing output files are refused.

```sh
python3.14 -m venv build/steamos-tools
build/steamos-tools/bin/pip install -r tools/steamos/requirements.txt
mkdir -p build/steamos-probe

# Inspection only. Replace the image path with your local copy.
build/steamos-tools/bin/python tools/steamos/export-steamframe.py \
  '/path/to/SteamFrame OOBE Repair 2026-09-22.img.bz2' \
  --report build/steamos-probe/image-report.json

# Export separately, or combine --report and --output in one invocation.
build/steamos-tools/bin/python tools/steamos/export-steamframe.py \
  '/path/to/SteamFrame OOBE Repair 2026-09-22.img.bz2' \
  --output build/steamos-probe/steamos-rootfs.tar
```

Btrfs requires random access. A compressed input is decompressed into a
temporary file, with a default 16 GiB limit. Passing an already decompressed
image avoids repeating that work. The tested export contains 226,048 entries
and is approximately 8.4 GiB; allow space for both the archive and an imported
filesystem, plus 7 GiB of temporary space when using compressed input.

The export preserves Linux ownership, modes, symlinks and hardlinks. It omits
boot/EFI/persistent partitions, kernel modules, firmware and debug files;
`dev`, `proc`, `sys`, `run` and `tmp` are exported as empty directories. Separate
`var-A` and `home` partitions are not merged into the export. This is a
development userspace export, not a recovery or bootable image.

## Reproduce the host checks

Follow [Building LinuxRunner](BUILDING.md) for dependencies, including Meson,
Ninja, libarchive development files and `clang` with `ld.lld` on PATH. Require
the real vDSO: a stub-only host build failed Bash signal return during the
initial experiment.

```sh
meson setup build/steamos-host -Dengine=asbestos -Dguest_arch=arm64 \
  -Djit=false -Djit_emit=false -Dcli_aot= -Dvdso_required=true
ninja -C build/steamos-host ish tools/fakefsify
```

Import onto a **case-sensitive** volume. The complete image contains both
`usr/lib/Xorg` (a file) and `usr/lib/xorg` (a directory); the import fails on a
case-insensitive macOS volume. For example, create an empty test disk image:

```sh
hdiutil create -size 16g -type SPARSE -fs 'Case-sensitive APFS' \
  -volname LinuxRunnerSteamOSProbe build/steamos-probe/steamos-test.sparseimage
mkdir -p build/steamos-probe/case-sensitive
hdiutil attach -nobrowse -mountpoint "$PWD/build/steamos-probe/case-sensitive" \
  build/steamos-probe/steamos-test.sparseimage
build/steamos-host/tools/fakefsify build/steamos-probe/steamos-rootfs.tar \
  build/steamos-probe/case-sensitive/rootfs
export CPU_ROOTFS="$PWD/build/steamos-probe/case-sensitive/rootfs"
```

Run these checks only in that disposable filesystem. The desktop check writes
configuration for a dedicated test account, disables its Steam autostart, runs
for approximately 50 seconds, captures window identities and a framebuffer,
then stops the display.

```sh
build/steamos-host/ish -f "$CPU_ROOTFS" /bin/bash --noprofile --norc \
  -ec "$(cat tests/linuxrunner/steamos/terminal.sh)"

build/steamos-host/ish -f "$CPU_ROOTFS" /bin/bash --noprofile --norc -ec \
  'id -u lrprobe >/dev/null 2>&1 || useradd -m -U -s /bin/bash lrprobe; exec runuser -u lrprobe -- /bin/bash --noprofile --norc -ec "$1"' \
  bash "$(cat tests/linuxrunner/steamos/desktop.sh)"
```

Expected markers are `STEAMOS_TERMINAL=PASS` and `STEAMOS_DESKTOP=PASS`.
Desktop logs and the XWD framebuffer are under the guest path
`/home/lrprobe/steamframe-probe/`. Save desired evidence before detaching the
test volume:

```sh
hdiutil detach "$PWD/build/steamos-probe/case-sensitive"
```

The exporter contract tests run without third-party dependencies:

```sh
python3.14 -m unittest discover -s tests/linuxrunner/steamos -v
```

## Prepared device image

The app installs a prepared `rootfs.tar.gz`, rather than the raw disk image.
`SteamOSSupport.bundle/manifest.json` pins its exact size, SHA-256, original
image identity and expected installed size. Other distributions retain their
512 MiB import limit; only a matching SteamOS pin admits the larger archive.
Copying, verification and extraction run off the UI thread with cooperative
cancellation. A completed filesystem is published under a new environment ID;
the original archive and existing environments are kept.

The device preparation tool requires the disposable case-sensitive fakefs root
from the previous steps. It adds checksum-pinned x11vnc 0.9.17 and
libvncserver 0.9.15 packages, records them in the SteamOS package database, and
builds the four LinuxRunner desktop helpers using that image's GCC. Their
checksums and source identities are checked by the app's offline desktop
profile. The root must be imported from the supplied image; use it only for
preparation, before creating test users or running desktop probes.

```sh
ninja -C build/steamos-host tools/unfakefsify
python3.14 tools/steamos/prepare-device-image.py \
  --source-image '/path/to/SteamFrame OOBE Repair 2026-09-22.img.bz2' \
  --root "$CPU_ROOTFS" --cache build/steamos-arch-cache \
  --output build/steamos-package \
  --app-manifest app/LinuxRunner/SteamOSSupport.bundle/manifest.json
```

The prepared archive for this candidate is 3,883,082,382 bytes. Installation
needs approximately 13.3 GB of additional free space for its verified snapshot,
expanded filesystem and working reserve. Rebuilding the prepared image may
change its checksum: rebuild the app with the corresponding generated manifest.
This is a local-image workflow; the app provides no SteamOS download endpoint.

Build and install the normal LinuxRunner scheme using your Apple development
team. For a separate test installation, override `ROOT_BUNDLE_IDENTIFIER` and
`LINUXRUNNER_DISPLAY_NAME` and use `app/LinuxRunner/SteamOSExperiment.entitlements`
for private-container storage with expanded virtual addressing on supported
devices. The signing profile must allow that entitlement.
Do not override `PRODUCT_NAME` globally: that also renames dependent libraries.

Copy the prepared archive to the test app's `Documents/SteamOS/rootfs.tar.gz`,
or select it through **Choose Prepared SteamOS Archive…**. Open
**Distributions → Set Up Linux → Continue → SteamOS ARM64 → Continue →
Install Prepared SteamOS**. Keep the app in the foreground until completion.
When requested, force-close and reopen the app, create or select a regular
Linux account, then choose **Check SteamOS Desktop → Continue → Open Desktop**.

The initial desktop uses Plasma X11 with software rendering and Steam autostart
disabled for newly configured accounts. Installed-package inspection is
available; managed repository updates and package changes are disabled for
this hardware-specific image experiment. Audio and system services remain
outside this milestone. Gamescope, Vulkan and local games are separate work.

The isolated storage-lifecycle change and allowlisted before/after evidence were
submitted upstream as [PR #18](https://github.com/mxhm11cr/LinuxRunner/pull/18).
