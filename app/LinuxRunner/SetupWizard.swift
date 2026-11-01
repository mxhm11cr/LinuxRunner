import SwiftUI

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
    @State private var username = ""
    @State private var password = ""
    @State private var confirmation = ""
    @State private var administrator = true
    @State private var working: String?
    @State private var problem: String?
    @State private var desktopReady = false
    @State private var extraDesktops: Set<String> = []
    private var step: SetupStep { SetupStep(rawValue: stepRaw) ?? .welcome }

    var body: some View {
        NavigationView {
            Form { content }
                .navigationTitle(title)
                .toolbar { ToolbarItem(placement: .cancellationAction) { Button(step == .done ? "Close" : "Later") { dismiss() }.disabled(working != nil || debian.running || arch.running || manager.busy) } }
        }
        .navigationViewStyle(.stack)
        .overlay(alignment: .bottom) { InstallProgressCard() }
        .interactiveDismissDisabled(working != nil || debian.running || arch.running || manager.busy)
        .onAppear(perform: resume)
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
            Section(footer: Text("Debian is recommended. Arch Linux ARM provides a rolling release with pacman and an experimental X11 desktop. Alpine is small and mainly for the terminal. Wayland is not supported.")) {
                Picker("Distribution", selection: $distribution) { Text("Debian GNU/Linux 13 (ARM64) — recommended").tag("debian"); Text("Arch Linux ARM (ARM64) — experimental").tag("arch"); Text("Alpine Linux 3.24 (ARM64)").tag("alpine") }.pickerStyle(.inline).labelsHidden()
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
        let alpineBytes = Int64((try? Bundle.main.url(forResource: "root", withExtension: "tar.gz")?.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        let download = distribution == "debian" ? (manifest?.downloadBytes ?? 0) : 0
        let installed = distribution == "debian" ? (manifest?.installedBytes ?? 0) : alpineBytes * 3
        Section("Storage") {
            row("Download", distribution == "arch" ? "Calculated from the signed package selection" : distribution == "debian" ? ByteCountFormatter.string(fromByteCount: download, countStyle: .file) : "None (included in the app)")
            row(distribution == "debian" ? "Installed size" : "Installed size (estimate)", distribution == "arch" ? "At least 1.5 GB free for base installation" : ByteCountFormatter.string(fromByteCount: installed, countStyle: .file))
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
        } else {
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
        Section(footer: Text("Installs the display, window manager, panel, file manager and sound once (network needed). Existing settings are kept.")) {
            HStack { Image(systemName: "checkmark.circle.fill").foregroundStyle(Color.accentColor); Text(LRRuntimeBridge.activeEnvironmentIsArch() ? "LinuxRunner Desktop (Openbox, LXQt panel, PCManFM-Qt)" : "LinuxRunner Desktop (Openbox, panel, file manager)") }
            HStack { Image(systemName: "circle").foregroundStyle(.secondary); VStack(alignment: .leading) { Text("LXQt · Enlightenment · Wayfire · Hyprland"); Text("Optional · install below once the desktop components are ready, or later in Settings › Desktop Environment").font(.caption).foregroundStyle(.secondary) } }
            HStack { Image(systemName: "circle").foregroundStyle(.secondary); VStack(alignment: .leading) { Text("KDE Plasma"); Text(LRRuntimeBridge.activeEnvironmentIsArch() ? "Unavailable: current Arch Plasma requires Wayland; use the X11 desktop above." : "Experimental · install later in Settings › Desktop Environment").font(.caption).foregroundStyle(.secondary) } }
        }
        Section {
            if desktopReady { Label("Desktop components installed", systemImage: "checkmark") ; Button("Continue") { stepRaw = SetupStep.done.rawValue } }
            else { Button("Install Desktop Components") { prepareDesktop() }.disabled(working != nil) }
        }
        if desktopReady {
            Section(header: Text("Another Desktop (Optional)"), footer: Text("Each comes from the distribution's own packages (network needed; Enlightenment is a large download, about 450 MB on Debian). The one installed last opens with Open Desktop; switch any time in Settings › Desktop Environment.")) {
                extraDesktopRow("LXQt", "lxqt", installed: LRRuntimeBridge.lxqtInstalled()) { LRRuntimeBridge.installLXQt($0, completion: $1) }
                extraDesktopRow("Enlightenment", "enlightenment", installed: LRRuntimeBridge.enlightenmentInstalled()) { LRRuntimeBridge.installEnlightenment($0, completion: $1) }
                if LRRuntimeBridge.activeEnvironmentIsDebian() || LRRuntimeBridge.activeEnvironmentIsArch() {
                    extraDesktopRow("Wayfire", "wayfire", installed: LRRuntimeBridge.wayfireInstalled()) { LRRuntimeBridge.installWayfire($0, completion: $1) }
                }
                if LRRuntimeBridge.activeEnvironmentIsArch() {
                    extraDesktopRow("Hyprland (experimental)", "hyprland", installed: LRRuntimeBridge.hyprlandInstalled()) { LRRuntimeBridge.installHyprland($0, completion: $1) }
                }
            }
        }
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
        guard !debian.running, !arch.running, !manager.busy else { return }
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
        } else {
            manager.error = nil; manager.create(name: "Alpine Linux")
            Task { @MainActor in
                while manager.busy { try? await Task.sleep(nanoseconds: 300_000_000) }
                if let created = manager.environments.first(where: { !known.contains($0.rootfsName) }) { finish(created.rootfsName) }
            }
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

    @ViewBuilder private func extraDesktopRow(_ title: String, _ tag: String, installed: Bool, install: @escaping (@escaping (String) -> Void, @escaping (Error?) -> Void) -> Void) -> some View {
        if installed || extraDesktops.contains(tag) { Label(title + " installed", systemImage: "checkmark") }
        else {
            Button("Install " + title) {
                problem = nil; working = "Starting"
                install({ working = $0 }) { error in
                    working = nil
                    if let error { problem = error.localizedDescription; return }
                    extraDesktops.insert(tag)
                    // Same per-environment choice as Settings › Desktop Environment.
                    UserDefaults.standard.set(tag, forKey: "LinuxRunnerDesktop.environment." + LRRuntimeBridge.activeRootName())
                }
            }.disabled(working != nil)
        }
    }

    private func prepareDesktop() {
        problem = nil; working = "Starting"
        LRRuntimeBridge.prepareDesktopProfile({ working = $0 }) { error in
            working = nil
            if let error { problem = error.localizedDescription } else { desktopReady = true }
        }
    }
}
