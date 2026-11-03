# Desktop sizing, presentation and diagnostics

Fit now measures the framebuffer view after immersive navigation/toolbars have
laid out, rather than the entire window. The existing 1600 × 1200 guest cap
still bounds rendering/capture cost; this is separate from an iPad's physical
2732 × 2048 panel resolution. Explicit saved sizes remain available. SteamOS
on iPad migrates the previous Wide default to Fit once per root; subsequent
choices, including Wide, survive. Other distribution defaults remain unchanged.

Metal framebuffer presentation defaults on for SteamOS on iPad when the native
virgl host is active. A saved YES or NO wins. This controls presentation of
received pixels; it is independent of the Linux application renderer and GPU
setting. Native virgl availability alone does not prove that a guest application
or Xorg actually used the GPU. The display-backend marker and guest graphics
logs provide separate evidence; Core Graphics remains the presentation fallback.

Opt-in `LinuxRunnerDesktop.performanceDiagnostics=YES` records bounded private
JSONL samples containing CPU time, footprint, thermal state, frame counts,
viewer/panel geometry, presenter and input diagnostics. These are diagnostic
counters, not panel FPS or causal input latency. Local input diagnostics can
include mouse vendor names; inspect and redact exports before sharing. The
option is off by default. Xorg/graphics log collection belongs to the guest graphics PR (#10).

The UI test helpers preserve UIImage orientation during OCR. Reference tests
require an explicitly named bundle/root; Arch installation requires another
opt-in and refuses an already configured Arch root. The broker operates only
on a new ready session in the named test environment. The tests use synthesized
input; they do not qualify actual Magic Keyboard hardware.

The consolidated desktop PR combines fork PRs #2 (guarded shared capture), #3
(socket paths), #4 (input) and #9 (viewer/diagnostics). Each final fork head
passed external Codex review before merging into the fork. It is based directly
on upstream main and does not contain storage scanning or the SteamOS installer
and guest GPU changes. Historical Arch observations require the full integration;
they are not an independent physical-device qualification of this grouped PR.

Validation: C display-policy checks and Python broker compilation pass. The
extracted branch also builds the Release iOS Simulator app and UI test bundle
with JIT and native virgl disabled. That compile does not exercise device Metal,
physical panel sizing or a real guest desktop.
