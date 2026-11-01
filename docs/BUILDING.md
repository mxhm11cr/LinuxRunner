# Building LinuxRunner

Most users install the IPA. These instructions build the **LinuxRunner** scheme in `iSH.xcodeproj`, preserving version **1.1.0**, build **900** and bundle identifier `dev.linuxrunner.app`.

## Tool Requirements

Use macOS with full Xcode and an iPhoneOS SDK, Git, Python 3, Meson, Ninja, CMake, pkg-config, `ld.lld` and system archive/signing tools on PATH. The retained Build 900 archive records iPhoneOS SDK **27.2**, ARM64 and minimum OS **15.0**. The deployment target is not broad device/older-OS qualification. The GPU dependency build additionally uses GN/depot_tools, rsync and ANGLE's downloaded toolchain inputs.

```sh
git clone https://github.com/mxhm11cr/LinuxRunner.git
cd LinuxRunner
git submodule update --init --recursive --checkout -- deps/libapps deps/libarchive deps/glslang deps/spirv-cross
scripts/bootstrap-macos.sh
```

Once published, select the approved `v1.1.0` revision when reproducing this release. Until then the public repository may still contain the previous version; do not replace a newer local checkout with it. The inherited `deps/linux` gitlink is optional and not used to boot a kernel in the app.

## Interpreter / Software Variant

The source defaults have `LINUXRUNNER_JIT=NO` and `LINUXRUNNER_VIRGL=NO`:

```sh
scripts/build-ipa.sh --unsigned
python3 scripts/verify-ipa.py build/releases
```

This builds a functional interpreter/software variant. It does not reproduce the Metal/JIT-capable Build 900 release binary.

## Metal / Optional-JIT Variant

The selected release IPA includes both compile-time capabilities. Runtime JIT permission remains optional and is checked separately.

Create a graphics dependency directory outside the repository's source files, and set `GPU_DEPS` to its absolute path. The directory must contain these repositories at the recorded revisions:

| Directory | Upstream | Revision |
| --- | --- | --- |
| `virglrenderer` | `https://gitlab.freedesktop.org/virgl/virglrenderer.git` | `e0a1caf3b147bb26dfe524dbb85ded1177e7e49e` |
| `libepoxy` | `https://github.com/anholt/libepoxy.git` | `d1f952c4565c389d82fd1657153e84e050f35b27` |
| `angle` | `https://chromium.googlesource.com/angle/angle` | `feebf60fe088ccec3a26a7419d6ac68edf542c7a` |
| `depot_tools` | `https://chromium.googlesource.com/chromium/tools/depot_tools.git` | `a20039648c0de69d8156906b18052bd47e9bf230` |

For fresh checkouts, prepare the dependencies with the recorded revisions. These commands download substantial additional build inputs:

```sh
mkdir -p ../linuxrunner-gpu-deps
export GPU_DEPS="$(cd ../linuxrunner-gpu-deps && pwd)"
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$GPU_DEPS/depot_tools"
git -C "$GPU_DEPS/depot_tools" checkout --detach a20039648c0de69d8156906b18052bd47e9bf230
export PATH="$GPU_DEPS/depot_tools:$PATH"
git clone https://gitlab.freedesktop.org/virgl/virglrenderer.git "$GPU_DEPS/virglrenderer"
git -C "$GPU_DEPS/virglrenderer" checkout --detach e0a1caf3b147bb26dfe524dbb85ded1177e7e49e
git clone https://github.com/anholt/libepoxy.git "$GPU_DEPS/libepoxy"
git -C "$GPU_DEPS/libepoxy" checkout --detach d1f952c4565c389d82fd1657153e84e050f35b27
git clone https://chromium.googlesource.com/angle/angle "$GPU_DEPS/angle"
git -C "$GPU_DEPS/angle" checkout --detach feebf60fe088ccec3a26a7419d6ac68edf542c7a
(cd "$GPU_DEPS/angle" && python3 scripts/bootstrap.py && gclient sync)
```

The bootstrap creates ANGLE's gclient configuration; sync resolves the pinned checkout's DEPS and tools. Follow the retained ANGLE `doc/DevSetup.md` for platform prerequisites. Do not substitute current dependency revisions.

After preparing those source checkouts, from the LinuxRunner repository root:

```sh
scripts/build-gpu-deps.sh iphoneos
LINUXRUNNER_JIT=YES LINUXRUNNER_VIRGL=YES scripts/build-ipa.sh --unsigned
LINUXRUNNER_JIT=YES python3 scripts/verify-ipa.py build/releases
```

`GPU_DEPS` must already be exported. The script checks the recorded virglrenderer/ANGLE revisions but records rather than enforces libepoxy's current HEAD; explicitly check out the table's libepoxy revision. It applies the retained iOS process-image patch to a copy. Output includes `build/gpu/iphoneos/REVISIONS` and static libraries/headers.

This preparation re-audited the retained archive; it did not perform a clean rebuild of all graphics dependencies. Clean-checkout qualification of the additional dependency/source bundle remains a release review item. The app's linker must keep dynamically resolved EGL exports: the verified IPA retains 44 EGL exports, including `eglGetProcAddress` and `eglGetCurrentDisplay`.

## Packaging and Signing

Default output is `build/releases/LinuxRunner.ipa` with the retained archive, symbols, checksum and verification reports. `RELEASE_DIR` and `ARTIFACT_NAME` choose other destinations/names; use the same artifact name when verifying. For the retained release filename use `ARTIFACT_NAME=LinuxRunner-1.1.0-build900`.

Unsigned mode adds an ad-hoc signature without Apple provisioning. A compatible installation method must provide device authorization. For your own signing, configure an Apple team in Xcode and use `TEAM_ID=YOUR_TEAM_ID scripts/build-ipa.sh`, adding the variant variables above if desired. Never commit certificates, keys or provisioning profiles. Optional App Group access uses `group.dev.linuxrunner.app`; a private-container fallback exists.

Alpine 3.24.2 is checksum-pinned. `ROOTFS_CACHE` can provide the cached archive; validation still applies. `verify-ipa.py` checks package identity, rootfs hash, 90 GUI resources, source fingerprint, device Mach-O dependencies, IPA/archive identity and matching symbols. Set `LINUXRUNNER_VERIFY_SOURCE_TREE` and `LINUXRUNNER_EXPECTED_SOURCE_COMMIT` when checking a retained artifact against its source.

## Guest Helpers and Runtime Tests

Guest helpers in `GUISupport.bundle` are not all rebuilt by an ordinary app build. Their sources/recipes are mapped in [bundled-guest-source-map.json](../licenses/bundled-guest-source-map.json). Preserve matching source and notices for the bundled rootfs, terminal renderer and graphics dependencies.

```sh
meson setup build/host -Dengine=asbestos -Dguest_arch=arm64 -Djit=false -Djit_emit=false -Dcli_aot=
ninja -C build/host ish
python3 scripts/generate-gui-support-manifest.py --check
```

Use disposable fixtures for mutating tests. [Architecture](ARCHITECTURE.md) · [Third-party notices](../THIRD_PARTY_NOTICES.md)
