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

### Diagnostics Tools (Build 902)

Everything below comes from what iOS really reports; nothing is synthesized.
Mac fixture results; physical iPhone results are still required.

| Tool / interface | Status |
|---|---|
| `ping` (ICMP datagram sockets) | Works. `IP_RETOPTS` (Linux's receive-options flag) is accepted and the reply TTL is reported correctly. |
| `tracepath`, `traceroute -U` | Works. `IP_RECVERR`/`MSG_ERRQUEUE` deliver the routers' real ICMP errors (hop address, port-unreachable, frag-needed MTU) through a companion unprivileged ICMP socket; `IP_MTU_DISCOVER` sets the DF bit. |
| `ip route`, `ip route get ADDR` | Works: the host's routing table; `route get` uses the host's own route choice (source address, device, gateway). |
| `ip addr`, `ip link` | Works (host interfaces). |
| `ip neigh` | **Not supported** (`Operation not supported`): iOS gives apps no public way to read the ARP/neighbour table. LinuxRunner does not return a fake empty table. |
| Connections to some localhost ports (seen: 22, 23 on iPhone) | iOS refuses them with `EPERM` before any packet is sent; Nmap reports them filtered. Other ports refuse normally (`ECONNREFUSED`). |
| Error-queue payload | The quoted data is exactly what the ICMP sender included: routers usually quote the probe; Darwin's own loopback errors quote only the UDP header. |
| IPv6 error queue (`IPV6_RECVERR`) | Not implemented yet. |

Bluetooth networking, unrestricted raw packet capture, radio scanning and host-interface/route administration are unsupported. Bluetooth keyboards/mice are a separate platform input feature.

### Diagnose a Connection

Check that the device itself can reach the intended network and that LinuxRunner has any requested local-network permission in system settings. Inspect **Settings → Network**, note the current transport and compare Linux query output. DNS, VPN and captive portal behavior may differ from package-server failures. Preserve the first error and avoid disabling package signature verification.

Interface counters are host-provided counters, not LinuxRunner-only traffic or a throughput benchmark. Address/route output can contain private network information; review it before attaching to an issue. [Troubleshooting](TROUBLESHOOTING.md)
