# ANGLE/virgl sampler compatibility

The host renderer starts every embedder job on virglrenderer's host context.
Context creation and global resource operations can otherwise change GL
bindings outside virglrenderer's guest-context cache, leaving subsequent
submissions with the wrong framebuffer bindings.

Desktop samplers also need transparent-zero borders. A private bridge validates
the Metal backend, sampler, wrap axis and all-zero border before mapping to
Metal's clamp-to-zero. Arbitrary border colors remain unsupported; the complete
GLES border-color extension is not advertised. The renderer patch is applied to
a copy of pinned virglrenderer. ANGLE replacement objects are built from copied
preferred source; upstream trees remain unchanged. Preserve both patches,
build scripts, pinned sources and their licenses in source distributions.

The build supports static macOS qualification as well as iOS, applies the
libepoxy process-image patch on both, and disables the unused WebGPU backend.
No prebuilt dependency libraries are added by this PR.

On Apple Silicon macOS, the extracted current `graphics/virgl_host.c` compiled
and passed `tests/linuxrunner/gpu/run-virgl-sampler-probe.sh`, linked against
retained pinned ANGLE/epoxy/virglrenderer libraries built with these patches.
Repeat, edge, zero-border and mirror-edge commands pass; nonzero border returns
EINVAL as expected. This run reused dependency libraries; it was not a clean
rebuild of all GPU dependencies, nor a physical iOS or full-desktop test.

Rebuild dependencies using the pinned source instructions in BUILDING.md, then
run the probe. `virgl_border_contract.c` is the separate guest pixel contract
for inside/outside and edge sampling, built against a matching EGL/GLES/GBM
sysroot. That guest contract is retained but was not rerun for this PR extraction.
Sampler acceptance alone does not qualify Xorg/Plasma or prove correct rendering
for every workload.

The consolidated upstream graphics PR combines the final reviewed fork PRs #6
and #7. It is based directly on upstream main; it contains no guest Mesa overlay
or SteamOS installer. Host contracts distinguish the virtual 60 Hz clock from
physical panel synchronization and command acceptance from rendered pixels.
