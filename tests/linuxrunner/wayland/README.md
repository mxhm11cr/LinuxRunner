# Wayland tests

Use a disposable ARM64 guest. Debian guest files must use a case-sensitive volume on macOS.

`run-shm-probe.sh` starts Weston and tests xdg-shell, shared memory, file descriptor transfer, frame callbacks, and buffer release. It accepts `headless` or `x11`. The X11 backend requires MIT-SHM.

## Wayfire simulator test

`LinuxRunnerUITests/testWayfireSimulatorInput` uses the app's Open Desktop flow. It starts the app twice in sequence, selects Wayfire at 640 × 480, reconnects the viewer, moves the app to the background and foreground, sends a UIKit touch and a software keyboard key, and checks the Wayland client's event log. The default test enables direct KMS. Set `LR_WAYFIRE_TEST_DIRECT=NO` to test RFB rollback. The test removes old log output before launch. It skips unless the test runner has both required environment variables.

Prepare a Debian guest with the Wayfire profile and a normal Linux login. Use the packaged `/usr/bin/wayfire`, without a launch wrapper. The software path uses seatd, the virtual DRM device, Mesa softpipe, and the KMS bridge. Metal requires a build with virgl enabled. Keep simulator and physical-device results separate.

Build `shm-client.c` inside the guest with the stable xdg-shell protocol:

```sh
wayland-scanner client-header \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg-shell-client-protocol.h
wayland-scanner private-code \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg-shell-protocol.c
cc -O2 -I. shm-client.c xdg-shell-protocol.c -lwayland-client -lxkbcommon -o wayfire-input-probe
```

Put the test client at a path the selected Linux user can execute. Add this entry to that user's `[autostart]` section in `wayfire.ini`, with the correct executable path:

```ini
lr_test_input = env LR_WAYLAND_INPUT_SECONDS=120 LR_WAYLAND_REQUIRE_INPUT=1 /usr/local/bin/wayfire-input-probe > /tmp/wayfire-input.log 2>&1
```

Set these values in the test runner's `.xctestrun` `EnvironmentVariables`:

| Variable | Value |
| --- | --- |
| `LR_WAYFIRE_TEST_ROOT` | The simulator's prepared root name |
| `LR_WAYFIRE_TEST_USER` | The normal Linux login, defaults to `wayfiretest` |
| `LR_WAYFIRE_TEST_DIRECT` | `YES` for direct KMS, the default, or `NO` for RFB |
| `LR_WAYFIRE_TEST_LOG` | The host path to the guest's `data/tmp/wayfire-input.log` |

The client emits `WAYLAND_INPUT_READY` after three frames. It emits `WAYLAND_SHM_PASS` only after it receives the required input and a buffer release. The normal input test waits up to 120 seconds. Set `LR_WAYLAND_EXTENDED_INPUT=1` and `LR_WAYLAND_INPUT_SECONDS=300` for the extended test. It records key symbols, Unicode code points, modifiers, pointer movement, buttons, and wheel events. Normal Weston tests do not wait for input unless requested.

Check the new session directory in the guest exchange after each direct test. Require `display-transport` to contain `direct-kms`, `session.log` to report `x11vnc disabled`, and `kms-bridge.log` to report `native input ready (no X11 capture)`. The bridge must create no X window. Keep the session logs and XCTest screenshots with the result.

## Extended acceptance tests

`testWayfireExtendedInput` checks Shift release across reconnect, drag, exact delivery of 7,003 mixed ASCII and Unicode characters, and rejection of clipboard input above 16 KiB. Set `LR_WAYFIRE_TEST_CLIPBOARD` to an absolute host file path. The simulator test app reads this fixture and writes the clipboard through UIKit. This avoids Xcode 27 simulator pasteboard items that remain promised when set from another process. The fixture code is excluded from device builds. This test checks native clipboard delivery, but does not qualify cross-app clipboard permission prompts.

`testWayfireResolutionMatrix` checks all five display choices. Set `LR_WAYLAND_TEST_GPU=metal` for a build with virgl enabled. Set `LR_WAYLAND_TEST_PRESENTATION=YES` to use the Metal viewer. Confirm the compositor's `GL renderer` line reports virgl on ANGLE Metal. A requested Metal setting alone is not proof.

Set `LR_WAYLAND_TEST_DESKTOP=hyprland` and select a prepared Arch root to use the shared acceptance helpers with Hyprland. `testWayfireDeviceLifecycle` checks input, reconnect, rotation, background, foreground, and desktop close. It requires an installed guest on the selected device. Keep simulator and physical-device results separate.

`kms-snapshot-client.c` submits XRGB or ARGB scanout with padded rows and a nonzero offset, then clears the guest buffer. Build it with the distribution's libdrm headers. A host mailbox subscriber must verify the captured pixels after guest buffer reuse. This checks the DRM ioctl path in addition to the portable mailbox test.

## Xwayland and environment removal

`testWayfireXwayland` uses the production app request and launcher path to open the bundled Xlib test inside Wayfire or Hyprland. Set `LR_WAYFIRE_TEST_EXCHANGE` to the host path of the guest's `.linuxrunner/gui` directory. Set `LR_XWAYLAND_TEST_LOG` to the host path of `data/tmp/xwayland-input.log`. Also set the root, user and clipboard variables above. The test checks the visible window, its click counter, ASCII text, and Greek and emoji key events. The legacy Xlib test does not render these Unicode glyphs.

`testNormalWayfireStartup` checks the normal launch path with a prepared Wayfire environment and root selected as its Linux user. It requires an immediate prompt to select a normal Linux user.

`testRemoveEnvironment` requires a disposable environment. Set `LR_REMOVE_TEST_NAME` to its displayed name. The test cancels removal, then confirms removal. It deletes that environment. Keep at least one other environment installed. The native `environment-removal-contract` simulator workflow also checks failed staging, active environment protection, preference cleanup and external folder preservation.
