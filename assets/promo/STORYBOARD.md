# LinuxRunner 1.0.0 — promotional video plan (45–60 s)

No finished video is included: a credible video needs **real screen recordings from a physical iPhone**, which could not
be captured automatically. Everything below uses only real recordings; do not substitute mock-ups.

## Required recordings (iPhone, Control Center › Screen Recording, landscape unless noted)
| # | Recording | Length to capture | Notes |
|---|---|---|---|
| R1 | Tap the LinuxRunner icon → Home appears | 5 s | portrait; Debian active |
| R2 | Home → **Open Desktop** → KDE Plasma appears | 30 s (cut to 6 s) | landscape, Immersive Landscape on |
| R3 | Plasma launcher → Konsole → `uname -a; cat /etc/os-release` | 15 s | type with the on-screen keyboard |
| R4 | Dolphin: browse Home, open a folder | 10 s | |
| R5 | Firefox or Chromium: load a public page, scroll | 15 s | wait for the page before recording |
| R6 | Blender: orbit the default cube (GPU-Resident Metal on) | 10 s | |
| R7 | Settings › Desktop Environment and Keep Screen Awake | 5 s | portrait |

## Storyboard
| Time | Scene | Source | Caption |
|---|---|---|---|
| 0:00–0:04 | LinuxRunner logo on dark background | app icon (`app/Assets.xcassets`) | **LinuxRunner** |
| 0:04–0:09 | App launch → Home | R1 | Linux apps on your iPhone |
| 0:09–0:15 | Debian badge on Home, Open Desktop | R1/R2 | Real Debian ARM64 — no virtual machine |
| 0:15–0:23 | KDE Plasma appears | R2 | Full desktop environments |
| 0:23–0:31 | Konsole command | R3 | A real Linux terminal |
| 0:31–0:37 | Dolphin | R4 | Files and folders |
| 0:37–0:45 | Browser | R5 | Desktop browsers |
| 0:45–0:52 | Blender | R6 | Even 3D apps |
| 0:52–0:58 | Closing card | — | **LinuxRunner 1.0.0** · open source · ARM64 Linux on iOS |

Speed-ups must be disclosed: add a small "Sped up ×N" label to any clip that is accelerated (startup is slow on an
interpreter; do not imply real-time speed).

## Transitions and style
Hard cuts or 6-frame cross-dissolves; captions bottom-centre, SF Pro Semibold, white on 40 % black; no background
music with lyrics; keep the iOS status bar (proves it is an iPhone) but crop personal notifications.

## Export
- Master: 1920×1080 (or 1080×1920 for vertical social cuts), 30 fps, H.264 High, 12–16 Mbit/s, AAC 48 kHz.
- GitHub README: a 10–15 s silent MP4 or GIF under 10 MB (e.g. R2 → Plasma), 1280 px wide.
