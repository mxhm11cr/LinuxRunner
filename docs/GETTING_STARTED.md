# Getting Started — Installing Linux in LinuxRunner

The IPA installs LinuxRunner. A distribution provides the Linux files, shell and packages inside it. You can keep several environments, but only one runs per application launch.

## 1. Install LinuxRunner

Download the IPA from [GitHub Releases](https://github.com/mxhm11cr/LinuxRunner/releases/latest) and sign/install it with a compatible sideloading tool of your choice. See [Installation](INSTALLATION.md) for checksum and signing details. Installing the app does not require compiling its source.

For an update, back up important files and preserve your bundle identifier and App Group configuration. Do not uninstall the existing app as a troubleshooting step.

## 2. Open LinuxRunner

The app has **Home**, **Distributions**, **Sessions** and **Settings** tabs. Home shows the active runtime, desktop status and workspace actions. A setup assistant may appear automatically when no environment exists. Otherwise, open it yourself in the next step.

## 3. Open Distributions

Select **Distributions**. Existing installations appear under **Installed**. Their files and package databases are separate.

To remove an installation, open its details and choose **Delete Distribution…**, or swipe left on its
row and choose **Delete**. A confirmation lists what will be removed. Export important Linux home
and exchange files first. Files in **On My iPhone/iPad → LinuxRunner → Shared**, linked external folders
and other installations are kept.

A mounted distribution cannot be deleted, even after its shell exits. Select another installation with
**Use on Next Launch**, fully close LinuxRunner and reopen it, then delete the old installation. Deletion
is also unavailable during distribution installation. This follows the existing relaunch requirement;
it does not stop an active desktop or unmount a running filesystem.

To use a different installed environment, open it and choose **Use on Next Launch**, then fully close LinuxRunner from the app switcher and reopen it. This switches the active root filesystem; it does not reinstall Linux.

## 4. Choose a Distribution

Under **Add a Distribution**, tap **Install** on the distribution you want. This is the unified installation/configuration workflow. An environment's detail screen also has the setup assistant for configuring that existing installation.

## 5. Choose a Linux Distribution

On **Choose Linux**, select:

| Choice in the app | Choose it for |
| --- | --- |
| **Debian GNU/Linux 13 (ARM64) — recommended** | Recommended `apt` environment; LinuxRunner Desktop, LXQt and experimental KDE Plasma/Wayfire. |
| **Arch Linux ARM (ARM64) — experimental** | Rolling `pacman` environment; the most desktop choices, including experimental Hyprland. The app still labels this distribution experimental. |
| **Fedora Linux 44 (ARM64)** | Official Fedora userspace with DNF and normal Linux accounts. Lightweight desktop and KDE Plasma. Sound is not yet qualified. |
| **Kali Linux 2026.3 (ARM64)** | Official Kali root filesystem with Xfce and optional tool sets, for legitimate development, diagnostics and authorized security research. |
| **SteamOS ARM64 · Experimental** | Installed from Valve's recovery image; terminal and Plasma X11. See [SteamOS ARM64](STEAMOS.md). |
| **Alpine Linux 3.24 (ARM64)** | A small, mainly terminal-oriented environment using `apk`. |

If the selected distribution is already installed, **Already Installed** offers **Configure <environment name>**. Use that to retain its files and accounts. Choose **Install Another Environment** only when you actually want a separate installation. Otherwise, choose **Continue**.

## 6. Review the Installation Options

The **Install Linux** screen shows download information, available storage and an installed-size estimate. Arch displays a base-installation free-space requirement; desktops and applications need additional space.

This wizard does **not** ask for a custom destination folder, partition, mirror or desktop choice on this screen. LinuxRunner manages the environment's storage. Account creation and desktop preparation follow installation.

Keep the app in the foreground during long operations. **Settings → General → Keep Screen Awake During Operations** prevents idle locking; it does not keep the app running in the background.

## 7. Install the Distribution

Use the button matching your choice: **Install Debian**, **Install Arch Linux ARM**, **Install Fedora Linux** (development build) or **Install Alpine**.

Debian and Arch download official packages and verify the signed package data. Arch retains verified downloads for retry. Fedora downloads the official Fedora 44 AArch64 WSL userspace image, checks its pinned digest authenticated from Fedora's signed checksum, then imports an isolated root. LinuxRunner runs it without WSL, systemd or a Linux kernel. Fedora needs at least 2 GB free for its base installation, plus desktop/application space. First start validates RPM/DNF and installs the required signed tools; keep networking available. Alpine unpacks the root filesystem included in the app. Follow the progress text; percentages are shown when the operation can measure them. Download and installation times depend on your connection and device.

Debian, Arch and Fedora offer **Cancel** during downloading and **Try Again** after a reported failure. Wait for cancellation to finish before retrying. Read the error instead of starting another installation over an existing environment. [Installation recovery](TROUBLESHOOTING.md#distribution-installation-fails-or-stops)

## 8. Complete Initial Setup and Create an Account

If **Restart LinuxRunner** appears, swipe the app away in the app switcher and open it again. Setup resumes for the selected environment. First-start package configuration and account-tool installation may take additional time; keep the app open.

On **Your Account**, either select an existing regular account or enter **Username**, **Password**, **Confirm password**, then choose **Create Account**. The interface accepts lowercase letters, digits, `-` and `_` for account names. **Administrator (can use sudo)** grants administrative use through `sudo`.

`root` is Linux's administrative account. A regular user is preferable for everyday applications; some graphical programs refuse to run as root. LinuxRunner stores a Linux password hash, not the entered password. Linux accounts are useful compatibility features, not a security boundary between programs in the app.

## 9. Open the Linux Terminal

For a terminal-only workflow, use **Later** when setup is idle to leave the assistant. You can return through **Distributions → <environment> → Set Up Linux…**. On Home, choose **Terminal**, or open the active environment and choose **Open Terminal**. If no runtime is running, Home offers **Start Linux Runtime**.

Try these read-only commands:

**Debian**

```sh
cat /etc/os-release
uname -m
```

**Arch Linux ARM**

```sh
cat /etc/os-release
pacman -Q
```

**Fedora (development build)**

```sh
cat /etc/os-release
rpm -q bash
```

**Alpine**

```sh
cat /etc/os-release
apk info
```

The architecture is normally reported as `aarch64`. Linux version/CPU information is a compatibility view; it does not mean the app booted a Linux kernel. Use `id` and `pwd` to check your selected account and working directory.

## 10. Install Desktop Components

A desktop is optional for terminal use. In the assistant's **Desktop** stage, choose **Install Desktop Components**. This adds the display server, window manager, panel, file manager and available sound support using the distribution's packages. Account tools and desktop packages may need network access even with bundled Alpine.

The setup desktop uses lightweight Openbox components (LXQt Panel/PCManFM-Qt on Arch). For another desktop, open **Settings → Desktop Environment**, use **Install LXQt**, **Install Enlightenment**, **Install Wayfire**, **Install Hyprland** or **Install KDE Plasma** where available, then select it in the **Desktop** picker under **Open Desktop Starts**. Stop an existing desktop before switching. Hyprland is experimental, Arch-only and requires at least 8 GB RAM; Metal is recommended. Enlightenment is disabled on Debian and Arch Plasma remains unvalidated.

When components are installed, choose **Continue**. **Home → Open Desktop** can also install missing desktop support automatically. Alpine's desktop path is experimental; use its terminal if graphical packages fail.

## 11. Launch and Use the Desktop

Choose **Open Desktop** at the end of setup or on Home. A running session may instead show **Resume Desktop**. The viewer displays the Linux desktop and forwards input.

| Control | Action |
| --- | --- |
| Tap | Left click in touch mode. |
| Long press | Right click by default; the input controls can select two-finger right click. |
| Pinch | Zoom the desktop view. |
| Two-finger movement | Pan while zoomed; scroll when fitted. |
| Keyboard button and modifier bar | Show the on-screen keyboard; send Esc, Tab, Shift, Ctrl, Alt and Super. |
| Hardware keyboard / supported mouse or trackpad | Forward the input delivered by iOS/iPadOS. Behavior depends on the accessory and platform settings. |
| **Desktop → Desktop Settings… → Display → Fit** / **Actual Size** | Change viewer scaling. |
| **Desktop Settings… → Display → Resolution & Viewer Preferences** | Select a window-sized Linux resolution for the next session start. |
| **Done** | Return to LinuxRunner while retaining the current desktop session. |
| **Sessions → Resume Desktop** | Reattach to a live session. |
| **Stop Desktop** | End the desktop and its applications; save documents first. |

Use **Desktop → Paste Clipboard Text**, including on iPad. It types clipboard text into the focused Linux application. Check the text and focus before using it.

On a hardware keyboard, **Control–Option–Command–M** opens Desktop Options, **–F** fits the view, **–0** uses actual size and **–W** closes the view. Other forwarded Command combinations use the Linux Super modifier. On iPad, hold Command to see available app shortcuts.

The larger iPad window can show more of a desktop. Four orientations, window-aware initial sizing, keyboard shortcuts and pointer handling are implemented; the maintainer has tested iPad support on a physical device. Changing viewer scale does not resize already-running Linux windows to a new display resolution.

Closing the viewer is different from stopping the desktop. iOS/iPadOS may suspend the app, and termination ends Linux processes even though saved files remain.

## 12. Install and Launch Applications

Use **Home → App Manager**, or **Distributions → <active environment> → Install, Manage and Repair Applications**. Search, inspect package details and review the proposed transaction. The App Catalog is a convenience list, not a guarantee that every listed application works in every distribution.

For a small command-line example, install `nano`. These commands change packages and require an administrator account; review each package manager's confirmation:

**Debian**

```sh
sudo apt update
sudo apt install nano
```

**Arch Linux ARM**

```sh
sudo pacman -Syu --needed nano
```

Arch is a rolling distribution: use a consistent full-system upgrade when adding packages, rather than synchronizing databases and installing against an old system. Back up important files before upgrades.

**Fedora (development build)**

```sh
sudo dnf install hello
hello
```

Fedora's Package Center supports package inventory, search, details and ordinary DNF operations. Retain signature verification. Fedora sound is not yet qualified.

**Alpine**

```sh
sudo apk update
sudo apk add nano
```

Run `nano` in the terminal. For installed graphical applications, use **Home → Applications** or the environment's discovered application list. Managed launchers supply the active display and authorization. If starting a GUI program from a terminal, open a fresh terminal after the desktop starts; an older shell cannot inherit a new display environment automatically.

Application compatibility varies by distribution, package version and workload. See [Compatibility](COMPATIBILITY.md) and [Troubleshooting](TROUBLESHOOTING.md). Never disable package signature verification to make an installation proceed.

## 13. Select Graphics, External Output or Optional JIT

For the accelerated path, use **Settings → Graphics → Linux GPU Device → GPU → Metal GPU · Experimental**. Fully close/reopen the app, then check **Active renderer**. Automatic starts the virtual GPU with Linux and falls back to software rendering if it cannot start. See [graphics and performance](PERFORMANCE.md).

For an external display, enable **Settings → Devices → External Desktop Output**, connect a compatible wired/AirPlay display, open the desktop and choose **Desktop → Use iPhone as Trackpad**. Independent controls require a separate iOS external scene. [Display guide](NETWORKING_AND_DISPLAYS.md)

JIT is optional. The interpreter works without activation. Enable **Settings → Advanced → Developer Mode** to inspect **Execution Engine / StikDebug**, actual status and activation controls. Availability depends on signing and OS restrictions; a compiled JIT does not imply permission to execute it.
