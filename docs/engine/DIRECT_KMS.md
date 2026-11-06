# Experimental direct KMS display

Enable **Desktop Settings → Display → Resolution & Viewer Preferences → Direct Wayland Display · Experimental**, then stop and start the desktop. The preference is `LinuxRunnerDesktop.directKMS` and defaults to off. Hyprland and Wayfire use the direct transport with software KMS or virgl GPU rendering. Wayfire requires seatd and the KMS bridge build dependencies. Select a non-root account in Settings → Linux Users before starting a Wayland desktop. Other desktops keep RFB. Wayfire keeps its KMS renderer when the preference is off, and the bridge sends its frames and input through X11/RFB. Wayland compositors use Mesa on the selected KMS device. They do not load the GLX application bridge. Existing sessions retain their original transport when the viewer reconnects. Disable the preference and restart the desktop to restore RFB.

The path is:

```
compositor → virtual DRM/KMS → owned BGRA snapshot → native viewer → screen
native input → bounded host queue → DRM input ioctl → Wayland virtual input
```

`fs/drm.c` takes a snapshot after synchronizing GPU scanout and before completing the modeset/page flip. Buffer pitch and offset are honored; XRGB/ARGB scanout is flattened to opaque BGRA. The guest may reuse its buffer after flip completion without modifying the snapshot. Snapshots are limited to the viewer's 2560 × 1600 maximum. A mutex protects the latest-frame mailbox; a slow viewer replaces pending frames rather than accumulating them. Viewer tokens prevent a detached client from consuming or stopping a newer client's mailbox. Reconnect explicitly snapshots the current scanout, including an idle desktop.

The native viewer drains the mailbox at up to 60 Hz on its main run loop and uses the existing Core Graphics/Metal presentation and external scene. This removes X11 scanout copies, x11vnc capture/encoding, RFB sockets and decoding from the direct display path. Xvfb remains running for existing session setup, diagnostics and compatibility; removing the auxiliary X server is separate work. `lr-kms-bridge --native-input` opens no X connection and creates no X window. Session readiness uses the compositor's virtual input connection and a submitted native frame, rather than a mapped bridge window. The session explicitly reports termination to the native viewer.

Input preserves pointer button transitions, drag, wheel and key ordering. Consecutive motion with the same button mask coalesces. Virtual keyboard events use the US physical map. Unicode text uses the same keyboard, with 32 cached extra symbols and stable keycodes within the X11 limit. This keeps client focus and avoids rebuilding a map for each repeated character. Clipboard input is chunked with queue backpressure; disconnect/reconnect releases held keys and buttons. The queue and native input helper are for the active runtime, not a security boundary between guest programs.

Wayfire and Hyprland also support X11 applications through Xwayland. Desktop preparation installs Xwayland. The compositor publishes its `DISPLAY`, `WAYLAND_DISPLAY` and authentication file for terminals and app launcher requests. Start an X11 application from the desktop or app launcher. Its window appears inside the Wayland desktop. Restart the desktop after updating an existing installation so it can publish the new environment.

This is **not zero-copy GPU presentation**. Virgl scanout still performs GPU readback, the host copies CPU pixels into an owned frame, and Metal presentation uploads pixels. Idle viewer polling and full-frame copies remain. Shared GPU surfaces, damage-aware snapshots, and event-driven delivery are possible follow-ups. No device FPS or latency improvement has yet been measured.

## Verification

Portable mailbox tests (latest-frame replacement, padded rows, opaque alpha, ownership after reuse, bounds, input ordering, motion coalescing, overflow, stale tokens and concurrent publication):

```sh
cc -I. -pthread -Wall -Wextra -Werror -fsanitize=address,undefined tests/linuxrunner/gui/kms_display_test.c -o /tmp/kms-display-test
/tmp/kms-display-test
```

On Linux with X11, Xext, Wayland client and xkbcommon development packages and xkb-data installed, test input translation using real xkb maps and recorded Wayland requests:

```sh
cc -I. -Wall -Wextra -Werror tests/linuxrunner/gui/kms_input_test.c \
  $(pkg-config --cflags --libs x11 xext wayland-client xkbcommon) \
  -Wl,--wrap=ioctl,--wrap=wl_proxy_marshal_constructor,--wrap=wl_proxy_marshal,--wrap=wl_display_dispatch_pending \
  -o /tmp/kms-input-test
/tmp/kms-input-test
```

Also run shell syntax checks for session.sh and desktop-components.sh, the desktop configuration/supervision tests, and `python3 scripts/generate-gui-support-manifest.py --check`.

For the software Wayfire path in the iOS simulator, prepare the guest and run `LinuxRunnerUITests/testWayfireSimulatorInput` as described in [the Wayland test instructions](../../tests/linuxrunner/wayland/README.md). The test checks app restart, viewer reconnect, background and foreground transitions, and real Wayland client input. Check the session transport logs as well as the XCTest result.

Before enabling by default, build on macOS and test on a physical device: Hyprland with software and Metal rendering, GPU Wayfire, idle reconnect, long ASCII/Unicode paste, held modifiers and drag during detach, external output/touchpad, background/foreground, compositor exit and RFB rollback. Compare identical workloads/resolutions with direct transport off/on, measuring CPU, frame pacing, input-to-visible-update latency and memory. A faster renderer is not evidence that the transport is faster.
