# Firefox in a direct KMS desktop

Date: 2026-11-07. Fix branch: `direct-kms-display`.

## Device evidence

The reported launch used the Wayfire app menu on an iPad 9th generation. The collected session reported:

- `display-transport=direct-kms`.
- Wayfire with the virgl renderer, Mesa 25.0.7 and ANGLE Metal on the Apple A13 GPU.
- A 1080 × 808 desktop.
- Repeated native memory warnings. The logged app footprint reached 1.55 GB.

No current iOS crash or jetsam report was available. The termination cause remains unconfirmed. The session logs did not capture a Firefox rendering error.

## Firefox launch policy

The Wayfire menu launches package desktop entries. It does not use `browser-launch.sh`. That script had Firefox software settings, but they did not protect the menu path.

`firefox-desktop.sh` creates managed desktop entries for installed Firefox packages. Wayfire and Hyprland put that managed directory first in `XDG_DATA_DIRS`. User desktop overrides retain their normal priority. The package files and Firefox profiles remain intact. Desktop actions and URL field codes are kept. D-Bus activation is disabled in the managed entry so it cannot bypass the launch policy. `TryExec` hides an entry when its executable is absent.

The menu entry, native application launcher and browser test launcher use `firefox-launch.sh`. It selects:

- `GDK_BACKEND=x11` and `MOZ_ENABLE_WAYLAND=0`.
- Mesa software rendering with softpipe.
- Mesa Metal interposer pass-through mode.
- The compositor's existing `DISPLAY` and `XAUTHORITY`.

It clears inherited Wayland socket and KMS loader overrides for the Firefox process. Firefox uses Xwayland inside the compositor. Wayfire or Hyprland still renders and presents through direct KMS. This change does not add a VNC transport or disable Wayland for other applications.

GTK documents `GDK_BACKEND=x11` as the X11 backend selection: [GTK runtime environment](https://gnome.pages.gitlab.gnome.org/gtk/gtk3/running.html).

Managed entries refresh at the next desktop start. Close the desktop and start it again after updating the app.

## GPU worker object lifetime

The virgl worker is a long-running POSIX thread. It previously called ANGLE without an autorelease pool around initialization or individual jobs. UIKit's main-loop pool does not drain objects created on that worker.

`lr_host_gpu_job` drains an autorelease pool after GPU initialization and after each GPU job, before the caller resumes. It uses a plain callback on non-Objective-C builds. Apple requires pool management on secondary threads that create autoreleased objects: [Autorelease pool blocks and threads](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/MemoryMgmt/Articles/mmAutoreleasePools.html).

The native regression test uses one worker for 10,000 jobs. Each job creates 32 temporary objects. All 320,000 objects are released before the next job. A control without the per-job pool retains its temporary objects until the outer pool drains. AddressSanitizer and UndefinedBehaviorSanitizer are enabled.

This proves the worker cleanup boundary. It does not measure the device's Metal memory use or prove that this defect caused the reported termination.

## Checks

- Six Firefox launch and menu policy tests, with literal arguments and preserved display authorization.
- Desktop entry parsing with the generated wrapper command.
- Native worker object-lifetime test with sanitizers.
- Desktop configuration and supervision tests.
- Signed Metal-enabled iOS device build.
- GUI support manifest and diff whitespace checks.

Before closing the rendering issue, check Firefox from the Wayfire menu on the iPad. Keep direct KMS enabled. Check visible page content, input and memory use over time. Compare the same workload with direct KMS disabled if the failure continues.
