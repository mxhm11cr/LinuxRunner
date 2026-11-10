# SteamOS ARM64 in LinuxRunner

SteamOS ARM64 is an **experimental** LinuxRunner environment built from Valve's published SteamFrame recovery image. It is not official SteamOS support and is not endorsed by Valve.

## What you get

A terminal, `pacman` package inspection and a KDE Plasma X11 desktop. Package installation and repository updates are not enabled for this environment.

The Steam client, gamescope, Vulkan and Windows games are **not supported**.

## Installing

Open **Distributions → Add a Distribution → SteamOS ARM64 → Install**, then choose **Install SteamOS**. LinuxRunner then:

1. Downloads the SteamFrame recovery image (4.07 GB) from Valve's server. Interrupted downloads resume.
2. Checks the image against a SHA-256 pinned in the app.
3. Downloads x11vnc and libvncserver from Arch Linux ARM and checks their pinned SHA-256 values.
4. Unpacks only the system partition and reads its Btrfs filesystem directly. Nothing from the image is executed during installation.
5. Installs the result as a new environment and deletes the download.

Allow about **19.4 GB of free space during installation**; about **8.9 GB** stays in use afterwards. Keep LinuxRunner open while it installs. Restart when asked, create a Linux account, then choose **Check SteamOS Desktop**.

Kernels, firmware, boot data and debug symbols are left out because LinuxRunner does not boot a Linux kernel.

## Notes

Plasma takes a few minutes to finish loading on the interpreter, and its wallpaper and panel appear late. A background update checker (`DiscoverNotifier`) has been seen to crash without affecting the desktop.
