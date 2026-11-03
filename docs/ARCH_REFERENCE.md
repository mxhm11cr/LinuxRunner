# Arch Linux ARM reference on iPad

The supported Arch Linux ARM installer completed on the physical iPad Air
13-inch (M4), iPadOS 27.2, in the local LinuxRunner 1.1.0 build 900 integration.
The original SteamOS and Alpine environments were preserved. The reference
uses its root shell; no test password or regular Linux account was created.

The native terminal reported `aarch64` and `ID=archarm`, with glibc
`2.43+r22+g8362e8ce10b2-2`, Bash `5.3.20-1` and Pacman
`7.1.0.r9.g54d9411-3`. Two physical XCTest runs opened the supported default
Openbox/LXQt-panel/PCManFM-Qt desktop, verified readiness, launched xterm through
the managed request queue, synthesized a tap and typed a command whose visible
output was `LR REF INPUT aarch64`.

This is basic launch and input qualification. It does not establish sustained
application compatibility or actual Magic Keyboard behavior. The desktop uses
Xvfb/RFB software rendering, a **960 × 540** framebuffer, Core Graphics
presentation, socket image capture and default VNC timing. SteamOS's Plasma,
accelerated Xorg and **1560 × 1112** output are different workloads; these
results are a regression reference, not a graphics-speed comparison.

## Unix-socket path correction

The first Arch desktop attempt installed its packages but could not establish
D-Bus: the host backing pathname for an abstract Unix socket exceeded Darwin's
`sun_path` capacity. The physical-device temporary directory plus decimal host
PID/socket ID left insufficient room. A targeted guest reproduction failed at
the tenth bind with `ENAMETOOLONG` (errno 36).

LinuxRunner now encodes the complete host PID/socket-ID pair in lowercase base
36, at most 13 bytes. The iOS prefix uses `/var` in place of `/private/var` only
after checking both directory identities. Guest pathname and abstract names
remain in their existing tables. No socket protocol, process working directory
or guest-visible namespace was changed.

The updated physical build passes 100 abstract bind/name/close cycles and a
100-byte guest pathname. The default Arch desktop now reports D-Bus ready and
all three desktop components present. Host tests also cover maximum ID values,
roundtrip uniqueness, exact capacity and unchanged failure buffers, verified
alias handling, and real Darwin datagram bind/send/receive.

## Checks and retained evidence

[Allowlisted device observations](../tests/linuxrunner/steamos/arch-reference-2026-11-02.json)
record the environment, package versions, results and limits without raw device
or account logs. Run the host socket check with:

```sh
scripts/test-native-unix-sockets.sh
```

`ReferenceDistributionUITests.m` skips ordinary runs unless a reference bundle
and installed root are explicitly supplied. Installation requires the separate
`LR_REFERENCE_INSTALL_ARCH=YES` opt-in. The desktop test accepts
`LR_REFERENCE_GUI_INPUT=YES`; start `reference_desktop_broker.py` before that
test, with the same device, bundle and root. The broker excludes preexisting
sessions and sends one xterm request only to a newly ready managed session.
Touches and text remain in XCTest. Its shell omits login profiles so account
startup commands cannot erase the probe before it is observed. The shared OCR
helper now preserves UIImage orientation when recognizing landscape captures.

The storage-policy A/B variant skips the terminal setup step, waits 70 seconds
after readiness (`LR_REFERENCE_QUIET_WINDOW=YES`), and uses the broker's
`--delay-after-ready 75` option. That keeps the scheduled 30–60 second native
counter window free of scripted interaction. Storage scans every installed
root, including the coinstalled SteamOS and Alpine environments. The resulting
CPU evidence belongs to the separate storage-scan PR; this socket correction
and the graphics integration are outside that patch.

The later [socket/shared capture probe](https://github.com/zachspartofaday/LinuxRunner/blob/codex/default-guarded-shared-capture/docs/CAPTURE_COMPARISON.md) tests input and
active repaint delivery separately from the storage comparison. It uses a
temporary instrumented app, exact per-input pixel IDs and a controlled Xlib
fixture. Its measurements exclude UIKit/hardware input and GPU/panel completion.

These are historical integration observations. See [the extracted PR scope and
validation limits](DESKTOP_PRESENTATION.md) before attributing them to this branch.
