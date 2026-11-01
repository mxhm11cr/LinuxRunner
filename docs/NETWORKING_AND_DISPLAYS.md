# Networking and External Displays

## DeX Mode — External Desktop

LinuxRunner can present its Linux desktop on a compatible external screen and use the phone as a control surface. This is LinuxRunner's runtime and iOS display integration, not Android or Samsung DeX.

1. Connect a display using a cable/adapter supported by the device. For wireless output, connect both ends to the same Wi-Fi and use **iOS Control Center → Screen Mirroring** to choose an AirPlay receiver; enter any receiver passcode in iOS.
2. Enable **Settings → Devices → External Desktop Output**. The page includes **Connect Wirelessly with AirPlay…** under **External Monitor · USB-C or AirPlay**.
3. Use **Home → Open Desktop** or **Resume Desktop**.
4. Open **Desktop → Use iPhone as Trackpad** in the viewer. The action enables external output and waits for a separate scene and a desktop frame. It can start/reconnect the session when needed.
5. While waiting, choose **Cancel Trackpad Connection** to cancel. While active, **Show Desktop on This Device** returns the desktop to the phone's viewer.

The trackpad supports slide-to-move, tap-to-click, hold-and-slide dragging, two-finger scrolling and two-finger right click. The on-screen keyboard/modifier controls remain available. Supported hardware keyboards, mice and trackpads use the input supplied by iOS/iPadOS. Sensitivity controls are under **Desktop Settings… → Input**.

### Output Behavior and Limits

A separate external scene is required for the display to show the desktop while the phone shows independent controls. If iOS supplies plain mirroring, both screens show the same view. Receiver compatibility, latency and resolution depend on the platform and connection; Miracast-only receivers are unsupported. The supplied external-display photo demonstrates the workflow, not qualification of every receiver.

**Desktop → Desktop Settings… → Display** provides Fit, Actual Size, Full Screen, external-output status and **Resolution & Viewer Preferences**. Scaling changes immediately; guest resolution changes apply on the next desktop start. Save work before restarting. The default is landscape 960 × 540. Reported iOS screen modes/maximum refresh are host capabilities, not measured Linux FPS. LinuxRunner displays one Linux output rather than an extended multi-monitor guest desktop.

**Done** detaches the viewer while retaining the session. iOS suspension/termination still applies. Reconnect rather than creating another desktop if the original is alive. [Desktop controls](DESKTOP_CONTROLS.md)

## Linux Networking

LinuxRunner translates Linux networking calls onto host APIs. Build 900 improves host-derived address/interface visibility and routing information, including IPv4/IPv6 netlink/proc views. Inspect the native view at **Settings → Network** and compare from a Linux terminal:

```sh
ip addr
ip link
ip route
```

Routes reflect available host routing information, not a separately controlled Linux network namespace. VPNs and platform policy can affect what is visible. Available host-compatible interfaces can carry connections through iOS, without granting Linux ownership of Ethernet/USB/radio hardware.

Bluetooth networking, unrestricted raw packet capture, radio scanning and host-interface/route administration are unsupported. Bluetooth keyboards/mice are a separate platform input feature.

### Diagnose a Connection

Check that the device itself can reach the intended network and that LinuxRunner has any requested local-network permission in system settings. Inspect **Settings → Network**, note the current transport and compare Linux query output. DNS, VPN and captive portal behavior may differ from package-server failures. Preserve the first error and avoid disabling package signature verification.

Interface counters are host-provided counters, not LinuxRunner-only traffic or a throughput benchmark. Address/route output can contain private network information; review it before attaching to an issue. [Troubleshooting](TROUBLESHOOTING.md)
