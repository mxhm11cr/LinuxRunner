# Graphics and Runtime Performance

## Enable the Metal GPU

Open **Settings → Graphics → Linux GPU Device → GPU → Metal GPU · Experimental**. Fully close LinuxRunner from the app switcher and reopen it, then check **Active renderer** and its reason on the same page. Build 900's **Automatic** GPU selection uses software. **Software Rendering** remains available for compatibility.

This virtual GPU path uses **guest Mesa/virgl → virglrenderer → ANGLE → Metal**. The separate **Linux Applications → Renderer** picker and a Metal framebuffer presenter are different mechanisms; neither alone confirms this GPU device is active. Even an active device does not guarantee every application/API uses it.

## Physical iPhone — Maintainer Report

The maintainer reports this result from physical-device Hyprland testing:

| Item | Reported value |
| --- | --- |
| Hardware | iPhone 16 Pro |
| OS | iOS 27.2 |
| Software rendering | Approximately 10 FPS |
| Metal rendering | Approximately 55–60 FPS |
| Test scope | Hyprland; detailed scene, distribution/package versions and duration not supplied |
| Measurement method | Counter/instrument not specified; informal observation supplied by the maintainer |

Reported on 2026-11-01. The maintainer reports that other desktops can achieve higher frame rates; no additional desktop-specific figures were supplied. No raw trace or paired-run dataset was supplied. Treat this as an owner-reported physical-device result, not an independently reproduced controlled benchmark. The owner separately confirms both the Hyprland and FreeCAD captures used the GPU. Do not substitute the development-host method below for this phone observation.

## Development-Host Hyprland Measurement

**Hardware/environment:** Apple M1 development host, macOS 27, LinuxRunner ARM64 runtime; Arch Linux ARM with Hyprland 0.57. Recorded release-candidate source: `4c1e31d5`.

**Workload:** continuously scrolling foot terminal, 960 × 540 output. **Method:** virtual-KMS page-flip count divided by 60 seconds, two alternating software/accelerated rounds on the same host/runtime. This measures compositor output updates, not a game's FPS or final phone/receiver scanout.

| Renderer | Round 1 | Round 2 | Mean |
| --- | --- | --- | --- |
| llvmpipe / software KMS | 4.1 FPS | 4.0 FPS | 4.05 FPS |
| Mesa virgl / virglrenderer / ANGLE / Metal | 55.6 FPS | 55.3 FPS | 55.45 FPS |

The ratio of means is **13.7×** for this workload. **These are not physical-iPhone measurements.** The owner confirms the supplied Hyprland and FreeCAD images used GPU acceleration; the images are separate from the benchmark. Hardware, desktop, graphics API, workload and presentation path affect real performance.

The earlier 4.5 → 49.9 FPS development-host result used an earlier candidate; it is superseded here by the later alternating measurement. It was not an iPhone result either.

## Interpreter and Optional JIT

Measured on **2026-11-01**, on the same Apple M1/macOS 27 host and Debian 13/LXQt filesystem fixture, comparing:

- Build 899 source `a1632e5a`, interpreter, Meson debugoptimized with JIT disabled.
- The current release-cycle interpreter, debugoptimized, JIT disabled.
- The same release-cycle runtime with JIT enabled (JIT code version 14).

The engineering record names the latter two as current main rather than recording an exact hash. Measurements preceded the final vector-RBIT and proc-stat-lock fixes; those fixes did not touch the measured paths. This provenance limits exact reproduction of the recorded comparison.

**Method:** three rounds, runtime order alternating within each round; medians and min–max below. Microbenchmarks report **ns/iteration**; macro workloads (`python_start` onward) report **wall-clock ms**. Output checksums were identical. Recorded correctness gates (`jit_fault_registers_contract`, `precise_fault_pc_contract`, `jit_smoke`) passed on each compared runtime. These are existing engineering results, not tests rerun during documentation preparation.

The microbenchmark source is retained at [interp_bench.c](../tests/linuxrunner/bench/interp_bench.c). The original orchestration scripts and fixture configuration are not a self-contained public benchmark kit; do not treat this table as a one-command reproducibility claim. Ratios are Build 899 time / candidate time; above 1 means faster.

| workload | rel 899 median (min–max) | current interpreter | current JIT | interpreter vs 899 | JIT vs 899 | outputs |
|---|---|---|---|---|---|---|
| alu | 13.81 (11.42–14) | 11.53 (11.41–14.77) | 2.56 (2.56–3.03) | 1.20× | 5.39× | identical |
| branch | 17.82 (14.66–18.32) | 16.08 (14.49–26.06) | 4.26 (4.24–5.32) | 1.11× | 4.18× | identical |
| mem_seq | 230075 (200050–245550) | 212350 (187875–325625) | 176100 (175475–186325) | 1.08× | 1.31× | identical |
| mem_rand | 84.5 (72.99–114.4) | 77.37 (73.83–87.83) | 73.69 (72.75–128.1) | 1.09× | 1.15× | identical |
| call | 40.25 (39.16–58.96) | 39.4 (39.09–39.94) | 9.52 (9.17–10.61) | 1.02× | 4.23× | identical |
| indirect | 30.06 (27.44–42.53) | 27.41 (27.35–27.54) | 4.96 (4.79–5.31) | 1.10× | 6.06× | identical |
| float | 14.01 (12.13–21.39) | 12.17 (12.12–13.38) | 13.82 (13.43–14.15) | 1.15× | 1.01× | identical |
| neon | 21130 (19060–22520) | 16740 (16665–23455) | 13455 (12070–13720) | 1.26× | 1.57× | identical |
| string | 31292.5 (28340–32290) | 23645 (23470–27787.5) | 19545 (19055–19842.5) | 1.32× | 1.60× | identical |
| atomic | 58.27 (53.97–87.54) | 49.55 (49.3–76.27) | 57.05 (56.32–60.02) | 1.18× | 1.02× | identical |
| mutex | 133.8 (119.19–155.95) | 109.33 (106.82–136.67) | 122.88 (122.4–127.89) | 1.22× | 1.09× | identical |
| syscall | 166.21 (165.56–183.6) | 154.05 (142.42–177.62) | 173.54 (160.77–184.74) | 1.08× | 0.96× | identical |
| pingpong | 9113.6 (7804.4–13183.4) | 7627.6 (7197.2–11092.2) | 7808.8 (7399.8–9593) | 1.19× | 1.17× | identical |
| python_start | 171 (141–212) | 148 (135–207) | 198 (197–238) | 1.16× | 0.86× | identical |
| python_compute | 822 (668–987) | 830 (675–831) | 488 (475–596) | 0.99× | 1.68× | identical |
| python_json | 1163 (904–1198) | 1063 (916–1083) | 675 (673–861) | 1.09× | 1.72× | identical |
| gcc_compile | 5654 (4695–6770) | 4806 (4656–6134) | 4505 (4147–5638) | 1.18× | 1.26× | identical |
| perl_regex | 197 (180–253) | 178 (165–242) | 202 (199–265) | 1.11× | 0.98× | identical |
| gzip | 356 (337–617) | 351 (328–412) | 382 (339–517) | 1.01× | 0.93× | identical |
| xz | 696 (636–935) | 637 (632–785) | 677 (666–895) | 1.09× | 1.03× | identical |
| sort | 466 (454–583) | 465 (420–549) | 461 (453–520) | 1.00× | 1.01× | identical |
| sha256 | 15 (15–32) | 15 (14–16) | 23 (22–27) | 1.00× | 0.65× | identical |


## Interpret the Numbers Carefully

NEON and string microbenchmarks show interpreter improvements of about 1.26× and 1.32×. The JIT benefits some compute-heavy workloads, while translation overhead makes several short workloads slower: Python startup is 198 ms versus 171 ms for Build 899, and the short SHA-256 macro is 23 ms versus 15 ms. Not every workload improves.

Macro timings varied by up to approximately 47% on the busy host; differences below about 10% are not significant in this run. These results are not device benchmarks, universal speedups, battery measurements or network benchmarks. The phone result above is a separate maintainer report. No optional-JIT device benchmark is available in the reviewed evidence.
