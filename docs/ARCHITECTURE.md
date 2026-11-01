# How LinuxRunner Works

LinuxRunner hosts ARM64 Linux userspace inside an iOS/iPadOS application. It combines instruction interpretation, optional block translation and a Linux ABI compatibility layer. It does not boot a Linux kernel or use hardware virtualization.

```text
Debian / Arch Linux ARM / Alpine programs and libraries
       │ ELF loading, guest address spaces
       ▼
ARM64 interpreter ─── optional JIT, when iOS permits executable memory
       │
Linux syscall compatibility: processes, threads, signals, memory, files, sockets
       │                                      │
Host platform APIs                     Linux graphics/display services
       │                               X11 / supported Wayland compositors
       │                                      │
iOS/iPadOS storage and network          Mesa virgl → virglrenderer
                                              → ANGLE → Metal
                                       or software rendering
                                              │
                                Native viewer / external scene / input
```

## Load and Execute ARM64 Programs

The ELF loader maps ARM64 Linux executables and shared libraries into guest address spaces, prepares process state and transfers control to the runtime. The Asbestos interpreter decodes supported instructions into cached sequences of precompiled host routines (gadgets). A shared ARM64 architecture does not make this native execution of arbitrary Linux programs.

The optional JIT translates supported guest blocks into host machine code. Its region must pass iOS executable-memory permission checks; without permission the interpreter remains usable. The default source configuration disables JIT compilation, while the selected Build 900 IPA includes it. Runtime status distinguishes compiled capability, debugger attachment and actual execution. [JIT implementation](engine/IOS_JIT.md)

## Supply Linux Semantics

The compatibility layer handles files, memory mappings, pseudo-terminals, sockets, process/thread state, signals, synchronization and guest `fork`/`clone`/`execve` behavior. These are userspace implementations and translations onto platform operations, not system calls into a guest Linux kernel. Coverage is incomplete.

Each environment has its own root filesystem and package database. `fakefs` combines real backing files with SQLite-held Linux ownership/mode metadata. Import/export and per-environment shared folders provide selected file access. Storage uses the optional App Group when provisioned and a private fallback otherwise.

Guest processes remain within LinuxRunner's application lifecycle. They do not become independent iOS applications or secure containers. The host sandbox remains the security boundary.

## Integrate Linux Desktops

X11 sessions use Xvfb and x11vnc to feed the native RFB viewer. Managed launchers provide the selected user's display authorization and session variables. The default lightweight session uses Openbox, a panel and a file manager; LXQt, Enlightenment and Debian KDE use their own components where supported.

Experimental Wayfire/Hyprland sessions use LinuxRunner's virtual DRM/KMS display path. Compatible guest Mesa/virgl workloads send rendering commands to the app's virglrenderer integration, which uses ANGLE's Metal backend. Software rendering remains available. This is distinct from the earlier selective graphics interposer and from Metal presentation of an already-rendered framebuffer.

Metal is not universal graphics API support. The application, guest Mesa driver, compositor, memory limits and display path all matter. [Performance and activation](PERFORMANCE.md)

## Forward Display and Input

UIKit/Metal presentation shows frames in the app or an external scene. Touch, keyboard and supported pointer events are forwarded to Linux. DeX Mode adds a relative phone touchpad when a separate external scene is available. One guest desktop is displayed; this does not implement multiple independently extended Linux monitors. [Display controls](NETWORKING_AND_DISPLAYS.md)

## Network Through the Host

Linux sockets use platform networking. Interface/address/route queries translate available host information into Linux-facing forms, including netlink and proc views. There is no independent Linux network namespace or unrestricted radio/raw-device access. Bluetooth networking is unsupported.

## Implementation Map

- `asbestos/guest-arm64/`: interpreter gadgets, decoding and optional JIT.
- `emu/`, `kernel/`, `fs/`: execution state, syscall compatibility and filesystems.
- `graphics/`: guest graphics bridge and helpers.
- `app/LinuxRunner/`: environment/setup UI, native runtime bridge, desktop/viewer integration.
- `scripts/gpu/`, `scripts/build-gpu-deps.sh`: graphics dependency integration/builds.
- `tests/linuxrunner/`: runtime, graphics and compatibility contracts.

A full-system emulator models a machine and its kernel; a conventional VM runs a guest kernel through virtualization. LinuxRunner instead supplies Linux userspace compatibility with instruction interpretation/translation. [Limitations](KNOWN_LIMITATIONS.md) · [Security](../SECURITY.md)
