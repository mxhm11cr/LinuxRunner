# Desktop controls and diagnostics

The desktop menu contains **Start / Reconnect Desktop**, **Use iPhone as Trackpad**, **Wireless Display**, **Paste Clipboard Text** and **Desktop Settings…**. The trackpad action is always available. Selecting it enables external output and waits for a separate display and the desktop’s first frame, starting or reconnecting the desktop when needed. While waiting, it becomes **Cancel Trackpad Connection**. It changes to **Show Desktop on This Device** while active. A disconnected display clears the mode; plain mirroring does not offer independent phone controls. Opening it dismisses the software keyboard and releases held input modifiers. Installation and sizing controls have moved into settings.

**Settings → Desktop Settings** also opens the controls without starting another desktop. Sections show:

- **Status:** actual session state, distribution, selected Linux user, reported desktop environment, display server, host thermal category and measured app memory where available.
- **Display:** received framebuffer size, Fit/Actual Size/fullscreen, next-start guest resolution, external-output switch and real iOS screen capabilities. Aspect ratio is preserved. Host refresh capability is not measured Linux FPS.
- **Input:** touchpad, pointer and scrolling sensitivity, keyboard settings, and explicit clipboard text input.
- **Desktop Session:** start/reconnect, stop/restart with a save-documents warning, environment selection, component installation and logs.
- **Advanced:** presentation backend, guest graphics preferences and current diagnostic status.

Guest resolution changes apply at the next desktop start. Fit/Actual Size change the viewer immediately. This release does not implement live Xvfb/RandR resolution changes, independent Linux multi-monitor outputs or independent guest outputs for each attached screen. Restart waits for the existing session process to exit before creating another session.

## Logs

Open **Desktop Logs** from settings or the viewer’s logs action. Search by diagnostic message, filter by component/severity, copy filtered records or explicitly export the diagnostic bundle. Refresh after the failure.

Records include bounded native lifecycle/network/display/memory events and filtered recent desktop, Plasma/KWin, D-Bus, X11 and application log tails when present. Missing logs are not invented. Application output retains the existing privacy filter and redaction. A tail’s timestamp is labeled **File updated**, because it is the file modification time; inferred severity is not a causal crash diagnosis. Review an export before sharing it publicly.

## External and wireless displays

The existing external scene and AirPlay workflow remain available; see [networking and displays](NETWORKING_AND_DISPLAYS.md). Enabling output cannot make iOS supply a separate wireless display. Receiver and device behavior varies. The supplied external-display photo demonstrates the phone touchpad workflow; it does not qualify every receiver. Desktop selection now includes experimental Wayland options; see [Compatibility](COMPATIBILITY.md).
