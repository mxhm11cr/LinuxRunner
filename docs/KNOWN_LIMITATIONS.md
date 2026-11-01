# Known Limitations

LinuxRunner executes ARM64 Linux userspace through its own runtime. It does not boot a Linux kernel, provide hardware virtualization, run x86 applications or implement every Linux system call. Kernel modules, general systemd-as-init workflows and conventional Linux containers are unsupported.

- **Hyprland:** experimental, Arch-only, at least 8 GB RAM, Metal recommended. First launch may need Retry; startup/performance optimization continues.
- **Other desktops:** Wayfire and Debian KDE Plasma remain experimental. Debian Enlightenment stops at its splash screen; Arch Plasma is not validated. Alpine has less desktop coverage.
- **Metal:** select the Linux GPU Device backend explicitly and fully restart. Automatic uses software in Build 900. Unsupported graphics paths can fall back or fail; the viewer using Metal is not proof of guest GPU acceleration.
- **JIT:** optional and restricted by signing/OS policy. Interpreter fallback remains available. Short programs can be slower with JIT translation overhead.
- **Devices:** 15.0 is the deployment target, not broad older-OS verification. An earlier iPhone 11/iOS 17 launch-black-screen report is unresolved. Memory/thermal limits affect demanding applications.
- **Networking:** host API compatibility, not unrestricted Linux network hardware access. Bluetooth networking is unsupported.
- **Displays:** one Linux output; receiver-dependent wired/AirPlay scene behavior. Mirroring cannot independently show the desktop and phone controls; Miracast-only receivers are unsupported.
- **Lifecycle:** app suspension can pause execution, and termination ends Linux processes. Save documents; running sessions do not survive app termination.
- **Isolation:** Linux accounts/permissions are compatibility features. The iOS application sandbox remains the security boundary.

See [Compatibility](COMPATIBILITY.md) for desktop/application scope and [Troubleshooting](TROUBLESHOOTING.md) for recovery that preserves files.
