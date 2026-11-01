# Compatibility — LinuxRunner 1.1.0 (900)

Compatibility depends on the distribution, package version, device memory, OS and execution/graphics path. Installing a package does not establish that every feature works.

## Devices and Distributions

The retained IPA is ARM64, device families iPhone/iPad, minimum OS metadata 15.0. Physical iPhone and iPad use is confirmed by the maintainer; no universal model/OS qualification is implied. Older OS testing is limited, with an unresolved earlier iPhone 11/iOS 17 launch-black-screen report. [Requirements](../README.md#supported-devices-and-requirements)

Debian 13 ARM64 and Arch Linux ARM are recommended for desktop applications. Alpine 3.24 ARM64 supplies a small `apk` terminal environment with less desktop integration coverage. Only one installed environment is active per application launch. Switching uses **Use on Next Launch** and a full app relaunch.

## Desktop Combinations

| Desktop | Debian | Arch | Alpine |
| --- | --- | --- | --- |
| LinuxRunner Desktop | Main lightweight path | Openbox / LXQt components | Experimental desktop path |
| LXQt | Available | Available | Lower coverage; prefer Debian/Arch |
| Enlightenment | Disabled: splash/black-screen startup problem | Available | Not qualified |
| Wayfire | Experimental | Experimental; GPU path implemented | Not offered |
| Hyprland | Not offered | Experimental, 8 GB RAM minimum; Metal recommended | Not offered |
| KDE Plasma | Experimental X11 session | Current Plasma not validated | Not offered |

Hyprland's first launch can need **Retry**. LinuxRunner Desktop remains the fallback. Experimental Wayland support does not establish complete Wayland/KWin compatibility.

## Application Results

| Application | Current evidence and scope |
| --- | --- |
| Vesktop / Discord | Maintainer reports full functionality tested in the new release. Supplied capture shows its interface; login QR is redacted. Exact distro/package version was not supplied. |
| Telegram Desktop | Maintainer tested login, messages and calls on Arch using Build 897. New release does not imply every package version has been retested. |
| Firefox | New supplied capture shows browsing the LinuxRunner GitHub repository. Complete browser/media/site compatibility has not been established. |
| Visual Studio Code | New supplied capture shows its editor opening a C project. Extensions/debuggers are not thereby qualified. |
| FreeCAD | New supplied capture shows its modeling interface with Metal selected. Full CAD workflows are not established by this image. |
| ONLYOFFICE | New external-display photo shows its document editor, controlled by the phone. Full office-suite feature coverage is not established. |
| Blender | Earlier supplied capture shows the 3D viewport. Rendering, plugins and all tools are not thereby qualified. |
| Dolphin / Konsole | Included in the experimental Debian Plasma path; Dolphin appears in the retained desktop capture. |
| Terminal tools | Shell/package-manager functionality is supported; individual programs remain subject to syscall/instruction coverage. |

The maintainer identifies all supplied app captures as physical-device use. Hyprland and FreeCAD captures used GPU acceleration. These facts do not turn separate development-host benchmarks into phone performance measurements.

## Graphics and Execution

Metal requires **Settings → Graphics → Linux GPU Device → GPU → Metal GPU · Experimental** and a full app restart. Check **Active renderer**. Software and accelerated paths support different workloads; Vulkan and arbitrary host graphics APIs are not universally available.

The interpreter remains the default. The selected Build 900 IPA includes optional JIT code, but iOS must grant executable memory before translated blocks can run. Debugger attachment alone is not proof of active JIT; inspect the app's actual backend/status. Physical-device JIT qualification is not established by the available engineering test record.

Linux networking exposes a bounded compatibility view of iOS-provided interfaces and routes. It does not grant radio control, unrestricted raw networking or Bluetooth networking. External output depends on receiver/platform scene support; screen mirroring cannot provide independent phone controls.

[Limitations](KNOWN_LIMITATIONS.md) · [Troubleshooting](TROUBLESHOOTING.md) · [Measured performance](PERFORMANCE.md)
