import SwiftUI
import UniformTypeIdentifiers

// Guided setup: Welcome → Distribution → Install Linux → (restart into it) →
// Account → Desktop → Finish → Open Desktop. Uses the verified installers, the
// native account setup (password hashed in the app; tools installed if absent)
// and the same desktop-profile repair as Open Desktop. It opens by itself only
// when no Linux environment exists; existing environments, homes and accounts
// are never modified, and an existing account can be used as it is.
enum SetupStep: String { case welcome, distribution, install, relaunch, account, desktop, done }

struct SetupWizardView: View {
    @ObservedObject var manager: EnvironmentManager
    var openDesktop: () -> Void
    @Environment(\.dismiss) private var dismiss
    @AppStorage("LinuxRunnerSetup.step") private var stepRaw = SetupStep.welcome.rawValue
    @AppStorage("LinuxRunnerSetup.distribution") private var distribution = "debian"
    @AppStorage("LinuxRunnerSetup.root") private var setupRoot = ""
    @StateObject private var debian = DebianInstaller()
    @StateObject private var arch = ArchInstaller()
    @StateObject private var fedora = FedoraInstaller()
    @StateObject private var kali = KaliInstaller()
    @StateObject private var steamos = SteamOSInstaller()
    @State private var steamOSImporter = false
    @State private var username = ""
    @State private var password = ""
    @State private var confirmation = ""
    @State private var administrator = true
    @State private var working: String?
    @State private var problem: String?
    @State private var desktopReady = false
    @State private var extraDesktops: Set<String> = []
    @State private var chosenDesktop = "linuxrunner"
    private var step: SetupStep { SetupStep(rawValue: stepRaw) ?? .welcome }

    var body: some View {
        NavigationView {
            Form { content }
                .navigationTitle(title)
                .toolbar { ToolbarItem(placement: .cancellationAction) { Button(step == .done ? "Close" : "Later") { dismiss() }.disabled(working != nil || debian.running || arch.running || fedora.running || kali.running || steamos.running || manager.busy) } }
        }
        .navigationViewStyle(.stack)
        .overlay(alignment: .bottom) { InstallProgressCard() }
        .interactiveDismissDisabled(working != nil || debian.running || arch.running || fedora.running || kali.running || steamos.running || manager.busy)
        .onAppear(perform: resume)
        .fileImporter(isPresented: $steamOSImporter, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls): if let source = urls.first { installSteamOS(from: source) }
            case .failure(let error): problem = error.localizedDescription
            }
        }
    }

    private var title: String {
        switch step {
        case .welcome: return "Welcome"; case .distribution: return "Choose Linux"; case .install: return "Install Linux"
        case .relaunch: return "Restart LinuxRunner"; case .account: return "Your Account"; case .desktop: return "Desktop"; case .done: return "Ready"
        }
    }

    @ViewBuilder private var content: some View {
        switch step {
        case .welcome:
            Section { Text("LinuxRunner runs real ARM64 Linux programs on this iPhone with its own interpreter — no virtual machine.")
                Text("Setup installs Linux, creates your account and prepares the graphical desktop. Everything can be changed later in Settings.").foregroundStyle(.secondary) }
            if !manager.environments.isEmpty { Section { Text("Configure an existing environment or install another distribution. Your files and accounts are preserved.").font(.footnote) } }
            Section { Button("Continue") { stepRaw = SetupStep.distribution.rawValue } }
        case .distribution:
            Section(footer: Text("Debian is recommended. Arch Linux ARM is a rolling release with pacman and experimental X11/Wayland desktops. Fedora offers experimental DNF userspace with a lightweight X11 desktop. Kali Linux is the official Kali userspace with its Xfce desktop and optional standard tools; kernel-level features such as monitor-mode Wi-Fi, raw packet capture and kernel modules are not available on iOS. Alpine is small and mainly for the terminal. SteamOS ARM64 is experimental: it is installed from Valve's SteamFrame recovery image (not official SteamOS support; Steam games are not supported). Desktop compatibility varies.")) {
                Picker("Distribution", selection: $distribution) { Text("Debian GNU/Linux 13 (ARM64) — recommended").tag("debian"); Text("Arch Linux ARM (ARM64)").tag("arch"); Text("Fedora Linux 44 (ARM64)").tag("fedora"); Text("Kali Linux \(KaliImage.version) (ARM64)").tag("kali"); Text("Alpine Linux 3.24 (ARM64)").tag("alpine"); Text("SteamOS ARM64 — experimental").tag("steamos") }.pickerStyle(.inline).labelsHidden()
            }
            let installed = manager.environments.filter { ($0.distributionID ?? "alpine") == distribution }
            if !installed.isEmpty {
                Section("Already Installed") {
                    ForEach(installed) { environment in
                        Button("Configure " + environment.name) { configureExisting(environment) }
                    }
                    Text("Uses the existing environment, accounts and files. Switching environments takes effect after relaunch.").font(.footnote).foregroundStyle(.secondary)
                }
            }
            Section { Button(installed.isEmpty ? "Continue" : "Install Another Environment") { stepRaw = SetupStep.install.rawValue } }
        case .install: install
        case .relaunch:
            Section { Text("Linux is installed and selected. Close LinuxRunner completely (swipe it away in the app switcher) and open it again — setup continues with your account.") }
            Section { Text("iOS allows one Linux runtime per app launch, so this restart is needed once.").font(.footnote).foregroundStyle(.secondary) }
        case .account: account
        case .desktop: desktop
        case .done:
            Section { Label("Linux is ready", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                Text("Add applications any time from Packages › App Catalog.").font(.footnote).foregroundStyle(.secondary) }
            Section { Button { stepRaw = SetupStep.welcome.rawValue; setupRoot = ""; dismiss(); openDesktop() } label: { Label("Open Desktop", systemImage: "desktopcomputer").font(.headline) }.disabled(!LRRuntimeBridge.isRunning()) }
        }
        if let working { Section { HStack(spacing: 12) { ProgressView(); Text(working) } } }
        if let problem { Section { Text(problem).foregroundStyle(.red) } }
    }

    @ViewBuilder private var install: some View {
        let free = (try? LRRuntimeBridge.managedStorageURL().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? 0
        let manifest = try? DebianManifest.load()
        let steamManifest = try? SteamOSManifest.load()
        let alpineBytes = Int64((try? Bundle.main.url(forResource: "root", withExtension: "tar.gz")?.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        let download = distribution == "debian" ? (manifest?.downloadBytes ?? 0) : distribution == "fedora" ? FedoraImage.bytes : distribution == "kali" ? KaliImage.bytes : 0
        let installed = distribution == "debian" ? (manifest?.installedBytes ?? 0) : alpineBytes * 3
        Section("Storage") {
            row("Download", distribution == "steamos" ? (steamManifest.flatMap { SteamOSInstaller.downloadedImage($0) } != nil ? "Already downloaded" : ByteCountFormatter.string(fromByteCount: steamManifest?.source?.bytes ?? 0, countStyle: .file) + " from Valve") : distribution == "arch" ? "Calculated from the signed package selection" : ["debian", "fedora", "kali"].contains(distribution) ? ByteCountFormatter.string(fromByteCount: download, countStyle: .file) : "None (included in the app)")
            row(distribution == "debian" || distribution == "steamos" ? "Installed size" : "Installed size (estimate)", distribution == "steamos" ? ByteCountFormatter.string(fromByteCount: steamManifest?.installedBytes ?? 0, countStyle: .file) : distribution == "arch" ? "At least 1.5 GB free for base installation" : distribution == "fedora" ? "At least 2 GB free for base installation" : distribution == "kali" ? "About 0.9 GB base; desktop about 2 GB more; standard tools about 6 GB more" : ByteCountFormatter.string(fromByteCount: installed, countStyle: .file))
            row("Available", ByteCountFormatter.string(fromByteCount: free, countStyle: .file))
        }
        if distribution == "debian" {
            Section {
                if debian.running { Text(debian.message); ProgressView(value: debian.fraction); Button("Cancel", role: .destructive) { debian.cancel() } }
                else { Button("Install Debian") { startInstall() }.disabled(free < max(Int64(1 << 30), download + installed)) }
                if let failure = debian.failure { Text(failure).foregroundStyle(.red); Button("Try Again") { startInstall() } }
            } footer: { Text("Official Debian packages, checked against Debian's signed index. The desktop, sound and account tools are added in the next steps.") }
        } else if distribution == "arch" {
            Section {
                if arch.running { Text(arch.message); ProgressView(value: arch.fraction); Button("Cancel", role: .destructive) { arch.cancel() } }
                else { Button("Install Arch Linux ARM") { startInstall() }.disabled(free < Int64(1_500_000_000)) }
                if let failure = arch.failure { Text(failure).foregroundStyle(.red); Button("Try Again") { startInstall() } }
            } footer: { Text("Official signed Arch Linux ARM packages. Verified downloads are kept for retry. First-start configuration finishes before desktop packages are installed.") }
        } else if distribution == "fedora" {
            Section {
                if fedora.running { Text(fedora.message); ProgressView(value: fedora.fraction); Button("Cancel", role: .destructive) { fedora.cancel() } }
                else { Button("Install Fedora Linux") { startInstall() }.disabled(free < Int64(2_000_000_000)) }
                if let failure = fedora.failure { Text(failure).foregroundStyle(.red); Button("Try Again") { startInstall() } }
            } footer: { Text("Authenticates Fedora's official AArch64 WSL userspace image; runs it with LinuxRunner, without WSL, a Linux kernel or systemd. The lightweight GTK/X11 desktop and experimental KDE Plasma are available. Fedora sound is not yet qualified.") }
        } else if distribution == "kali" {
            Section {
                if kali.running { Text(kali.message); ProgressView(value: kali.fraction); Button("Cancel", role: .destructive) { kali.cancel() } }
                else { Button("Install Kali Linux") { startInstall() }.disabled(free < Int64(2_000_000_000)) }
                if let failure = kali.failure { Text(failure).foregroundStyle(.red); Button("Try Again") { startInstall() } }
            } footer: { Text("Kali's official ARM64 root filesystem (\(KaliImage.version)), checked against its published SHA-256; packages come from Kali's signed archive. The image's preset root and kali passwords are locked at first start; you create your own account next. Runs with LinuxRunner, without a Linux kernel or systemd.") }
        } else if distribution == "steamos" {
            Section {
                Text(steamos.message)
                if steamos.running {
                    ProgressView(value: steamos.fraction)
                    Button("Cancel Installation", role: .destructive) { steamos.cancel() }
                } else {
                    let required = steamManifest?.directRequiredFreeBytes(imagePresent: steamManifest.flatMap { SteamOSInstaller.downloadedImage($0) } != nil) ?? .max
                    Button(steamos.failure == nil ? "Install SteamOS" : "Try Again") { installSteamOSFromValve() }
                        .disabled(steamManifest?.supportsDirectInstall != true || free < required)
                    if free < required, required != .max {
                        Text("Needs " + ByteCountFormatter.string(fromByteCount: required, countStyle: .file) + " free while installing; about " + ByteCountFormatter.string(fromByteCount: steamManifest?.streamInstalledBytes ?? 0, countStyle: .file) + " stays in use afterwards.").foregroundStyle(.secondary)
                    }
                    if FileManager.default.fileExists(atPath: SteamOSInstaller.preparedArchiveURL.path) {
                        Button("Install Prepared SteamOS Archive") { installSteamOS(from: SteamOSInstaller.preparedArchiveURL) }
                            .disabled(steamManifest == nil || free < (steamManifest?.requiredFreeBytes ?? Int64.max))
                    }
                }
                if let failure = steamos.failure { Text(failure).foregroundStyle(.red) }
                if steamManifest == nil { Text("This build has no supported SteamOS manifest.").foregroundStyle(.secondary) }
            } footer: { Text("Downloads Valve's SteamFrame recovery image from Valve's server, checks its published SHA-256, and installs its ARM64 system with a terminal and the Plasma X11 desktop. Two display packages (x11vnc, libvncserver) come from Arch Linux ARM with pinned checksums. The download is removed after installation. Keep LinuxRunner open while it installs.") }
        } else if distribution == "alpine" {
            Section { if manager.busy { ProgressView("Unpacking Alpine Linux…") } else { Button("Install Alpine") { startInstall() } }
                if let error = manager.error { Text(error).foregroundStyle(.red) } }
        }
    }

    @ViewBuilder private var account: some View {
        if !LRRuntimeBridge.isRunning() || (!setupRoot.isEmpty && LRRuntimeBridge.activeRootName() != setupRoot) {
            Section { Text("Close LinuxRunner completely and open it again to continue.") }
        } else {
            let existing = LRRuntimeBridge.linuxAccounts().filter { $0["name"] != "root" && ($0["uid"].flatMap(Int.init) ?? 0) >= 1000 }
            if !existing.isEmpty {
                Section("Use an existing account") {
                    ForEach(existing, id: \.self) { account in Button(account["name"] ?? "") { useAccount(account["name"] ?? "") }.disabled(working != nil) }
                }
            }
            Section(existing.isEmpty ? "Create your account" : "Or create a new account") {
                TextField("Username", text: $username).textInputAutocapitalization(.never).autocorrectionDisabled().textContentType(.username)
                SecureField("Password", text: $password).textContentType(.newPassword)
                SecureField("Confirm password", text: $confirmation).textContentType(.newPassword)
                Toggle("Administrator (can use sudo)", isOn: $administrator)
                Button("Create Account") { createAccount() }.disabled(working != nil || username.isEmpty || password.isEmpty || password != confirmation)
                if !confirmation.isEmpty && password != confirmation { Text("The passwords do not match.").font(.footnote).foregroundStyle(.orange) }
            }
            Section { Text("Lowercase letters, digits, - and _. LinuxRunner stores only Linux's own password hash, never the password.").font(.footnote).foregroundStyle(.secondary) }
        }
    }

    @ViewBuilder private var desktop: some View {
        if LRRuntimeBridge.activeEnvironmentIsSteamOS() {
            Section {
                Label("SteamOS Plasma X11", systemImage: "desktopcomputer")
                Text("SteamOS includes the Plasma desktop; LinuxRunner adds its display bridge. This experiment uses software rendering.")
                if desktopReady { Label("Desktop components checked", systemImage: "checkmark"); Button("Continue") { stepRaw = SetupStep.done.rawValue } }
                else { Button("Check SteamOS Desktop") { prepareDesktop() }.disabled(working != nil) }
            }
        } else {
        let distro = DesktopCatalog.activeDistribution()
        Section(header: Text("Choose a Desktop"), footer: Text("Each desktop comes from \(DesktopCatalog.distributionName(distro))'s own packages (network needed). The display, input and file-sharing components every desktop uses are installed first. You can add or switch desktops later in Settings › Desktop Environment; existing settings and files are kept.")) {
            ForEach(DesktopCatalog.options) { option in desktopChoiceRow(option, distro: distro) }
        }
        .onAppear { if distro == "kali" && chosenDesktop == "linuxrunner" { chosenDesktop = "xfce" } }
        Section {
            let chosen = DesktopCatalog.option(chosenDesktop)
            if let working {
                HStack { ProgressView(); Text(working).font(.callout) }
            } else if desktopReady && DesktopCatalog.isInstalled(chosen.id) {
                Label("\(chosen.title) is installed and selected", systemImage: "checkmark.circle.fill")
                Button("Continue") { stepRaw = SetupStep.done.rawValue }
            } else {
                Button(DesktopCatalog.isInstalled(chosen.id) && desktopReady ? "Use \(chosen.title)" : "Install \(chosen.title)") { installChosenDesktop() }
                    .disabled(DesktopCatalog.unavailableReason(chosen.id, distro) != nil)
                if problem != nil { Button("Retry") { installChosenDesktop() } }
            }
        } footer: {
            if problem != nil { Text("The package log is kept; Settings › Repair Center can check and repair the package database without deleting your files.") }
        }
        if distro == "kali" && desktopReady {
            Section(header: Text("Kali Tools (Optional)"), footer: Text("Kali's standard tool selection (kali-linux-default) from Kali's signed archive: about 1.5 GB to download and 6 GB installed. Tools that need kernel access, such as monitor-mode Wi-Fi, raw packet capture or kernel modules, install but cannot use those features on iOS.")) {
                if LRRuntimeBridge.kaliToolsInstalled() || extraDesktops.contains("kali-tools") { Label("Kali standard tools installed", systemImage: "checkmark") }
                else {
                    Button("Install Kali Standard Tools") {
                        problem = nil; working = "Kali tools: starting"
                        LRRuntimeBridge.installKaliTools({ working = "Kali tools: \($0)" }) { error in
                            working = nil
                            if let error { problem = error.localizedDescription } else { extraDesktops.insert("kali-tools") }
                        }
                    }.disabled(working != nil)
                }
            }
        }
        }
    }

    @ViewBuilder private func desktopChoiceRow(_ option: DesktopOption, distro: String) -> some View {
        let reason = DesktopCatalog.unavailableReason(option.id, distro)
        let selected = chosenDesktop == option.id && reason == nil
        Button { if reason == nil { chosenDesktop = option.id; problem = nil } } label: {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                    .foregroundStyle(reason == nil ? Color.accentColor : Color.secondary).font(.title3)
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Text(option.title).foregroundStyle(reason == nil ? Color.primary : Color.secondary)
                        ForEach(option.badges(distro), id: \.self) { badge in
                            Text(badge).font(.caption2.weight(.semibold)).padding(.horizontal, 6).padding(.vertical, 2)
                                .background(Capsule().fill((badge == "Recommended" ? Color.accentColor : Color.secondary).opacity(0.15)))
                        }
                        if DesktopCatalog.isInstalled(option.id) && desktopReady { Text("Installed").font(.caption2).foregroundStyle(.green) }
                    }
                    Text(option.detail).font(.caption).foregroundStyle(.secondary)
                    if let reason { Label(reason, systemImage: "info.circle").font(.caption).foregroundStyle(.orange) }
                }
            }
        }
        .disabled(working != nil || reason != nil)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .accessibilityHint(reason ?? "Selects this desktop")
    }

    private func row(_ title: String, _ value: String) -> some View { HStack { Text(title); Spacer(); Text(value).foregroundStyle(.secondary) } }

    private func resume() {
        if step == .relaunch, LRRuntimeBridge.isRunning(), LRRuntimeBridge.activeRootName() == setupRoot { stepRaw = SetupStep.account.rawValue }
    }

    private func configureExisting(_ environment: LinuxEnvironment) {
        problem = nil; desktopReady = false
        setupRoot = environment.rootfsName
        distribution = environment.distributionID ?? "alpine"
        LRRuntimeBridge.setNextRootName(environment.rootfsName)
        stepRaw = manager.active(environment) ? SetupStep.account.rawValue : SetupStep.relaunch.rawValue
    }

    private func startInstall() {
        guard !debian.running, !arch.running, !fedora.running, !kali.running, !steamos.running, !manager.busy else { return }
        problem = nil
        let known = Set(manager.environments.map(\.rootfsName))
        let finish: (String) -> Void = { root in
            setupRoot = root; LRRuntimeBridge.setNextRootName(root)
            stepRaw = (LRRuntimeBridge.isRunning() && LRRuntimeBridge.activeRootName() == root) ? SetupStep.account.rawValue : SetupStep.relaunch.rawValue
        }
        if distribution == "debian" {
            debian.install { environment in manager.acceptInstalled(environment); finish(environment.rootfsName) }
        } else if distribution == "arch" {
            arch.install { environment in manager.acceptInstalled(environment); finish(environment.rootfsName) }
        } else if distribution == "fedora" {
            fedora.install { environment in manager.acceptInstalled(environment); finish(environment.rootfsName) }
        } else if distribution == "kali" {
            kali.install { environment in manager.acceptInstalled(environment); finish(environment.rootfsName) }
        } else if distribution == "alpine" {
            manager.error = nil; manager.create(name: "Alpine Linux")
            Task { @MainActor in
                while manager.busy { try? await Task.sleep(nanoseconds: 300_000_000) }
                if let created = manager.environments.first(where: { !known.contains($0.rootfsName) }) { finish(created.rootfsName) }
            }
        } else { problem = "Choose a supported Linux distribution before installing." }
    }

    private func installSteamOSFromValve() {
        guard !debian.running, !arch.running, !fedora.running, !kali.running, !steamos.running, !manager.busy else { return }
        problem = nil
        steamos.installFromValve { environment in
            manager.acceptInstalled(environment)
            setupRoot = environment.rootfsName
            LRRuntimeBridge.setNextRootName(environment.rootfsName)
            stepRaw = SetupStep.relaunch.rawValue
        }
    }

    private func installSteamOS(from source: URL) {
        guard !debian.running, !arch.running, !fedora.running, !kali.running, !steamos.running, !manager.busy else { return }
        problem = nil
        steamos.install(source: source) { environment in
            manager.acceptInstalled(environment)
            setupRoot = environment.rootfsName
            LRRuntimeBridge.setNextRootName(environment.rootfsName)
            stepRaw = SetupStep.relaunch.rawValue
        }
    }

    private func createAccount() {
        problem = nil; working = "Preparing"
        LRRuntimeBridge.createLinuxUser(username, password: password, administrator: administrator, progress: { working = $0 }) { error in
            working = nil
            if let error { problem = error.localizedDescription; return }
            password = ""; confirmation = ""
            useAccount(username)
        }
    }

    private func useAccount(_ name: String) {
        do { try LRRuntimeBridge.setDefaultLinuxUser(name); problem = nil; stepRaw = SetupStep.desktop.rawValue }
        catch { problem = error.localizedDescription }
    }

    // Base components first (every desktop session uses them), then the chosen
    // desktop. Stages are numbered so progress is meaningful; failures keep the
    // log and leave the previous desktop selected.
    private func installChosenDesktop() {
        let option = DesktopCatalog.option(chosenDesktop)
        guard DesktopCatalog.unavailableReason(option.id, DesktopCatalog.activeDistribution()) == nil else { return }
        let needsExtra = option.id != "linuxrunner" && !DesktopCatalog.isInstalled(option.id)
        let total = needsExtra ? 2 : 1
        problem = nil; working = "Step 1 of \(total) · Desktop components: starting"
        LRRuntimeBridge.prepareDesktopProfile({ working = "Step 1 of \(total) · Desktop components: \($0)" }) { error in
            if let error { working = nil; problem = error.localizedDescription; return }
            desktopReady = true
            guard needsExtra, let install = DesktopCatalog.installer(option.id) else { selectDesktop(option.id); return }
            working = "Step 2 of 2 · \(option.title): starting"
            install({ working = "Step 2 of 2 · \(option.title): \($0)" }) { error in
                if let error { working = nil; problem = error.localizedDescription; return }
                selectDesktop(option.id)
            }
        }
    }

    // SteamOS: the image already contains Plasma; this only runs its
    // offline profile check (steamos-profile.sh) and selects Plasma.
    private func prepareDesktop() {
        problem = nil; working = "Checking the SteamOS desktop"
        LRRuntimeBridge.prepareDesktopProfile({ working = $0 }) { error in
            working = nil
            if let error { problem = error.localizedDescription } else { desktopReady = true; selectDesktop("plasma") }
        }
    }

    private func selectDesktop(_ id: String) {
        working = nil
        // Same per-environment choice as Settings › Desktop Environment.
        UserDefaults.standard.set(id, forKey: "LinuxRunnerDesktop.environment." + LRRuntimeBridge.activeRootName())
        extraDesktops.insert(id)
    }
}

struct DesktopOption: Identifiable {
    let id: String
    let title: String
    let detail: String
    let experimental: Bool
    let lightweight: Bool
    func badges(_ distro: String) -> [String] {
        var b: [String] = []
        if id == "linuxrunner" { b.append("Recommended") }
        if lightweight { b.append("Lightweight") }
        if experimental { b.append("Experimental") }
        return b
    }
}

// What each distribution can actually install and start, matching
// desktop-profile.sh. Unavailable options say why instead of disappearing.
enum DesktopCatalog {
    static let options: [DesktopOption] = [
        DesktopOption(id: "linuxrunner", title: "LinuxRunner Desktop", detail: "Openbox with a panel and file manager. Fastest start and lowest memory use; best for first-time Linux users.", experimental: false, lightweight: true),
        DesktopOption(id: "xfce", title: "Xfce", detail: "On Kali: Kali's own desktop (kali-desktop-xfce) with the Kali menu, themes and wallpaper.", experimental: false, lightweight: false),
        DesktopOption(id: "lxqt", title: "LXQt", detail: "A light, traditional Qt desktop with a start menu, panel and file manager.", experimental: false, lightweight: true),
        DesktopOption(id: "wayfire", title: "Wayfire", detail: "A Wayland compositor with a panel and dock, running in the X display.", experimental: false, lightweight: false),
        DesktopOption(id: "enlightenment", title: "Enlightenment", detail: "A distinctive desktop with its own compositor; large download.", experimental: false, lightweight: false),
        DesktopOption(id: "plasma", title: "KDE Plasma", detail: "The full KDE desktop. Heaviest option: slowest start and highest memory use.", experimental: true, lightweight: false),
        DesktopOption(id: "hyprland", title: "Hyprland", detail: "A tiling Wayland compositor with Waybar, for keyboard-driven use.", experimental: true, lightweight: false),
    ]
    static func option(_ id: String) -> DesktopOption { options.first { $0.id == id } ?? options[0] }
    static func activeDistribution() -> String {
        if LRRuntimeBridge.activeEnvironmentIsKali() { return "kali" }
        if LRRuntimeBridge.activeEnvironmentIsDebian() { return "debian" }
        if LRRuntimeBridge.activeEnvironmentIsArch() { return "arch" }
        if LRRuntimeBridge.activeEnvironmentIsFedora() { return "fedora" }
        return "alpine"
    }
    static func distributionName(_ d: String) -> String {
        switch d { case "debian": return "Debian"; case "arch": return "Arch Linux ARM"; case "fedora": return "Fedora"; case "kali": return "Kali Linux"; default: return "Alpine Linux" }
    }
    static func unavailableReason(_ id: String, _ d: String) -> String? {
        switch (id, d) {
        case ("linuxrunner", _): return nil
        case ("xfce", "kali"): return nil
        case ("xfce", _): return "Offered on Kali Linux, where it is Kali's own desktop; not yet tested on this distribution."
        case (_, "kali") where id != "linuxrunner": return "Kali's desktop is Xfce; other desktops are not yet tested on Kali."
        case ("lxqt", "fedora"): return "Not yet tested with Fedora in LinuxRunner, so it is not offered yet."
        case ("lxqt", _): return nil
        case ("enlightenment", "debian"): return "Debian's Enlightenment session stops at its splash screen in LinuxRunner."
        case ("enlightenment", "fedora"): return "Not yet tested with Fedora in LinuxRunner, so it is not offered yet."
        case ("enlightenment", _): return nil
        case ("wayfire", "alpine"): return "Alpine's packages lack what LinuxRunner's Wayfire session needs."
        case ("wayfire", _): return nil
        case ("plasma", "alpine"): return "LinuxRunner's Plasma session supports Debian, Arch Linux ARM and Fedora only."
        case ("plasma", _): return nil
        case ("hyprland", "arch"): return nil
        case ("hyprland", "debian"): return "Debian 13 does not package Hyprland; it needs Arch Linux ARM."
        case ("hyprland", _): return "Needs Arch Linux ARM, which packages Hyprland for this setup."
        default: return "Not available."
        }
    }
    static func isInstalled(_ id: String) -> Bool {
        switch id {
        case "xfce": return LRRuntimeBridge.xfceInstalled()
        case "lxqt": return LRRuntimeBridge.lxqtInstalled()
        case "enlightenment": return LRRuntimeBridge.enlightenmentInstalled()
        case "wayfire": return LRRuntimeBridge.wayfireInstalled()
        case "plasma": return LRRuntimeBridge.plasmaInstalled()
        case "hyprland": return LRRuntimeBridge.hyprlandInstalled()
        default: return true // part of the desktop components
        }
    }
    static func installer(_ id: String) -> ((@escaping (String) -> Void, @escaping (Error?) -> Void) -> Void)? {
        switch id {
        case "xfce": return { LRRuntimeBridge.installXfce($0, completion: $1) }
        case "lxqt": return { LRRuntimeBridge.installLXQt($0, completion: $1) }
        case "enlightenment": return { LRRuntimeBridge.installEnlightenment($0, completion: $1) }
        case "wayfire": return { LRRuntimeBridge.installWayfire($0, completion: $1) }
        case "plasma": return { LRRuntimeBridge.installPlasma($0, completion: $1) }
        case "hyprland": return { LRRuntimeBridge.installHyprland($0, completion: $1) }
        default: return nil
        }
    }
}
