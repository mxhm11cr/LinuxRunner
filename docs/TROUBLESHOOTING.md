# Troubleshooting

Preserve your environment first. Do not uninstall LinuxRunner, delete its root filesystem, remove package locks or disable package signatures as an initial repair step. Save Linux documents before stopping a desktop. Back up important files before package repair or upgrades.

## Find Repair and Diagnostic Controls

For the active environment, open **Home → App Manager**. The equivalent path is **Distributions → <environment> → Install, Manage and Repair Applications**.

In **Repair Center**:

1. Run **Diagnose Package State** and **Check Dependencies** first.
2. Review the terminal output.
3. If needed, select **Check / Repair Package State** and review its confirmation. Debian also has **Configure Interrupted Debian Packages**.

Repair executes distribution package tools and maintainer scripts. Its metadata checkpoint is not a complete filesystem backup or automatic rollback.

## Distribution Installation Fails or Stops

Keep LinuxRunner in the foreground and read the current progress/error text. Check network access, device date/time and available storage. Download progress and package configuration are separate phases; there is no fixed completion time.

After Debian/Arch reports a failure, use **Try Again** in the same setup workflow. Arch retains verified downloads for retry. If you cancel, wait for cancellation to finish before starting another operation. Avoid installing another environment over a partially configured one merely to retry.

If **Restart LinuxRunner** is displayed, fully close the app from the app switcher and reopen it. Existing environment selection and account setup resume. Use **Configure <environment name>** for an existing root; **Install Another Environment** creates a separate one.

If first-start initialization reports no progress or an error, export diagnostics, reopen the app and retry the indicated operation. Do not repeatedly force-close an active package transaction.

## Package-Manager Errors or Interrupted Installation

Only one modifying package operation should run in an environment. If a lock is active, allow the other operation to finish; never delete a lock simply because it exists.

**Debian:** use **Configure Interrupted Debian Packages**, then diagnosis/dependency checks. **Check / Repair Package State** can request dependency repair; review changes before confirming.

**Arch:** use diagnosis followed by the managed repair action. It checks package state and can restore supported damaged ownership records using validated, signed cached packages. Recovery may correctly stop if an archive, signature or ownership check fails. Keep the first error and export the report; do not force overwrite files, edit pacman's local database or bypass signatures. Arch package installation uses a consistent `pacman -Syu` transaction; avoid partial upgrades.

**Alpine:** run diagnosis and dependency checks before managed repair. Repair can run `apk fix`; review the operation. Package availability alone does not establish runtime compatibility.

## Desktop Installation Problems or Missing Dependencies

Use **Distributions → your distribution → Continue Setup → Install Desktop Components**, or **Desktop → Desktop Settings… → Desktop Session → Install / Repair Desktop Components**. Keep the app open during installation. **Details** and **GUI Logs** show the failing component/package step.

Try LinuxRunner Desktop or LXQt before a more demanding desktop. **Settings → Desktop Environment** offers the current installation controls. Debian Enlightenment is disabled because it stops at the splash screen. Alpine's desktop path has less coverage.

If dependencies are missing, use **Check Dependencies** and the package manager's supported repair flow. Do not download unrelated libraries from arbitrary sites or globally replace system libraries.

## A Black Desktop, Missing Panel or Missing Wallpaper

Distinguish a black Linux desktop from the entire native app failing to open. An X11 window manager can be alive even if the panel or desktop surface has failed.

1. Read the startup status and **Details** before restarting.
2. Try **Desktop → Start / Reconnect Desktop**, or **Sessions → Resume Desktop**, if the session is alive.
3. If only a panel/surface failed, open applications may still work. Components restart a bounded number of times; repeated failures are reported as degraded rather than silently marked ready.
4. Save work, then use **Stop Desktop** and start it again if reconnecting does not resolve the issue. Stopping closes applications in that desktop.
5. Export the report with `panel.log`, `desktop.log` and Qt diagnostics when present.

Do not hardcode a display number, run `xhost +` or replace X11 authorization files. Managed launchers supply the current user's authenticated display context.

## A Graphical Application Crashes

Check [Compatibility](COMPATIBILITY.md). Telegram login, messaging and calls have been tested on Arch; report any regression with the exact package and build. Arch GTK/glycin applications still have write-seal compatibility gaps; a Qt desktop does not fix every GTK application.

Launch through **Home → Applications** with a regular Linux user. If using a shell, open a fresh terminal after starting Desktop so it can adopt the current session context. A shell opened earlier cannot inherit later environment changes.

For a problem specific to Metal, select **Settings → Graphics → Linux GPU Device → GPU → Software Rendering**, then fully close/reopen LinuxRunner. Record **Active renderer** before and after. The separate Linux Applications Renderer control does not select the virtual GPU. Close other heavy applications if memory is tight. Record the program/package version, exact action and whether the whole app closed or just the Linux program.

## Terminal Does Not Start or Uses the Wrong Environment

Check Home's runtime status. Use **Start Linux Runtime** if offered. Only the active environment can open a terminal. To switch, use **Distributions → <environment> → Use on Next Launch**, fully close LinuxRunner and reopen it.

Check the selected account under **Settings → Linux Users**. After opening a shell, use `id` and `pwd` to check the account/home. Do not reinstall the distribution just because a terminal failed; preserve the error and export diagnostics.

## Existing Desktop Sessions and Reconnection

**Done** detaches the viewer while retaining the live session. **Resume Desktop** reconnects to it. **Stop Desktop** ends that desktop and its applications. Switching the default user requires saving work and stopping/restarting the desktop before applications use the new account.

iOS/iPadOS may suspend or terminate LinuxRunner. Keep Screen Awake prevents idle locking in the foreground; it does not prevent termination or preserve Linux processes after the app exits. Reopen the app and desktop; saved files should remain in the same storage container.

## Input or iPad Display Problems

Try **Fit to Screen** and **Actual Size** for viewer scaling. **Desktop Settings… → Display → Resolution & Viewer Preferences** chooses a Linux resolution when a new session starts; save work before restarting. Use the on-screen keyboard/modifier bar to distinguish accessory input problems from Linux application behavior.

For pointer issues, inspect **Settings → Devices** and its mouse settings. Report whether input came from touch, mouse, trackpad or a hardware keyboard. iPad support has been physically tested. Include model, iPadOS version, orientation and window size when reporting a layout problem.

## The Installer Says the IPA Is Invalid

Download the actual IPA from the release's **Assets**, not a saved preview/web page. Compare file size and SHA-256 before signing. A mismatch can explain errors such as “not a ZIP”, “could not find EOCD” or “invalid IPA”. If the checksum matches, report the exact tool/version and error; static archive validation cannot guarantee every signing tool accepts the package. [Installation](INSTALLATION.md)

## LinuxRunner Opens to a Black Screen

An earlier report concerns iPhone 11/iOS 17; its resolution is not confirmed. Report device/OS and app version/build. If the interface is unavailable, inspect the platform's **Settings → Privacy & Security → Analytics & Improvements → Analytics Data** for LinuxRunner crash/hang entries. Review their contents before sharing. A configured deployment target does not prove all OS versions work.

## Environments Seem Missing After Re-signing

The app may use an App Group container or private application storage, depending on its signature. A changed bundle identifier, App Group setting or signing/container configuration can make old environments inaccessible. Preserve the installed app and its backups while investigating. Reinstalling with the original identity/configuration may restore access, but do not promise recovery after deletion or data loss.

## Collect Logs and Report a Problem

- **Settings → Advanced → Export Open Desktop Report** collects startup/session and related diagnostics.
- **Home → App Manager → Diagnostics → Export Diagnostic Report** provides package/runtime information.
- **Home → View Crash Reports** shows recorded Linux application crashes.

Review exported files before sharing. Automatic redaction is useful but cannot guarantee that logs, filenames, screenshots or program output contain no private information. Remove messages, account identifiers, addresses, tokens and sensitive paths. Keep the original privately if further diagnosis is needed.

Open a [GitHub issue](https://github.com/mxhm11cr/LinuxRunner/issues/new/choose) with device/OS, app version/build/source identity, distribution, package version, desktop/graphics mode, steps, expected/actual behavior and the first relevant error. State whether an app merely launched or was actually used. Use the [private security channel](../SECURITY.md) for vulnerabilities.

## Hyprland or Wayfire Startup Fails

Hyprland is experimental, Arch-only and requires at least 8 GB RAM; Metal is recommended. If the first launch reports an error, select **Retry** when offered. Check that desktop package installation completed. Save work before stopping/restarting a session; try LinuxRunner Desktop or LXQt if failures persist. Do not remove the root filesystem. Include the selected environment, renderer, memory and first compositor error in the report.

## Metal Is Selected but Rendering Is Still Software

Use **Settings → Graphics → Linux GPU Device → GPU → Metal GPU · Experimental**, then fully close and reopen the app. Check **Active renderer** and its reason. Automatic starts the virtual GPU with Linux and falls back to software rendering if it cannot start. A Metal viewer/presentation label alone does not mean the Linux application used GPU rendering. A workload can use an unsupported API or fall back even with Metal selected.

## JIT Is Unavailable

JIT is optional. Keep using Interpreter if iOS refuses executable memory. Under **Settings → Advanced**, enable **Developer Mode**, then inspect **Execution Engine / StikDebug → JIT Status** and the running backend. Debugger attachment and a JIT-capable build alone do not prove translated blocks are running. Signing, OS and activation-method restrictions apply; do not remove the app or its Linux data to obtain JIT.

## External Display Shows the Wrong View

Enable **Settings → Devices → External Desktop Output**, connect a supported display/receiver and use **Desktop → Use iPhone as Trackpad**. A separate iOS external scene and the first desktop frame are required. Plain mirroring shows the phone's view and cannot provide independent controls. Cancel a waiting trackpad connection if needed, then check the receiver/platform connection. [External display guide](NETWORKING_AND_DISPLAYS.md)
