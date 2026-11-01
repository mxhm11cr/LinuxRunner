# Third-party notices

LinuxRunner derives from iSH (Theodore Dubois and contributors) through ios-linuxkit (Rui Carmo and contributors). Preserve [LICENSE.md](LICENSE.md), [LICENSE.IOS](LICENSE.IOS), the [full GPL texts](licenses/), all source copyright notices and the in-app Notices & Credits. The combined application is GPL-3.0-covered; the upstream contribution-specific GPL-2.0 terms are retained without extending them to every LinuxRunner addition.

## Components in the application

| Component | Retained source / notice | Terms |
| --- | --- | --- |
| libarchive | `deps/libarchive`, pinned revision in `SOURCE_ORIGIN.txt` | BSD-style; per-file terms in COPYING |
| glslang | `deps/glslang` and `GUISupport.bundle/shader-compiler-notices.txt` | Multiple licenses, including BSD and Apache-2.0; preserve complete LICENSE.txt |
| SPIRV-Cross | `deps/spirv-cross` and shader-compiler-notices.txt | Apache-2.0; retain notices |
| hterm / libapps | `deps/libapps` | BSD-3-Clause and component-specific terms |
| xterm.js and addons | `app/terminal/xterm-vendor/LICENSES.md`, VENDORED.txt | MIT; package versions recorded with the bundle |
| Ghostty Web | `app/terminal/ghostty-web-VENDORED.txt`, terminal-notices.txt | MIT; vendor record describes local WKWebView patches |
| Fira Code and JetBrains Mono Nerd Fonts | Font VENDORED.txt files and terminal-notices.txt | SIL OFL-1.1 and relevant Nerd Fonts notices |
| LZMA API headers | `deps/lzma-api/COPYING`, provenance.json | Header-specific public-domain notice; system liblzma linked separately |
| SQLite and Apple system libraries | Platform libraries | Their respective upstream/platform terms |
| Mesa demo sources | `tests/linuxrunner/gpu/mesa-demos`, checksums and per-file notices | MIT-style notices retained in source and mesa-demos-notice.txt |
| LinuxRunner guest graphics and helper binaries | `graphics/`, GUI bundle sources and build scripts | Part of the GPL-covered derivative; gpu-bridge-notice.txt and gpu-glx-notice.txt |

The app bundles `gpu-glx-notice.txt`, `gpu-bridge-notice.txt`, `mesa-demos-notice.txt`, `shader-compiler-notices.txt`, `terminal-notices.txt` and xterm's `LICENSES.md`. Retain these notices in the source and packaged application.

The custom LinuxRunner wallpaper and active icon have retained generation scripts and original SVG geometry. Inherited alternate iSH icons retain their original files. KDE wallpaper and application artwork visible in screenshots belong to their respective projects.

## GPU-enabled Build 900

The Metal-capable application links **virglrenderer** (MIT/Mesa-derived), **libepoxy** (MIT and retained Khronos terms) and **ANGLE** (BSD-style and component-specific terms). ANGLE's linked source inputs also include Abseil, astc-encoder, Dawn, Perfetto, zlib, LLVM libc++/libc++abi and Volk. Their terms and source copyrights are retained; none is presented as original LinuxRunner code.

[GPU dependency revisions and notice origins](licenses/gpu-dependencies.json) records the exact checkouts. [GPU third-party notices](licenses/GPU-THIRD-PARTY-NOTICES.txt) conservatively reproduces the checked-out source trees' license/notice texts, including nested components. It is broader than a list of linked binaries. Preserve applicable upstream per-file notices in corresponding source as well as this binary-distribution notice material.

The guest Mesa packages used by installed distributions retain their own terms. The runtime's virtual GPU integration and selective earlier graphics helpers are distinct paths. The source distribution must include matching graphics integration source, patches, dependency sources and build instructions; automatic GitHub source archives alone omit externally checked-out dependencies.

## Bundled Alpine minirootfs

The IPA contains Alpine 3.24.2 aarch64, root archive SHA-256 `9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773`. Packages retain their individual licenses. The APK database is the authoritative package/version inventory; the release review records it separately.

GPL-covered package groups include BusyBox, apk-tools, alpine-baselayout and scanelf; musl-utils contains mixed-license parts. Other components include musl (MIT), OpenSSL (Apache-2.0), zlib (Zlib), Alpine keys/release metadata (MIT), and certificate data (MPL-2.0 / MIT). This root archive is an aggregation, not a relicensing of Alpine packages under the application license.

Bundled Alpine components retain their own source-distribution requirements. Their package versions and license fields are recorded in the root filesystem APK database.

The rootfs patch bundle contains `wasm-polyfill.js` and `fetch-polyfill.js`. The former describes an llhttp parser reimplementation. The upstream llhttp MIT notice is retained in `licenses/llhttp-MIT.txt` as parser-interface attribution. LinuxRunner's polyfill sources remain in the rootfs patch bundle.

## Downloaded Linux software

Debian and Arch Linux ARM packages and additional Alpine packages are downloaded from distribution mirrors by the user's device. They keep their own licenses and notices (for Debian, see `/usr/share/doc/<package>/copyright`). KDE, Mesa, browsers and other installed programs are separate projects.

## Source availability

The application source, build scripts and dependency revisions are retained in the repository. [SOURCE_ORIGIN.txt](SOURCE_ORIGIN.txt) records the release lineage and exact dependency commits. Submodule contents are not included in GitHub's automatic source archives; initialize the pinned submodules when building from a clone. Source-distribution requirements for bundled binaries apply separately from the licenses of packages downloaded later by users. The release source asset must include the matching application snapshot, pinned build dependencies, graphics source/patches, matching Alpine recipes and checksum-verified inputs, and matching Ghostty Web/Ghostty source. The unchanged bundled Alpine root archive can use the already retained matching source inputs. Preserve the complete notices asset alongside the IPA.
