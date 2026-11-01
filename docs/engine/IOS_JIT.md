# Optional JIT on iOS (2026-11-01)

JIT is optional. The default interpreter works without executable-memory permission or a debugger. The selected 1.1.0 Build 900 IPA includes JIT code; the source configuration still defaults to an interpreter-only build.

## Build Variants

| Variant | Configuration | Runtime behavior |
| --- | --- | --- |
| Default source build | `LINUXRUNNER_JIT=NO` | Interpreter only |
| JIT-capable (selected Build 900 IPA) | `LINUXRUNNER_JIT=YES` | Interpreter by default; JIT only after platform permission and successful activation |

Under **Settings → Advanced**, enable **Developer Mode**, then open **Execution Engine / StikDebug**. Inspect **JIT Status**, running backend, debugger state and translated-block counts. Selecting a mode is not proof it is active. **Attach with StikDebug** and **Activate JIT Now** request experimental activation; they do not promise compatibility with every OS/signing method.

The reviewed engineering record establishes host and Simulator execution, not successful physical-device JIT activation. The maintainer's GPU/device application tests do not by themselves qualify JIT. Device JIT/TXM activation remains separately unverified in the available record.

## Runtime behaviour (JIT-capable variant)

Settings › Execution Engine: Interpreter (default) / Automatic / JIT. The screen shows the backend that is
actually running and the runtime's reason, never the selected mode as if it were active.

- The translator never maps executable memory by itself on iOS (`jit.c`: `jit_init` uses only a region
  provided by `jit_ios_acquire`). Until then every block runs on the gadgets as in the standard build.
- `jit_ios_acquire(txm=false)` — devices without TXM: the existing dual mapping (RW view + `vm_remap` alias
  made `PROT_READ|PROT_EXEC`). iOS's own `mprotect` check is the permission test; it succeeds only for a
  process a debugger has attached to (StikDebug attaches and detaches).
- `jit_ios_acquire(txm=true)` — iOS 26+: StikDebug's documented breakpoint protocol, `x16=1; brk #0xf00d`
  (`JIT26PrepareRegion(NULL, 120 MB)`, returns the RX mapping), a writable alias via `vm_remap` +
  `mprotect(RW)`, then `x16=0; brk #0xf00d` (`JIT26Detach`). Called only while `sysctl` reports `P_TRACED`:
  a `brk` without StikDebug's script attached kills the process. The single 120 MB region is prepared before
  detaching, as the protocol requires.
- Both paths then run a generated `mov w0,#42; ret` through the executable view before reporting success.
- Activation can happen while Linux is running (StikDebug's `enable-jit` URL attaches to the running pid);
  blocks translated afterwards run natively, existing ones keep their gadgets.
- Crash sentinel: the path being tried is stored before the attempt. If LinuxRunner dies during it, the next
  launch records that path as failed, never retries it automatically, and reports this in the status.
- `stikdebug://enable-jit?bundle-id=…&pid=…` (+ `script-name=universal` on iOS 26+);
  `stikdebug` is declared in `LSApplicationQueriesSchemes`.

Not used: private APIs (`csops` / `CS_DEBUGGED` is not read; the debugger check is the public `sysctl`
`P_TRACED`, the permission check is iOS's own `mprotect`), StikJIT's built-in helper extension (needs
pairing files, Bonjour/local-network permissions and its own target; StikJIT states no license), any bypass
of code signing. Signing tools must keep `get-task-allow` for a debugger to attach.

## Verification so far (Mac only)

- `jit_smoke.sh`: MAP_JIT and dual mapping on Alpine, Debian, Arch — 6/6 PASS with the changed `jit.c`.
- iOS Simulator builds: standard and JIT-capable both build.
- iOS Simulator run (iPhone 18 Pro, iOS 27 runtime, JIT-capable build, mode Automatic), after updating
  CoreSimulator with `xcodebuild -runFirstLaunch`:
  - launched with lldb attached (`P_TRACED`): the app logged `activate ok=1 JIT active · Classic: dual
    mapping, 120 MB code region`; the bundled Alpine booted and ran its shell; `jit_describe()` called in the
    running app reported `mode: JIT` and 16571 blocks translated.
  - launched without a debugger: no activation attempt; `mode: off (gadgets only)`.
  The Simulator executes the iOS code paths but does not enforce iOS's executable-memory policy, so this
  proves the integration (acquisition, live activation, translation, interpreter fallback), not that a
  device grants the region. The TXM path has not run anywhere.

## Device test plan

1. Install the JIT-capable IPA signed with `get-task-allow`; Settings › Execution Engine shows "Interpreter".
2. Mode Automatic → Attach with StikDebug → return: status should become "JIT active · Classic…" (iOS ≤ 18)
   or "JIT active · TXM…" (iOS 26); `cat /proc/ish/jit` in the terminal must say `mode: JIT`.
3. Without StikDebug: status must stay Interpreter with iOS's refusal as the reason; Linux keeps working.
4. Compare `interp_bench` / Python start with and without the JIT on the device.
