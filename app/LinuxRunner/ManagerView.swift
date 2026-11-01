import SwiftUI
import Combine
import UniformTypeIdentifiers
import GameController
import AVFoundation
import Network
import Darwin

@MainActor final class EnvironmentManager: ObservableObject {
    @Published var environments: [LinuxEnvironment] = []
    @Published var storage: [UUID: Int64] = [:]
    @Published var error: String?
    // Environment create/import/remove/install in progress (also keeps the screen awake).
    @Published var busy = false { didSet { if busy != oldValue { busy ? LRRuntimeBridge.beginAwake("environment-operation") : LRRuntimeBridge.endAwake("environment-operation") } } }
    @Published var detected: [LinuxApplication] = []
    @Published var graphical: [LinuxApplication] = []
    @Published var discovering = false
    @Published var applicationIcons: [String: UIImage] = [:]
    @Published var logs: [String] = []
    private var lastStorageMeasurement = Date.distantPast
    private var measuringStorage = false
    private var catalogWritable = true
    private var observers: [NSObjectProtocol] = []

    init() {
        do {
            let url = LRRuntimeBridge.catalogURL()
            if FileManager.default.fileExists(atPath: url.path) {
                environments = try EnvironmentCatalog.read(url).environments
            }
            for root in LRRuntimeBridge.rootNames() where !environments.contains(where: { $0.rootfsName == root }) {
                var recovered = LinuxEnvironment(name: root == "default" ? "Alpine Linux" : "Alpine Linux · \(root.prefix(8))", rootfsName: root)
                if let data = try? Data(contentsOf: LRRuntimeBridge.rootURL(root).appendingPathComponent("environment-install.json")),
                   let metadata = try? JSONSerialization.jsonObject(with: data) as? [String: String] {
                    recovered.distributionID = metadata["distributionID"]; recovered.distribution = metadata["distribution"] ?? recovered.distribution
                    recovered.name = recovered.distribution; recovered.rootfsVersion = metadata["version"] ?? recovered.rootfsVersion
                    recovered.packageManager = metadata["packageManager"]; recovered.defaultShell = metadata["defaultShell"]
                    recovered.installationState = "Imported"
                }
                environments.append(recovered)
            }
            if let index = environments.firstIndex(where: { $0.rootfsName == LRRuntimeBridge.activeRootName() }), LRRuntimeBridge.isRunning() {
                environments[index].lastBootedAt = Date()
            }
            try persist()
        } catch { catalogWritable = false; self.error = error.localizedDescription }
        observers.append(NotificationCenter.default.addObserver(forName: NSNotification.Name("ProcessExitedNotification"), object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        })
        refresh()
    }
    deinit { observers.forEach(NotificationCenter.default.removeObserver) }
    func persist() throws {
        guard catalogWritable else { throw EnvironmentError.invalidMetadata }
        try EnvironmentCatalog(environments: environments).write(LRRuntimeBridge.catalogURL())
    }
    func refresh() {
        detected = LRRuntimeBridge.isRunning() ? LRRuntimeBridge.discoverApplications().compactMap { item in
            guard let name = item["name"], let command = item["command"] else { return nil }
            return LinuxApplication(name: name, command: command)
        } : []
        if !discovering {
            discovering = true
            let root = LRRuntimeBridge.activeRootName()
            DispatchQueue.global(qos: .utility).async { [weak self] in
                let entries = LRRuntimeBridge.isRunning() ? LRRuntimeBridge.discoverGraphicalApplications() : []
                DispatchQueue.main.async {
                    guard let self else { return }
                    self.discovering = false
                    guard root == LRRuntimeBridge.activeRootName() else { return }
                    var icons: [String: UIImage] = [:]
                    self.graphical = entries.compactMap { item in
                        guard let name = item["name"] as? String, let executable = item["executable"] as? String,
                              let args = item["arguments"] as? [String], let directory = item["directory"] as? String else { return nil }
                        var app = LinuxApplication(name: name, command: executable, arguments: args)
                        app.workingDirectory = directory
                        app.genericName = item["genericName"] as? String; app.comment = item["comment"] as? String
                        app.icon = item["icon"] as? String; app.categories = item["categories"] as? [String]
                        app.displayMode = (item["terminal"] as? Bool) == true ? .terminal : .application
                        guard (try? app.validate()) != nil else { return nil }
                        if let data = item["iconPNG"] as? Data, let image = UIImage(data: data), image.size.width <= 512, image.size.height <= 512 {
                            icons[app.launchKey] = image
                        }
                        return app
                    }
                    self.applicationIcons = icons
                }
            }
        }
        logs = LRRuntimeBridge.logLines()
        guard !measuringStorage, Date().timeIntervalSince(lastStorageMeasurement) >= 30 else { return }; measuringStorage = true
        lastStorageMeasurement = Date()
        let items = environments.map { ($0.id, LRRuntimeBridge.rootURL($0.rootfsName), LRRuntimeBridge.exchangeURL($0.rootfsName)) }
        Task.detached(priority: .utility) { [weak self] in
            let measured = EnvironmentStorage.measure(items)
            await self?.setStorage(measured)
        }
    }
    private func setStorage(_ measured: [UUID: Int64]) {
        storage = measured; measuringStorage = false
        for index in environments.indices {
            environments[index].storageBytes = measured[environments[index].id]
            if active(environments[index]) {
                environments[index].lastBootResult = "Runtime running; application acceptance recorded separately"
                if ["debian", "arch"].contains(environments[index].distributionID ?? "") {
                    environments[index].installationState = FileManager.default.fileExists(atPath: LRRuntimeBridge.rootURL(environments[index].rootfsName).appendingPathComponent("data/var/lib/linuxrunner/initialized").path) ? "Base packages configured" : "First-start configuration pending; setup may require repair"
                    let support = LRRuntimeBridge.exchangeURL(environments[index].rootfsName).appendingPathComponent(".linuxrunner/gui")
                    if let profile = try? String(contentsOf: support.appendingPathComponent("profile-check.result"), encoding: .utf8) {
                        let first = profile.components(separatedBy: .newlines).first ?? ""
                        environments[index].desktopAvailability = first == "ready" ? "Components verified; session startup checked separately" : first
                        environments[index].guiAvailability = first == "ready" ? "X11 components verified" : "Desktop profile requires repair"
                    }
                }
            }
        }
        try? persist()
    }

    func acceptInstalled(_ environment: LinuxEnvironment) {
        do { environments.append(environment); try persist(); refresh() } catch { self.error = error.localizedDescription }
    }
    func create(name: String) {
        guard !busy else { return }
        busy = true
        let root = UUID().uuidString.lowercased()
        LRRuntimeBridge.createEnvironment(root) { [weak self] failure in
            Task { @MainActor in
            guard let self else { return }
            self.busy = false
            if let failure { self.error = failure.localizedDescription; return }
            do {
                try FileManager.default.createDirectory(at: LRRuntimeBridge.exchangeURL(root), withIntermediateDirectories: true)
                self.environments.append(LinuxEnvironment(name: name.isEmpty ? "Alpine Linux" : name, rootfsName: root))
                try self.persist()
                self.refresh()
            } catch { self.error = error.localizedDescription }
            }
        }
    }
    func add(application: LinuxApplication, to environment: LinuxEnvironment) {
        do {
            try application.validate()
            guard let index = environments.firstIndex(where: { $0.id == environment.id }) else { return }
            let previous = environments
            environments[index].applications.append(application)
            do { try persist() } catch { environments = previous; throw error }
        } catch { self.error = error.localizedDescription }
    }
    func active(_ environment: LinuxEnvironment) -> Bool {
        environment.rootfsName == LRRuntimeBridge.activeRootName() && LRRuntimeBridge.isRunning()
    }
    func size(_ environment: LinuxEnvironment) -> String {
        storage[environment.id].map { ByteCountFormatter.string(fromByteCount: $0, countStyle: .file) } ?? "Calculating…"
    }
}

private enum EnvironmentStorage {
    static func measure(_ items: [(UUID, URL, URL)]) -> [UUID: Int64] {
        var sizes: [UUID: Int64] = [:]
        for (id, root, exchange) in items {
            let size = StorageUsage.bytes(root) + StorageUsage.bytes(exchange)
            sizes[id] = size
        }
        return sizes
    }
}

@MainActor @objc(LinuxRunnerShell) final class LinuxRunnerShell: NSObject {
    @objc static func makeController(terminal: TerminalViewController) -> UIViewController {
        UIHostingController(rootView: ManagerView(terminal: terminal))
    }
    @objc(makeDesktopSettingsWithController:) static func makeDesktopSettings(controller: LRGUIViewController) -> UIViewController { UIHostingController(rootView: DesktopControlSettings(controller: controller)) }
    @objc(makeDesktopLogsController) static func makeDesktopLogsController() -> UIViewController { UIHostingController(rootView: DesktopLogsScreen()) }
    @objc(makeTaskManagerController) static func makeTaskManagerController() -> UIViewController {
        let host = UIHostingController(rootView: TaskManagerView())
        host.overrideUserInterfaceStyle = .dark
        return host
    }
}

// Password entry stays in the distribution's passwd PTY. Native state contains
// account names only; /etc/shadow and password bytes never enter SwiftUI.
private struct PasswordTarget: Identifiable { let name: String; var id: String { name } }
// Native password change: hashed in the app, set with chpasswd -e.
private struct SetPasswordSheet: View {
    let name: String
    @Environment(\.dismiss) private var dismiss
    @State private var password = ""
    @State private var confirmation = ""
    @State private var working: String?
    @State private var problem: String?
    var body: some View {
        NavigationView {
            Form {
                SecureField("New password", text: $password).textContentType(.newPassword)
                SecureField("Confirm password", text: $confirmation).textContentType(.newPassword)
                if !confirmation.isEmpty && password != confirmation { Text("The passwords do not match.").foregroundStyle(.orange) }
                if let working { HStack(spacing: 10) { ProgressView(); Text(working) } }
                if let problem { Text(problem).foregroundStyle(.red) }
            }
            .navigationTitle("Password for \(name)")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() }.disabled(working != nil) }
                ToolbarItem(placement: .confirmationAction) { Button("Save") {
                    working = "Preparing"; problem = nil
                    LRRuntimeBridge.setLinuxPassword(name, password: password, progress: { working = $0 }) { error in
                        working = nil
                        if let error { problem = error.localizedDescription } else { dismiss() }
                    }
                }.disabled(password.isEmpty || password != confirmation || working != nil) }
            }
        }.navigationViewStyle(.stack)
    }
}

private struct LinuxUsersView: View {
    @State private var accounts: [[String: String]] = []
    @State private var selected = "root"
    @State private var username = ""
    @State private var session: TerminalPresentation?
    @State private var error: String?
    @State private var confirmCreate = false
    @State private var confirmRemove = false
    @State private var removeName = ""
    @State private var newPassword = ""
    @State private var newConfirmation = ""
    @State private var newAdministrator = false
    @State private var creating: String?
    @State private var passwordFor: PasswordTarget?
    private var running: Bool { LRRuntimeBridge.isRunning() }
    private func createNative() {
        error = nil; creating = "Preparing"
        LRRuntimeBridge.createLinuxUser(username, password: newPassword, administrator: newAdministrator, progress: { creating = $0 }) { failure in
            creating = nil
            if let failure { error = failure.localizedDescription; return }
            newPassword = ""; newConfirmation = ""; username = ""; refresh()
        }
    }

    var body: some View {
        List {
            Section("Active Linux Environment") {
                Text(LRRuntimeBridge.activeRootName()).font(.caption).foregroundColor(.secondary)
                Text("New ordinary terminals log in as the selected user using the distribution's su. Existing terminals, managed desktops and administrative/package launches retain their identity.").font(.footnote)
                Text("Linux UID permissions apply inside LinuxRunner. Accounts do not create separate iOS security identities.").font(.footnote).foregroundColor(.secondary)
            }
            Section("Default Linux Session User") {
                Picker("Login account", selection: $selected) {
                    ForEach(accounts, id: \.self) { account in
                        Text("\(account["name"] ?? "") · UID \(account["uid"] ?? "")").tag(account["name"] ?? "")
                    }
                    if !accounts.contains(where: { $0["name"] == selected }) {
                        Text(selected + " (not available)").tag(selected)
                    }
                }
                Button("Use for New Sessions") {
                    do { _ = try LRRuntimeBridge.setDefaultLinuxUser(selected) }
                    catch { self.error = error.localizedDescription }
                }.disabled(!running || !accounts.contains(where: { $0["name"] == selected }))
                Button("Open Login Terminal") {
                    do { session = TerminalPresentation(controller: try LRRuntimeBridge.terminal(forLinuxUser: selected)) }
                    catch { self.error = error.localizedDescription }
                }.disabled(!running || !accounts.contains(where: { $0["name"] == selected }))
            }
            Section("Create Linux User") {
                TextField("Username", text: $username).autocorrectionDisabled().textInputAutocapitalization(.never).textContentType(.username)
                SecureField("Password", text: $newPassword).textContentType(.newPassword)
                SecureField("Confirm password", text: $newConfirmation).textContentType(.newPassword)
                Toggle("Administrator (can use sudo)", isOn: $newAdministrator)
                Button("Create Account") { createNative() }
                    .disabled(!running || creating != nil || username.isEmpty || newPassword.isEmpty || newPassword != newConfirmation)
                if let creating { HStack(spacing: 10) { ProgressView(); Text(creating) } }
                Text("Account tools (and sudo for administrators) are installed automatically if this environment lacks them. LinuxRunner sends only Linux's own password hash and stores no password.").font(.caption).foregroundColor(.secondary)
            }
            Section("Manage Accounts") {
                ForEach(accounts, id: \.self) { account in
                    VStack(alignment: .leading, spacing: 6) {
                        Text(account["name"] ?? "").font(.headline)
                        Text("UID \(account["uid"] ?? "") · GID \(account["gid"] ?? "") · \(account["home"] ?? "")").font(.caption)
                        Button("Set Password…") { passwordFor = PasswordTarget(name: account["name"] ?? "") }.disabled(!running)
                        if (Int(account["uid"] ?? "") ?? 0) >= 1000 {
                            Button("Remove Account — Keep Home…", role: .destructive) {
                                removeName = account["name"] ?? ""
                                confirmRemove = true
                            }.disabled(!running || account["name"] == LRRuntimeBridge.defaultLinuxUser())
                        }
                    }
                }
                Button("Refresh Accounts", action: refresh).disabled(!running)
                Text("System users without a login shell are omitted. Removing an account preserves its home and files; their numeric ownership remains. System accounts and the default login are protected.").font(.caption).foregroundColor(.secondary)
            }
            if let error { Section("Account Operation") { Text(error).foregroundColor(.red) } }
        }
        .navigationTitle("Linux Users")
        .onAppear(perform: refresh)
        .sheet(item: $passwordFor) { target in SetPasswordSheet(name: target.name) }
        .confirmationDialog("Create \(username) in the active Linux environment?", isPresented: $confirmCreate, titleVisibility: .visible) {
            Button("Create User") {
                do { session = TerminalPresentation(controller: try LRRuntimeBridge.createLinuxUser(username)) }
                catch { self.error = error.localizedDescription }
            }
        }
        .confirmationDialog("Remove \(removeName) and keep its home?", isPresented: $confirmRemove, titleVisibility: .visible) {
            Button("Remove Account — Keep Home", role: .destructive) {
                do { session = TerminalPresentation(controller: try LRRuntimeBridge.removeLinuxUserKeepingHome(removeName)) }
                catch { self.error = error.localizedDescription }
            }
        } message: {
            Text("The distribution updates its account databases. Home directories and files remain, with their existing numeric ownership. Close this user's sessions first. No forced deletion is used.")
        }
        .sheet(item: $session, onDismiss: refresh) { item in
            TerminalScreen(controller: item.controller)
        }
    }
    private func refresh() {
        accounts = LRRuntimeBridge.linuxAccounts()
        selected = LRRuntimeBridge.defaultLinuxUser()
    }
}

private struct TerminalPresentation: Identifiable {
    let id = UUID()
    let controller: TerminalViewController
}

struct ManagerView: View {
    @StateObject private var manager = EnvironmentManager()
    let terminal: TerminalViewController
    @State private var selectedTab = 0
    @State private var desktopSummary: [String: Any] = [:]
    @State private var confirmStopDesktop = false
    @State private var startingRuntime = false
    @State private var autoStartRuntime = LRRuntimeBridge.autoStart()
    @AppStorage("LinuxRunner.developerMode") private var developerMode = false
    @AppStorage("LinuxRunner.keepScreenAwake") private var keepScreenAwake = true
    @AppStorage("LinuxRunner.experimentalMemoryPCFusion") private var memoryPCFusion = true
    @AppStorage("LinuxRunner.appearance") private var appearance = "dark"
    private let sessionTimer = Timer.publish(every: 3, on: .main, in: .common).autoconnect()
    /// What the full-screen graphical cover shows. The request is the cover's
    /// item, so its content receives the request itself: separate @State
    /// flags read only inside the content closure were captured stale, and
    /// Open Desktop showed the Standalone GUI Test (IPA 889–891 device reports).
    @State private var graphical: GraphicalRequest?
    @State private var desktopAfterSetup = false
    @State private var setup = false
    @AppStorage("LinuxRunnerSetup.step") private var setupStep = "welcome"
    @State private var diagnostics = false
    @State private var desktopSettings = false
    @State private var settings = false
    @State private var importer = false
    @State private var session: TerminalPresentation?
    @State private var share: ExportItem?
    @State private var importRoot: String?
    @State private var selectedEnvironment: LinuxEnvironment?
    @State private var showDetail = false
    @State private var showFiles = false
    @Environment(\.scenePhase) private var phase

    var body: some View {
        TabView(selection: $selectedTab) {
            NavigationView { home }.navigationViewStyle(.stack)
                .tabItem { Label("Home", systemImage: "house") }.tag(0)
            NavigationView { distributions }.navigationViewStyle(.stack)
                .tabItem { Label("Distributions", systemImage: "square.stack.3d.up") }.tag(1)
            NavigationView { sessions }.navigationViewStyle(.stack)
                .tabItem { Label("Sessions", systemImage: "rectangle.on.rectangle") }.tag(2)
            NavigationView { preferences }.navigationViewStyle(.stack)
                .tabItem { Label("Settings", systemImage: "slider.horizontal.3") }.tag(3)
        }
        .overlay(alignment: .bottom) { InstallProgressCard() }
        .accentColor(Color(red: 0.36, green: 0.68, blue: 0.98))
        .preferredColorScheme(appearance == "system" ? nil : appearance == "light" ? .light : .dark)
        .onAppear {
            desktopSummary = LRRuntimeBridge.desktopSessionSummary()
            // First launch without Linux, or setup waiting for its relaunch/account step.
            // Once per launch, so "Later" is respected.
            // Only when there is no Linux at all, or a setup the user started is
            // waiting for its restart; never for an existing, usable installation.
            if !SetupAutoPresentation.shown && (manager.environments.isEmpty || (setupStep == "relaunch" && LRRuntimeBridge.isRunning() && LRRuntimeBridge.activeRootName() == UserDefaults.standard.string(forKey: "LinuxRunnerSetup.root"))) { SetupAutoPresentation.shown = true; setup = true }
        }
        #if targetEnvironment(simulator)
        .onAppear { SimulatorWorkflow.start(desktop: openDesktop, guiTest: openGUITest, manager: manager,
                                            terminal: { if let environment = activeEnvironment { openTerminal(environment) } },
                                            type: { terminal.sendSessionInput($0) }, tab: { selectedTab = $0 }) }
        #endif
        // Opening the desktop from the wizard waits until the wizard's cover is
        // gone: a cover presented while another is dismissing is dropped.
        .fullScreenCover(isPresented: $setup, onDismiss: { if desktopAfterSetup { desktopAfterSetup = false; openDesktop() } }) {
            SetupWizardView(manager: manager, openDesktop: { desktopAfterSetup = true })
        }
        .onReceive(sessionTimer) { _ in if phase == .active { desktopSummary = LRRuntimeBridge.desktopSessionSummary() } }
        .alert("Stop Desktop?", isPresented: $confirmStopDesktop) {
            Button("Cancel", role: .cancel) { }
            Button("Stop", role: .destructive) { LRRuntimeBridge.stopDesktopSession(); desktopSummary = LRRuntimeBridge.desktopSessionSummary() }
        } message: { Text("Save documents first. This closes applications owned by the desktop; persistent files and the terminal environment remain.") }
        .fullScreenCover(item: $graphical) { request in NativeController(controller: presentedController(request)) }
        .sheet(isPresented: $diagnostics) { DeveloperDiagnosticsView() }
        .sheet(isPresented: $desktopSettings) {
            if let navigation = LRRuntimeBridge.desktopController() as? UINavigationController,
               let viewer = navigation.viewControllers.first as? LRGUIViewController {
                DesktopControlSettings(controller: viewer, openDesktop: openDesktop)
            }
        }
        .fullScreenCover(item: $session) { item in
            TerminalScreen(controller: item.controller)
        }
        .sheet(item: $share) { item in ShareView(item: item) }
        .fileImporter(isPresented: $importer, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
            guard let root = importRoot else { return }
            transfer(result: result, root: root)
        }
        .alert("LinuxRunner", isPresented: Binding(get: { manager.error != nil }, set: { if !$0 { DispatchQueue.main.async { manager.error = nil } } })) {
            Button("OK") { manager.error = nil }
        } message: { Text(manager.error ?? "") }
        .onChange(of: phase) { _ in manager.refresh() }
    }
    private var activeEnvironment: LinuxEnvironment? { manager.environments.first(where: manager.active) }
    @AppStorage("LinuxRunnerGraphics.mode") private var graphicsMode = "mesa"
    /// Requested renderer, refined by what the latest session actually reported.
    private var graphicsStatus: String {
        // The Linux GPU device (Metal, virgl) when this run actually has it.
        if LRRuntimeBridge.virglActive() { return "Metal GPU · Experimental" }
        let requested = graphicsMode == "mesa-metal-resident" ? "GPU-Resident Metal" : graphicsMode == "mesa-metal-verify" ? "Metal Verification" : "Software"
        guard let routing = LRRuntimeBridge.graphicsSummary()["routing"] as? String else { return requested }
        if routing.contains("reason=") { return requested + " · fell back to software" }
        return routing.contains("active=mesa ") || routing.hasSuffix("active=mesa") ? "Software" : requested + " · active"
    }
    private func statusRow(_ title: String, systemImage: String, value: String, color: Color) -> some View {
        HStack { Label(title, systemImage: systemImage); Spacer(); Text(value).font(.callout).foregroundColor(color).lineLimit(1) }
    }
    private var desktopAlive: Bool { desktopSummary["alive"] as? Bool == true }
    private var recentApplications: [LinuxApplication] {
        let keys = UserDefaults.standard.stringArray(forKey: "LinuxRunner.launcher." + LRRuntimeBridge.activeRootName() + ".recent") ?? []
        return keys.prefix(6).compactMap { key in manager.graphical.first { $0.launchKey == key } }
    }
    private func openDesktop() { _ = LRRuntimeBridge.desktopTraceBegin("open-desktop button"); graphical = GraphicalRequest(desktop: true, launch: nil) }
    private func openGUITest() { LRRuntimeBridge.desktopTrace("gui-test", result: "info", detail: "Standalone GUI Test opened"); graphical = GraphicalRequest(desktop: false, launch: nil) }
    /// Built when the full-screen cover presents; records which controller the
    /// presentation actually received (desktop or GUI Test) for the timeline.
    private func presentedController(_ request: GraphicalRequest) -> UIViewController {
        // SwiftUI rebuilds the content on every refresh; record each request once.
        if GraphicalRequest.lastTraced != request.id {
            GraphicalRequest.lastTraced = request.id
            LRRuntimeBridge.desktopTrace("02 view", result: "info", detail: "full-screen content built desktop=\(request.desktop) application=\(request.launch != nil)")
        }
        return request.desktop ? (request.launch.map { LRRuntimeBridge.desktopController(withConfiguration: $0) } ?? LRRuntimeBridge.desktopController()) : LRRuntimeBridge.guiController()
    }
    private func startRuntime() {
        guard !startingRuntime, LRRuntimeBridge.canStartRuntime() else { return }
        startingRuntime = true
        // Yield once so the native loading state can be drawn before the
        // existing serialized boot path initializes the runtime.
        DispatchQueue.main.async {
            let result = LRRuntimeBridge.startRuntime()
            if LRRuntimeBridge.isRunning() {
                if terminal.sessionTerminalUUID == nil {
                    terminal.loadViewIfNeeded()
                    terminal.startNewSession()
                }
            } else {
                manager.error = "Runtime initialization failed (\(result)). " + LRRuntimeBridge.runtimeStatus()
            }
            startingRuntime = false
            desktopSummary = LRRuntimeBridge.desktopSessionSummary()
            manager.refresh()
        }
    }
    private var home: some View {
        List {
            Section {
                VStack(alignment: .leading, spacing: 10) {
                    Text("LinuxRunner").font(.system(size: 34, weight: .bold, design: .rounded)).tracking(-1)
                    Text("Your Linux workspace, on iPhone.").font(.subheadline).foregroundColor(.secondary)
                    HStack(spacing: 7) {
                        Circle().fill(LRRuntimeBridge.isRunning() ? Color.green : Color.orange).frame(width: 6, height: 6)
                        Text(activeEnvironment?.name ?? LRRuntimeBridge.runtimeStatus()).font(.caption)
                        Spacer()
                        Text("ARM64 · Interpreter").font(.caption2).foregroundColor(.secondary)
                    }.padding(.top, 8)
                    if !LRRuntimeBridge.isRunning() {
                        Button(action: startRuntime) {
                            HStack { if startingRuntime { ProgressView().tint(.white) } else { Image(systemName: "power") }; Text(startingRuntime ? "Starting Runtime…" : "Start Linux Runtime").fontWeight(.semibold); Spacer(); Image(systemName: "arrow.right") }
                                .padding(16).foregroundColor(.white).background(Color(red: 0.12, green: 0.30, blue: 0.52)).cornerRadius(12)
                        }.buttonStyle(.plain).disabled(startingRuntime || !LRRuntimeBridge.canStartRuntime()).accessibilityIdentifier("Start Linux Runtime")
                    } else { Button(action: openDesktop) {
                        HStack { Image(systemName: "desktopcomputer"); Text(desktopAlive ? "Resume Desktop" : "Open Desktop").fontWeight(.semibold); Spacer(); Image(systemName: "arrow.up.right") }
                            .padding(16).foregroundColor(.white).background(Color(red: 0.12, green: 0.30, blue: 0.52)).cornerRadius(12)
                    }.buttonStyle(.plain).accessibilityIdentifier("Open Desktop") }
                }.padding(.vertical, 12)
            }
            if LRRuntimeBridge.isRunning() {
                Section("Status") {
                    statusRow("Runtime", systemImage: "cpu", value: activeEnvironment?.name ?? "Running", color: .green)
                    statusRow("Desktop", systemImage: "desktopcomputer", value: desktopSummary["state"] as? String ?? "Stopped", color: desktopAlive ? .green : .secondary)
                    NavigationLink(destination: GraphicsSettingsView()) { statusRow("Graphics", systemImage: "square.3.layers.3d", value: graphicsStatus, color: LRRuntimeBridge.virglActive() ? .green : graphicsMode == "mesa" ? .secondary : .blue) }
                }
            }
            Section("Workspace") {
                NavigationLink(destination: ApplicationGridView(manager: manager, run: run)) { Label("Applications", systemImage: "square.grid.2x2") }
                if let environment = activeEnvironment {
                    NavigationLink(destination: PackageView(environment: environment, enabled: manager.active(environment), run: run, refreshApplications: manager.refresh)) { Label("App Manager", systemImage: "bag") }
                    Button { openTerminal(environment) } label: { Label("Terminal", systemImage: "terminal") }
                    NavigationLink(destination: files(environment)) { Label("Files", systemImage: "folder") }
                }
            }
            Section("Recent Application Crashes") {
                NavigationLink(destination: GuestCrashListView()) { Label("View Crash Reports", systemImage: "exclamationmark.bubble") }
            }
            if !recentApplications.isEmpty {
                Section("Recently Used") { ForEach(recentApplications) { app in Button { run(app) } label: {
                    HStack { if let image = manager.applicationIcons[app.launchKey] { Image(uiImage: image).resizable().scaledToFit().frame(width: 28, height: 28) } else { Image(systemName: "app").frame(width: 28) }; Text(app.name); Spacer(); Image(systemName: "arrow.up.right").font(.caption).foregroundColor(.secondary) }
                } } }
            }
            Section("Installed Distributions") { ForEach(manager.environments) { environment in
                HStack { Image(systemName: "shippingbox").foregroundColor(.accentColor); VStack(alignment: .leading, spacing: 4) { Text(environment.name); Text(manager.size(environment)).font(.caption).foregroundColor(.secondary) }; Spacer(); Text(manager.active(environment) ? "Running" : "Installed").font(.caption).foregroundColor(.secondary) }
            } }
            if !LRRuntimeBridge.isRunning() { Section { Text(LRRuntimeBridge.runtimeStatus()).foregroundColor(.orange) } }
        }.listStyle(.insetGrouped).navigationTitle("Home").navigationBarTitleDisplayMode(.inline)
            .toolbar { Button { manager.refresh() } label: { Image(systemName: "arrow.clockwise") }.accessibilityLabel("Refresh") }
    }
    private var distributions: some View {
        List {
            Section("Installed") { ForEach(manager.environments) { environment in
                NavigationLink(destination: environmentDetail(environment)) {
                    HStack(spacing: 14) { Image(systemName: "shippingbox.fill").font(.title2).foregroundColor(.accentColor)
                        VStack(alignment: .leading, spacing: 5) { Text(environment.name).font(.headline); Text("ARM64 · " + manager.size(environment)).font(.caption).foregroundColor(.secondary)
                            if LRRuntimeBridge.nextRootName() == environment.rootfsName && !manager.active(environment) { Text("Selected for next launch").font(.caption).foregroundColor(.accentColor) }
                        }; Spacer(); if manager.active(environment) { Image(systemName: "circle.fill").font(.system(size: 7)).foregroundColor(.green) }
                    }.padding(.vertical, 6)
                }
            } }
            Section("Add Environment") {
                Button { setupStep = "welcome"; setup = true } label: { Label("Set Up Linux…", systemImage: "sparkles") }
                if manager.busy { ProgressView("Preparing environment…") }
            }
            Section { Text("Each distribution retains its own packages and files. One runtime is active at a time; switching takes effect after relaunch.").font(.footnote).foregroundColor(.secondary) }
        }.listStyle(.insetGrouped).navigationTitle("Distributions")
            .toolbar { Button { manager.refresh() } label: { Image(systemName: "arrow.clockwise") }.accessibilityLabel("Refresh Distributions") }
    }
    private var sessions: some View {
        List {
            Section("Desktop") {
                Label(desktopSummary["state"] as? String ?? "Stopped", systemImage: "desktopcomputer").font(.headline)
                if desktopAlive {
                    Text("Profile: " + (activeEnvironment?.name ?? LRRuntimeBridge.activeRootName())).font(.subheadline)
                    if let user = desktopSummary["user"] as? String {
                        Text("Desktop user: " + user).font(.subheadline)
                        if user != LRRuntimeBridge.defaultLinuxUser() { Text("The selected login changed. Save work and stop this desktop before launching applications under the new user.").font(.footnote).foregroundColor(.orange) }
                        if user == "root" { Text("This session runs as root. Applications that require a normal user will refuse to start. Choose a Linux user in Settings, then start a new desktop.").font(.footnote).foregroundColor(.orange) }
                    }
                    if let geometry = desktopSummary["geometry"] as? String { Text("Display: " + geometry).font(.caption).foregroundColor(.secondary) }
                    Text(desktopSummary["connected"] as? Bool == true ? "Viewer connected" : "Viewer detached; desktop retained").font(.caption).foregroundColor(.secondary)
                    Button("Resume Desktop", action: openDesktop)
                    Button("Stop Desktop", role: .destructive) { confirmStopDesktop = true }
                } else { Button("Open Desktop", action: openDesktop).disabled(!LRRuntimeBridge.isRunning()) }
                Text("Navigating away retains windows. iOS may suspend or terminate the app; saved files remain persistent.").font(.footnote).foregroundColor(.secondary)
            }
            Section("Runtime") { Text(LRRuntimeBridge.runtimeStatus()); if let environment = activeEnvironment { Button("Open Terminal") { openTerminal(environment) } } }
            if developerMode { Section("Inspection") { Button("Runtime Diagnostics") { diagnostics = true }; Button("Standalone GUI Test", action: openGUITest) } }
        }.listStyle(.insetGrouped).navigationTitle("Sessions")
    }
    private var preferences: some View {
        List {
            Section("Appearance") { Picker(selection: $appearance) { Text("Dark").tag("dark"); Text("Light").tag("light"); Text("System").tag("system") } label: { Label("Native Theme", systemImage: "circle.lefthalf.filled") } }
            Section {
                Toggle(isOn: $keepScreenAwake) { Label("Keep Screen Awake During Operations", systemImage: "sun.max") }
            } header: { Text("General") } footer: {
                Text("Prevents automatic screen locking while LinuxRunner installs Linux, desktop components, account tools or packages, and while a terminal command runs. iOS pauses apps in the background, so keep LinuxRunner open in the foreground during long operations.")
            }
            Section("Desktop & Input") {
                Button { desktopSettings = true } label: { Label("Desktop Settings", systemImage: "display") }
                NavigationLink(destination: GraphicsSettingsView()) { Label("Graphics", systemImage: "cpu") }
                NavigationLink(destination: DesktopEnvironmentView()) { Label("Desktop Environment", systemImage: "macwindow.on.rectangle") }
                NavigationLink(destination: AudioSettingsView()) { Label("Audio", systemImage: "speaker.wave.2") }
                NavigationLink(destination: DesktopPreferencesView()) { Label("Display and Gestures", systemImage: "hand.tap") }
                NavigationLink(destination: ApplicationGridView(manager: manager, run: run)) { Label("Application Launcher", systemImage: "square.grid.2x2") }
            }
            Section("Files & Devices") {
                NavigationLink(destination: FileSharingView(environment: activeEnvironment, manager: manager, share: $share)) { Label("File Sharing", systemImage: "folder") }
                NavigationLink(destination: DevicesView()) { Label("Devices", systemImage: "cable.connector") }
                NavigationLink(destination: NetworkDiagnosticsView()) { Label("Network", systemImage: "network") }
            }
            Section("Environment") {
                NavigationLink(destination: LinuxUsersView()) { Label("Linux Users", systemImage: "person.2") }
                NavigationLink(destination: StorageManagementView(environments: manager.environments)) { Label("Storage Management", systemImage: "internaldrive") }
            }
            Section("Advanced") {
                NavigationLink(destination: TerminalSettingsView()) { Label("Terminal & Keyboard", systemImage: "keyboard") }
                Button { LRRuntimeBridge.exportDesktopReport { url in if let url { share = ExportItem(url: url) } } } label: { Label("Export Open Desktop Report", systemImage: "doc.text.magnifyingglass") }
                Toggle(isOn: $developerMode) { Label("Developer Mode", systemImage: "hammer") }
                if developerMode {
                    Toggle(isOn: $autoStartRuntime) { Label("Start Runtime on Launch", systemImage: "power") }.onChange(of: autoStartRuntime) { UserDefaults.standard.set($0, forKey: "LinuxRunnerRuntimeAutoStart") }
                    Button(action: openGUITest) { Label("Standalone GUI Test", systemImage: "rectangle.dashed") }.disabled(!LRRuntimeBridge.isRunning())
                    Button { diagnostics = true } label: { Label("Runtime Diagnostics", systemImage: "stethoscope") }
                    NavigationLink(destination: ExecutionEngineView()) { Label("Execution Engine / StikDebug", systemImage: "bolt") }
                    Toggle(isOn: $memoryPCFusion) { Label("Memory Dispatch Fusion", systemImage: "memorychip") }
                    Text("On by default from build 884. Applies after fully closing and reopening LinuxRunner. Uses the same Asbestos engine with fewer dispatch steps per memory access (Mac: Python and Node startup 4-5% faster, ABI suites and browser tests pass). Turn off to restore the previous memory dispatch path if an app misbehaves.").font(.caption)
                }
            }
            Section("About") { NavigationLink(destination: LinuxRunnerAboutView()) { Label("About LinuxRunner", systemImage: "info.circle") } }
        }.listStyle(.insetGrouped).navigationTitle("Settings")
    }
    private func openTerminal(_ environment: LinuxEnvironment) {
        guard manager.active(environment) else { return }
        if terminal.sessionTerminalUUID == nil { terminal.loadViewIfNeeded(); terminal.startNewSession() }
        session = TerminalPresentation(controller: terminal)
    }
    private func run(_ application: LinuxApplication) {
        do {
            try application.validate()
            guard LRRuntimeBridge.isRunning() else { manager.error = LRRuntimeBridge.runtimeStatus(); return }
            if application.displayMode == .application {
                _ = LRRuntimeBridge.desktopTraceBegin("application launcher")
                let launch: [String: Any] = ["requestID": UUID().uuidString, "executable": application.command, "arguments": application.arguments,
                    "directory": application.workingDirectory, "environment": application.environment ?? [:]]
                graphical = GraphicalRequest(desktop: true, launch: launch)
                return
            }
            let controller = LRRuntimeBridge.terminal(forCommand:[application.command] + application.arguments, directory: application.workingDirectory)
            session = TerminalPresentation(controller: controller)
        } catch { manager.error = error.localizedDescription }
    }
    private func environmentDetail(_ environment: LinuxEnvironment) -> some View {
        List {
            Section("Environment") {
                Text("\(environment.distribution) \(environment.rootfsVersion) · ARM64")
                Text(manager.size(environment))
                Text("Home: \(environment.homePath)")
                if let state = environment.installationState { Text("Installation: \(state)") }
                if let result = environment.lastBootResult { Text("Boot result: \(result)") }
                if let gui = environment.guiAvailability { Text("GUI: \(gui)") }
                if let desktop = environment.desktopAvailability { Text("Desktop: \(desktop)") }
                Text("Created: \(environment.createdAt.formatted())")
                if let last = environment.lastBootedAt { Text("Last boot: \(last.formatted())") }
                Button("Open Terminal") { openTerminal(environment) }.disabled(!manager.active(environment))
                Button("Set Up Linux…") {
                    UserDefaults.standard.set(environment.distributionID ?? "alpine", forKey: "LinuxRunnerSetup.distribution")
                    UserDefaults.standard.set(environment.rootfsName, forKey: "LinuxRunnerSetup.root")
                    LRRuntimeBridge.setNextRootName(environment.rootfsName)
                    setupStep = manager.active(environment) ? "account" : "relaunch"
                    setup = true
                }
                NavigationLink("Application Launcher", destination: ApplicationGridView(manager: manager, run: run))
                Text("Start GUI support here before running X11 apps. DISPLAY is supplied by the managed session, not a global terminal variable.").font(.caption)
                if !manager.active(environment) {
                    Button("Use on Next Launch") {
                        LRRuntimeBridge.setNextRootName(environment.rootfsName)
                        manager.error = "Environment selected. Close LinuxRunner from the app switcher and open it again. Files are preserved."
                    }
                }
                NavigationLink("Files", destination: files(environment))
            }
            Section("Applications") {
                ForEach(manager.environments.first(where: { $0.id == environment.id })?.applications ?? []) { app in
                    VStack(alignment: .leading) {
                        Text(app.name).font(.headline)
                        Text(([app.command] + app.arguments).joined(separator: " ")).font(.caption).textSelection(.enabled)
                        Button("Run") { run(app) }.disabled(!manager.active(environment)).accessibilityIdentifier("run-\(app.name)")
                    }
                }
                NavigationLink("Create Launcher", destination: LauncherEditor { manager.add(application: $0, to: environment) })
            }
            if manager.active(environment) && !manager.detected.isEmpty {
                Section("Discovered Applications") {
                    ForEach(manager.detected) { app in
                        Button(app.name) { run(app) }
                    }
                }
            }
            if manager.active(environment) && !manager.graphical.isEmpty {
                Section("Installed graphical applications") {
                    Text("Discovered from this environment's desktop entries. Listed applications have not all been compatibility-tested.").font(.caption)
                    ForEach(manager.graphical) { app in Button { run(app) } label: { VStack(alignment: .leading) { Text(app.name); if let comment = app.comment, !comment.isEmpty { Text(comment).font(.caption) } } } }
                    Button("Refresh Applications") { manager.refresh() }
                }
            }
            Section("App Manager") {
                NavigationLink("Install, Manage and Repair Applications", destination: PackageView(environment: environment, enabled: manager.active(environment), run: run, refreshApplications: manager.refresh))
            }
        }.navigationTitle(environment.name)
    }
    private func files(_ environment: LinuxEnvironment) -> some View {
        ExchangeFilesView(environment: environment, manager: manager, share: $share)
    }
    private func transfer(result: Result<[URL], Error>, root: String) {
        do {
            guard let source = try result.get().first else { return }
            manager.busy = true
            Task.detached {
                do { try LRRuntimeBridge.importSharedFile(source, relative: "", root: root, control: LRTransferControl()) }
                catch { await MainActor.run { manager.error = error.localizedDescription } }
                await MainActor.run { manager.busy = false; manager.refresh() }
            }
        } catch { manager.error = error.localizedDescription }
    }
}

struct GraphicalRequest: Identifiable {
    static var lastTraced: UUID?
    let id = UUID()
    let desktop: Bool
    let launch: [String: Any]?
}

struct NativeController: UIViewControllerRepresentable {
    let controller: UIViewController
    func makeUIViewController(context: Context) -> UIViewController { controller }
    func updateUIViewController(_ uiViewController: UIViewController, context: Context) {}
}

struct TerminalScreen: View {
    let controller: TerminalViewController
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        // The header and status-bar area continue the terminal theme's surface.
        let surface = Color(uiColor: { controller.loadViewIfNeeded(); return controller.view.backgroundColor ?? .black }())
        VStack(spacing: 0) {
            HStack {
                Text("Terminal").font(.headline)
                Spacer()
                Button("Done") { dismiss() }
            }.padding(.horizontal).padding(.vertical, 8)
             .environment(\.colorScheme, UserPreferences.shared().keyboardAppearance == .light ? .light : .dark)
            NativeController(controller: controller)
        }.background(surface.ignoresSafeArea())
         .onAppear { currentTerminalViewController = controller }
         .onDisappear {
             if currentTerminalViewController === controller { currentTerminalViewController = nil }
         }
    }
}

enum SetupAutoPresentation { static var shown = false }

struct CreateEnvironmentView: View {
    var create: (String) -> Void
    @State private var name = "Alpine Linux"
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationView {
            Form {
                TextField("Environment name", text: $name)
                Text("Alpine 3.24.2 ARM64 from the verified bundled image. Debian and Ubuntu support is planned.")
                Button("Create Environment") { create(name.trimmingCharacters(in: .whitespacesAndNewlines)); dismiss() }
            }.navigationTitle("New Environment").toolbar { Button("Cancel") { dismiss() } }
        }.navigationViewStyle(.stack)
    }
}

struct LauncherEditor: View {
    var save: (LinuxApplication) -> Void
    @State private var name = ""
    @State private var command = "/usr/bin/python3"
    @State private var arguments = "[]"
    @State private var directory = "/root"
    @State private var displayMode = LinuxDisplayMode.terminal
    @State private var environment = "{}"
    @State private var graphicsMode = ""
    @State private var error: String?
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        Form {
            TextField("Name", text: $name)
            TextField("Absolute executable", text: $command).autocorrectionDisabled().textInputAutocapitalization(.never)
            TextField("Arguments as a JSON array", text: $arguments).autocorrectionDisabled().textInputAutocapitalization(.never)
            Text("Example: [\"--version\"]. Each string is one argument; shell expansion is not applied.").font(.caption)
            TextField("Working directory", text: $directory).autocorrectionDisabled().textInputAutocapitalization(.never)
            Picker("Open in", selection: $displayMode) {
                Text("Native Terminal").tag(LinuxDisplayMode.terminal)
                Text("Linux GUI viewer").tag(LinuxDisplayMode.application)
            }
            if displayMode == .application {
                TextField("Environment as JSON object", text: $environment).autocorrectionDisabled().textInputAutocapitalization(.never)
                Text("DISPLAY and Xauthority are set for each GUI session. Install GUI Support before launching X11 applications.").font(.caption)
                Picker("Renderer", selection: $graphicsMode) {
                    Text("Settings Default").tag("")
                    Text("Software (Mesa)").tag("software")
                    Text("GPU-Resident Metal · Experimental").tag("mesa-metal-resident")
                    Text("Metal Verification · Diagnostics").tag("mesa-metal-verify")
                }
                Text("Settings Default follows Settings › Graphics. GPU modes require Debian; unsupported draws remain software, and a GPU mode that cannot start falls back to software rendering.").font(.caption)
            }
            Button("Save Launcher") {
                do {
                    let args = try JSONDecoder().decode([String].self, from: Data(arguments.utf8))
                    var app = LinuxApplication(name: name, command: command, arguments: args, workingDirectory: directory)
                    app.displayMode = displayMode
                    if displayMode == .application {
                        app.environment = try JSONDecoder().decode([String: String].self, from: Data(environment.utf8))
                        var launchEnvironment = app.environment ?? [:]
                        // Empty: follow the session default chosen in Settings › Graphics.
                        launchEnvironment["LR_GUI_GPU_MODE"] = graphicsMode.isEmpty ? nil : graphicsMode
                        app.environment = launchEnvironment
                    }
                    try app.validate(); save(app); dismiss()
                } catch { self.error = error.localizedDescription }
            }
            if let error { Text(error).foregroundColor(.red) }
        }.navigationTitle("Create Launcher")
    }
}

/// Curated applications with their real Debian package names. Status reflects recorded
/// test evidence only (docs/reports); nothing here is advertised beyond what was tested.
struct CatalogApplication: Identifiable {
    enum Evidence: String { case verified = "Verified on Mac", partial = "Partial", experimental = "Experimental", untested = "Untested"
        var color: Color { switch self { case .verified: return .green; case .partial: return .orange; case .experimental: return .blue; case .untested: return .secondary } } }
    let package: String, name: String, executable: String, category: String, summary: String, evidence: Evidence, note: String
    var id: String { package }
    static let debian: [CatalogApplication] = [
        .init(package: "blender", name: "Blender", executable: "blender", category: "Graphics", summary: "3D creation suite", evidence: .experimental, note: "Launches on iPhone 15 Pro (first start takes minutes). Try Settings › Graphics › GPU-Resident Metal."),
        .init(package: "firefox-esr", name: "Firefox ESR", executable: "firefox-esr", category: "Internet", summary: "Web browser", evidence: .experimental, note: "Multi-tab browsing passes on Mac; occasional shutdown abort inside Firefox. Software rendering."),
        .init(package: "geany", name: "Geany", executable: "geany", category: "Development", summary: "Lightweight IDE", evidence: .verified, note: "Opens, edits and saves in the automated Mac suite."),
        .init(package: "mousepad", name: "Mousepad", executable: "mousepad", category: "Accessories", summary: "Text editor", evidence: .verified, note: ""),
        .init(package: "featherpad", name: "FeatherPad", executable: "featherpad", category: "Accessories", summary: "Qt text editor", evidence: .verified, note: ""),
        .init(package: "xfce4-terminal", name: "Xfce Terminal", executable: "xfce4-terminal", category: "System", summary: "Terminal emulator", evidence: .verified, note: ""),
        .init(package: "thunar", name: "Thunar", executable: "thunar", category: "System", summary: "File manager", evidence: .verified, note: ""),
        .init(package: "gnumeric", name: "Gnumeric", executable: "gnumeric", category: "Office", summary: "Spreadsheet", evidence: .verified, note: ""),
        .init(package: "abiword", name: "AbiWord", executable: "abiword", category: "Office", summary: "Word processor", evidence: .partial, note: "Starts and edits; quitting is not fully verified."),
        .init(package: "zathura", name: "Zathura", executable: "zathura", category: "Office", summary: "Document viewer", evidence: .verified, note: ""),
        .init(package: "qpdfview", name: "qpdfview", executable: "qpdfview", category: "Office", summary: "PDF viewer", evidence: .verified, note: ""),
        .init(package: "qalculate-qt", name: "Qalculate!", executable: "qalculate-qt", category: "Accessories", summary: "Calculator", evidence: .verified, note: ""),
        .init(package: "galculator", name: "Galculator", executable: "galculator", category: "Accessories", summary: "Calculator", evidence: .partial, note: "Works; closing the window is not fully verified."),
        .init(package: "ristretto", name: "Ristretto", executable: "ristretto", category: "Graphics", summary: "Image viewer", evidence: .partial, note: "Works; closing the window is not fully verified."),
        .init(package: "feh", name: "feh", executable: "feh", category: "Graphics", summary: "Image viewer", evidence: .verified, note: ""),
        .init(package: "libreoffice", name: "LibreOffice", executable: "libreoffice", category: "Office", summary: "Office suite", evidence: .untested, note: "Large install; not covered by automated tests yet."),
        .init(package: "gimp", name: "GIMP", executable: "gimp", category: "Graphics", summary: "Image editor", evidence: .untested, note: "Not covered by automated tests yet."),
        .init(package: "inkscape", name: "Inkscape", executable: "inkscape", category: "Graphics", summary: "Vector graphics editor", evidence: .untested, note: "Not covered by automated tests yet."),
    ]
}
private struct CatalogRow: View {
    let item: CatalogApplication
    let installed: LinuxApplication?
    let enabled: Bool
    var install: () -> Void
    var launch: (LinuxApplication) -> Void
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) { Text(item.name).font(.headline); Text(item.evidence.rawValue).font(.caption2.weight(.semibold)).padding(.horizontal, 6).padding(.vertical, 2).foregroundColor(item.evidence.color).overlay(Capsule().stroke(item.evidence.color.opacity(0.6))) }
                Text(item.category + " · " + item.summary).font(.caption).foregroundColor(.secondary)
                if !item.note.isEmpty { Text(item.note).font(.caption2).foregroundColor(.secondary) }
            }
            Spacer()
            if let app = installed { Button("Open") { launch(app) }.buttonStyle(.borderedProminent) }
            else { Button("Install", action: install).buttonStyle(.bordered).disabled(!enabled) }
        }.padding(.vertical, 4).accessibilityElement(children: .combine)
    }
}
struct PackageView: View {
    let environment: LinuxEnvironment
    let enabled: Bool
    var run: (LinuxApplication) -> Void
    var refreshApplications: () -> Void
    @State private var query = ""
    @State private var packages: [LinuxPackageRecord] = []
    @State private var apps: [LinuxApplication] = []
    @State private var previousResult = ""
    @State private var details = ""
    @State private var log = ""
    @State private var status = "No package operation requested"
    @State private var pendingOperation: String?
    @State private var showConfirmation = false
    @State private var error: String?
    @State private var share: ExportItem?
    private let timer = Timer.publish(every: 2, on: .main, in: .common).autoconnect()
    private var support: URL { LRRuntimeBridge.exchangeURL(environment.rootfsName).appendingPathComponent(".linuxrunner/gui") }
    var body: some View {
        Form {
            Section("Distribution") {
                Text(environment.distribution + " · " + environment.architecture)
                Text(enabled ? "Operations use this running environment's real package manager." : "Select this environment for the next launch, then reopen LinuxRunner.").font(.caption)
                Text(status).font(.caption).textSelection(.enabled)
            }
            if environment.distributionID == "debian" {
                Section {
                    ForEach(CatalogApplication.debian.filter { query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) || $0.package.contains(query.lowercased()) }) { item in
                        CatalogRow(item: item, installed: apps.first { ($0.command as NSString).lastPathComponent == item.executable }, enabled: enabled,
                                   install: { query = item.package; confirm("install") }, launch: run)
                    }
                } header: { Text("App Catalog") } footer: {
                    Text("Verified: passed LinuxRunner's automated Mac tests. Partial: works with known gaps. Experimental: works in testing with open issues. Untested: no test evidence yet. Installation uses Debian's package manager in a Linux terminal.")
                }
            }
            Section("Packages") {
                Button("Installed Packages") { execute("installed") }
                Button("Refresh Repository Indexes") { execute("update") }
                Button("Available Updates") { execute("updates") }
                TextField("Package name or search term", text: $query).autocorrectionDisabled().textInputAutocapitalization(.never)
                Button("Search") { execute("search") }.disabled(!PackageOperation.validName(query))
                Button("Package Details") { execute("details") }.disabled(!PackageOperation.validName(query))
                Button("Install Package") { confirm("install") }.disabled(!PackageOperation.validName(query))
                Button("Review Package Upgrade") { confirm("upgrade") }.disabled(!PackageOperation.validName(query))
                Button("Review Removal", role: .destructive) { confirm("remove") }.disabled(!PackageOperation.validName(query))
                Text("Output, dependency plans and progress open in a Linux terminal. Removal asks for confirmation there. Close the terminal when finished, then refresh the installed list. Cancellation during configuration may require Repair Center.").font(.caption)
            }.disabled(!enabled)
            if !packages.isEmpty {
                Section("Package Results") {
                    ForEach(packages) { package in
                        Button { query = package.name; execute("details") } label: {
                            VStack(alignment: .leading) {
                                Text(package.name).font(.headline)
                                Text([package.version, package.architecture, package.status].filter { !$0.isEmpty }.joined(separator: " · ")).font(.caption)
                                if !package.summary.isEmpty { Text(package.summary).font(.caption) }
                            }
                        }.disabled(!enabled)
                    }
                }
            }
            if !details.isEmpty { Section("Package Details") { Text(details).font(.caption).textSelection(.enabled) } }
            Section("Installed Graphical Applications") {
                Button("Refresh Applications") { loadApplications() }
                ForEach(apps) { app in Button("Launch " + app.name) { run(app) } }
                Text("Detected from this environment's desktop entries; discovery does not guarantee runtime compatibility. Use Create Launcher for programs without desktop entries.").font(.caption)
            }.disabled(!enabled)
            Section("Repair Center") {
                Button("Diagnose Package State") { execute("diagnose") }
                Button("Check Dependencies") { execute("dependencies") }
                if environment.distributionID == "debian" { Button("Configure Interrupted Debian Packages") { confirm("configure") } }
                Button("Check / Repair Package State") { confirm("repair") }
                Button("Reset GUI Sessions") { LRRuntimeBridge.resetGraphicalSessions(); refreshApplications() }
                Text("Repair runs native maintainer scripts and keeps a package-database metadata checkpoint inside Linux. It is not a full snapshot or automatic rollback. APT dependency repair asks for confirmation before changing packages. Safe diagnostic mode: use Diagnose and Check Dependencies before repair.").font(.caption)
            }.disabled(!enabled)
            Section("Diagnostics") {
                if !log.isEmpty { Text(log).font(.system(.caption, design: .monospaced)).textSelection(.enabled) }
                Button("Export Diagnostic Report") {
                    do { share = ExportItem(url: try LRRuntimeBridge.exportFullDiagnosticBundle()) } catch { self.error = error.localizedDescription }
                }
            }
            if let error { Text(error).foregroundColor(.red) }
        }.navigationTitle("Linux App Manager")
        .onAppear { readResults(); loadApplications() }.onReceive(timer) { _ in readResults() }
        .confirmationDialog("Confirm Package Operation", isPresented: $showConfirmation, titleVisibility: .visible) {
            Button("Continue in Linux Terminal") { if let operation = pendingOperation { execute(operation) } }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(pendingOperation == "install" ? "Install \(query) and its dependencies into \(environment.name)? Arch also synchronizes and upgrades the system to keep rolling dependencies consistent, with a separate pacman confirmation in the terminal." : pendingOperation == "upgrade" ? "Review an upgrade of \(query) and its dependencies. Arch requires a consistent system upgrade and shows that transaction in pacman before confirmation; Debian and Alpine upgrade the named package." : pendingOperation == "remove" ? "Review removal of \(query) in \(environment.name). The terminal will list affected packages and ask before removing them. No purge or autoremove is requested." : "Run package configuration/repair in \(environment.name). Maintainer scripts may update system configuration. A metadata checkpoint is saved first; this does not restore arbitrary filesystem changes.")
        }
        .sheet(item: $share) { ShareView(item: $0) }
    }
    private func confirm(_ operation: String) { pendingOperation = operation; showConfirmation = true }
    private func execute(_ operation: String) {
        do {
            guard enabled, LRRuntimeBridge.activeRootName() == environment.rootfsName else { throw EnvironmentError.invalidMetadata }
            _ = try LRRuntimeBridge.prepareApplicationSupport()
            let command = try PackageOperation.command(operation, value: query)
            LRRuntimeBridge.logCategory("PACKAGE", event: "operation=\(operation) root=\(environment.rootfsName) requested")
            run(LinuxApplication(name: "Package " + operation, command: command[0], arguments: Array(command.dropFirst()), workingDirectory: "/root"))
        } catch { self.error = error.localizedDescription }
    }
    private func loadApplications() {
        refreshApplications()
        guard enabled else { apps = []; return }
        apps = LRRuntimeBridge.discoverGraphicalApplications().compactMap { item in
            guard let name = item["name"] as? String, let path = item["executable"] as? String, let argv = item["arguments"] as? [String] else { return nil }
            var app = LinuxApplication(name: name, command: path, arguments: argv, workingDirectory: item["directory"] as? String ?? "/root")
            app.displayMode = item["terminal"] as? Bool == true ? .terminal : .application
            return app
        }
    }
    private func read(_ name: String, limit: Int = 262144) -> String {
        let url = support.appendingPathComponent(name)
        guard (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true,
              let data = try? Data(contentsOf: url), data.count <= 2 * 1048576 else { return "" }
        return String(data: data.suffix(limit), encoding: .utf8) ?? ""
    }
    private func readResults() {
        packages = LinuxPackageRecord.parse(read("package-state.tsv"))
        details = read("package-details.txt", limit: 16384)
        log = LRRuntimeBridge.redactDiagnosticText(read("package-operation.log", limit: 8192), application: false)
        let code = read("package-result").trimmingCharacters(in: .whitespacesAndNewlines)
        if code != previousResult { previousResult = code; if code == "0" { loadApplications() } }
        if code == "running" { status = "Operation running or interrupted; inspect terminal / Diagnose Package State." }
        else if !code.isEmpty { status = code == "0" ? "Last package operation completed (exit 0)" : "Last operation failed (exit \(code)); inspect logs and Repair Center." }
    }
}

struct ExportItem: Identifiable { let id = UUID(); let url: URL }
struct ShareView: UIViewControllerRepresentable {
    let item: ExportItem
    func makeUIViewController(context: Context) -> UIActivityViewController {
        let controller = UIActivityViewController(activityItems: [item.url], applicationActivities: nil)
        controller.completionWithItemsHandler = { _, _, _, _ in LRRuntimeBridge.deleteExport(item.url) }
        return controller
    }
    func updateUIViewController(_ uiViewController: UIActivityViewController, context: Context) {}
}

// One entry point reuses both the guarded exchange and existing Files bookmarks.
private struct FileSharingView: View {
    let environment: LinuxEnvironment?
    @ObservedObject var manager: EnvironmentManager
    @Binding var share: ExportItem?
    var body: some View {
        List {
            Section("Import Files") {
                if let environment { NavigationLink("Choose Destination & Import", destination: ExchangeFilesView(environment: environment, manager: manager, share: $share)) }
                else { Text("Install or select a Linux environment before importing files.").foregroundColor(.secondary) }
                Text("Open a destination subfolder, then Import File Here to select a file from iOS Files.").font(.caption).foregroundColor(.secondary)
            }
            Section("Export Files") {
                if let environment { NavigationLink("Select File or Folder to Export", destination: ExchangeFilesView(environment: environment, manager: manager, share: $share)) }
                Text("Select a shared file and Save to Files, or export a folder as ZIP. The native picker selects the iOS destination.").font(.caption).foregroundColor(.secondary)
            }
            Section("Shared Folders") {
                if let environment {
                    NavigationLink("Open Shared Folder", destination: ExchangeFilesView(environment: environment, manager: manager, share: $share))
                    Text("Linux: /mnt/LinuxRunnerShared · ~/Shared when the home alias is available").font(.caption).textSelection(.enabled)
                    Text("iOS Files: On My iPhone/iPad → LinuxRunner → Shared → " + environment.rootfsName).font(.caption).textSelection(.enabled)
                    Text(FileManager.default.fileExists(atPath: LRRuntimeBridge.sharedExchangeURL(environment.rootfsName).path) ? "App shared folder exists. The Linux mount is prepared when this environment runs." : "The app shared folder will be created on first access.").font(.caption).foregroundColor(.secondary)
                }
                NavigationLink("Add or Manage Shared Folders", destination: SharedFoldersView())
                Text("External locations require your Files selection. Manage access and check availability here; removing a link keeps its files.").font(.caption).foregroundColor(.secondary)
            }
        }.navigationTitle("File Sharing")
    }
}
private struct DesktopFileSharingDestination: View {
    @StateObject private var manager = EnvironmentManager()
    @State private var share: ExportItem?
    var body: some View { FileSharingView(environment: manager.environments.first { $0.rootfsName == LRRuntimeBridge.activeRootName() }, manager: manager, share: $share).sheet(item: $share) { ShareView(item: $0) } }
}

private struct SharedFileEntry: Identifiable {
    let name: String
    let directory: Bool
    let bytes: Int64
    var id: String { name }
}
private struct SaveToFilesPicker: UIViewControllerRepresentable {
    let url: URL
    let complete: () -> Void
    func makeCoordinator() -> Coordinator { Coordinator(complete) }
    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(forExporting: [url], asCopy: true)
        picker.delegate = context.coordinator
        return picker
    }
    func updateUIViewController(_ controller: UIDocumentPickerViewController, context: Context) {}
    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let complete: () -> Void
        init(_ complete: @escaping () -> Void) { self.complete = complete }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { complete() }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) { complete() }
    }
}
struct ExchangeFilesView: View {
    let environment: LinuxEnvironment
    @ObservedObject var manager: EnvironmentManager
    @Binding var share: ExportItem?
    let relative: String
    @State private var entries: [SharedFileEntry] = []
    @State private var importer = false
    @State private var creatingFolder = false
    @State private var folderName = ""
    @State private var failure: String?
    @State private var operation: LRTransferControl?
    @State private var progress: [String: NSNumber] = [:]
    @State private var save: ExportItem?
    @State private var snapshot: URL?
    @State private var selected: SharedFileEntry?
    private let tick = Timer.publish(every: 0.3, on: .main, in: .common).autoconnect()
    init(environment: LinuxEnvironment, manager: EnvironmentManager, share: Binding<ExportItem?>, relative: String = "") {
        self.environment = environment; self.manager = manager; _share = share; self.relative = relative
    }
    private var guestPath: String { "/mnt/LinuxRunnerShared" + (relative.isEmpty ? "" : "/" + relative) }
    private func refresh() {
        Task.detached {
            do {
                let records = try LRRuntimeBridge.sharedEntries(relative, root: environment.rootfsName)
                let values = records.compactMap { item -> SharedFileEntry? in
                    guard let record = item as? [String: Any], let name = record["name"] as? String else { return nil }
                    return SharedFileEntry(name: name, directory: record["directory"] as? Bool == true, bytes: (record["bytes"] as? NSNumber)?.int64Value ?? 0)
                }
                await MainActor.run { entries = values }
            } catch { await MainActor.run { failure = error.localizedDescription } }
        }
    }
    private func export(_ entry: SharedFileEntry) {
        guard !manager.busy else { return }; let control = LRTransferControl(); operation = control; manager.busy = true; failure = nil
        Task.detached {
            do {
                let url = try LRRuntimeBridge.exportSharedItem(entry.name, relative: relative, root: environment.rootfsName, control: control)
                await MainActor.run { snapshot = url; save = ExportItem(url: url) }
            } catch { await MainActor.run { failure = error.localizedDescription } }
            await MainActor.run { operation = nil; manager.busy = false; refresh() }
        }
    }
    private func imported(_ result: Result<URL, Error>) {
        do {
            let source = try result.get(); let control = LRTransferControl(); operation = control; manager.busy = true; failure = nil
            Task.detached {
                do { try LRRuntimeBridge.importSharedFile(source, relative: relative, root: environment.rootfsName, control: control) }
                catch { await MainActor.run { failure = error.localizedDescription } }
                await MainActor.run { operation = nil; manager.busy = false; refresh() }
            }
        } catch { failure = error.localizedDescription }
    }
    var body: some View {
        List {
            Section("Destination in Linux") {
                Text(guestPath).font(.system(.subheadline, design: .monospaced)).textSelection(.enabled)
                Text("The selected Linux user also gets ~/Shared when that name is free. In iOS Files: On My iPhone/iPad → LinuxRunner → Shared → \(environment.rootfsName). Each environment has its own folder.").font(.caption).foregroundColor(.secondary)
                Button("Import File Here") { importer = true }.disabled(manager.busy)
                Button("New Folder") { folderName = ""; creatingFolder = true }.disabled(manager.busy)
                if let operation {
                    let bytes = progress["bytes"]?.int64Value ?? 0, total = progress["total"]?.int64Value ?? 0
                    if total > 0 { ProgressView(value: Double(bytes), total: Double(total)) }
                    Text("Transferred \(ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file))").font(.caption)
                    Button("Cancel Transfer", role: .cancel) { operation.cancel() }
                }
                if let failure { Text(failure).font(.footnote).foregroundColor(.orange).textSelection(.enabled) }
            }
            Section("Files") {
                ForEach(entries) { entry in
                    if entry.directory {
                        NavigationLink(destination: ExchangeFilesView(environment: environment, manager: manager, share: $share, relative: relative.isEmpty ? entry.name : relative + "/" + entry.name)) {
                            Label(entry.name, systemImage: "folder")
                        }.contextMenu { Button("Export Folder as ZIP") { export(entry) } }
                    } else {
                        Button { selected = entry } label: {
                            HStack { Label(entry.name, systemImage: "doc"); Spacer(); Text(ByteCountFormatter.string(fromByteCount: entry.bytes, countStyle: .file)).font(.caption).foregroundColor(.secondary) }
                        }.disabled(manager.busy)
                    }
                }
                if entries.isEmpty { Text("No visible files in this folder.").foregroundColor(.secondary) }
            }
            if !relative.isEmpty { Section { Button("Export This Folder as ZIP") { let leaf = (relative as NSString).lastPathComponent; exportParent(leaf) }.disabled(manager.busy) } }
            if relative.isEmpty {
                Section("Existing Files") { NavigationLink("Previous Downloads Exchange", destination: LegacyExchangeFilesView(environment: environment, manager: manager, share: $share)) }
            }
            Section { Text("Transfers stream data up to 512 MiB. Existing names are preserved. Exporting a directory creates a ZIP; links and special files are refused. Save changes before exporting a file that another app is editing.").font(.caption).foregroundColor(.secondary) }
        }.navigationTitle(relative.isEmpty ? "Shared Files" : (relative as NSString).lastPathComponent)
            .onAppear(perform: refresh).toolbar { Button("Refresh", action: refresh) }
            .onReceive(tick) { _ in if let operation { progress = operation.snapshot() as? [String: NSNumber] ?? [:] } }
            .fileImporter(isPresented: $importer, allowedContentTypes: [.data], onCompletion: imported)
            .sheet(item: $save, onDismiss: { if let snapshot { LRRuntimeBridge.deleteExport(snapshot) }; snapshot = nil }) { item in SaveToFilesPicker(url: item.url) { save = nil } }
            .confirmationDialog("Export File", isPresented: Binding(get: { selected != nil }, set: { if !$0 { selected = nil } })) {
                if let selected { Button("Save to Files…") { export(selected); self.selected = nil } }
                Button("Cancel", role: .cancel) { selected = nil }
            }
            .alert("New Folder", isPresented: $creatingFolder) {
                TextField("Folder name", text: $folderName).autocorrectionDisabled().textInputAutocapitalization(.never)
                Button("Cancel", role: .cancel) { }
                Button("Create") { do { try LRRuntimeBridge.createSharedDirectory(folderName, relative: relative, root: environment.rootfsName); refresh() } catch { failure = error.localizedDescription } }
            }
    }
    private func exportParent(_ leaf: String) {
        // The current directory is explicitly selected, but the native archive
        // API still opens it fd-relative to its parent and refuses symbolic links.
        guard !manager.busy else { return }; let parent = (relative as NSString).deletingLastPathComponent
        let control = LRTransferControl(); operation = control; manager.busy = true; failure = nil
        Task.detached {
            do { let url = try LRRuntimeBridge.exportSharedItem(leaf, relative: parent == "." ? "" : parent, root: environment.rootfsName, control: control); await MainActor.run { snapshot = url; save = ExportItem(url: url) } }
            catch { await MainActor.run { failure = error.localizedDescription } }
            await MainActor.run { operation = nil; manager.busy = false }
        }
    }
}

private struct LegacyExchangeFilesView: View {
    let environment: LinuxEnvironment
    @ObservedObject var manager: EnvironmentManager
    @Binding var share: ExportItem?
    @State private var leaves: [String] = []
    @State private var importer = false
    var body: some View {
        List {
            Section {
                Text("Shared folder: /home/mobile/Downloads").font(.subheadline).textSelection(.enabled)
                Text("Copy files here in Linux to export them. Import supports regular files up to 512 MiB and preserves existing names.").font(.caption)
                Button("Import File") { importer = true }.disabled(manager.busy)
            }
            Section("Files — tap to export") {
                ForEach(leaves, id: \.self) { leaf in
                    Button(leaf) {
                        manager.busy = true
                        Task.detached {
                            do {
                                let url = try LRRuntimeBridge.exportFile(leaf, root: environment.rootfsName)
                                await MainActor.run { share = ExportItem(url: url) }
                            } catch { await MainActor.run { manager.error = error.localizedDescription } }
                            await MainActor.run { manager.busy = false }
                        }
                    }.disabled(manager.busy)
                }
                if leaves.isEmpty { Text("No files in the shared folder.").foregroundColor(.secondary) }
            }
        }.navigationTitle("Files")
         .onAppear(perform: refresh)
         .toolbar { Button("Refresh", action: refresh) }
         .fileImporter(isPresented: $importer, allowedContentTypes: [.data]) { result in
             do {
                 let source = try result.get()
                 manager.busy = true
                 Task.detached {
                     do { try LRRuntimeBridge.importFile(source, root: environment.rootfsName) }
                     catch { await MainActor.run { manager.error = error.localizedDescription } }
                     await MainActor.run { manager.busy = false; refresh(); manager.refresh() }
                 }
             } catch { manager.error = error.localizedDescription }
         }
    }
    private func refresh() {
        leaves = ((try? FileManager.default.contentsOfDirectory(at: LRRuntimeBridge.exchangeURL(environment.rootfsName), includingPropertiesForKeys: [.isRegularFileKey, .isSymbolicLinkKey])) ?? []).filter {
            guard !$0.lastPathComponent.hasPrefix("."), let info = try? $0.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey]) else { return false }
            return info.isRegularFile == true && info.isSymbolicLink != true
        }.map(\.lastPathComponent).sorted()
    }
}

struct DiagnosticsView: View {
    @ObservedObject var manager: EnvironmentManager
    @Binding var share: ExportItem?
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationView {
            List {
                Section("Runtime") {
                    Text("App: \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?") (\(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"))")
                    Text("Source: \(Bundle.main.infoDictionary?["LinuxRunnerSourceCommit"] as? String ?? "unknown") · iSH-derived runtime")
                    Text("Guest: ARM64 · " + ((LRRuntimeBridge.jitStatusReport() as? [String: Any])?["state"] as? String ?? "Interpreter Active"))
                    Text("Host: \(UIDevice.current.systemName) \(UIDevice.current.systemVersion) · ARM64")
                    Text(LRRuntimeBridge.runtimeStatus())
                    ForEach(manager.environments.filter(manager.active)) { env in
                        Text("\(env.distribution) \(env.rootfsVersion) · \(manager.size(env))")
                    }
                }
                Section("Diagnostics") {
                    Text("Categories: EXEC, SYSCALL, FS, MEMORY, SIGNAL, NETWORK, LOADER, UI. This export includes app events and instruction-fault metadata. Detailed syscall tracing is a developer build option.").font(.caption)
                    Button("Export Diagnostics") {
                        do {
                            let url = try LRRuntimeBridge.exportLogs()
                            dismiss()
                            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { share = ExportItem(url: url) }
                        } catch { manager.error = error.localizedDescription }
                    }
                    ForEach(Array(manager.logs.enumerated()), id: \.offset) { _, line in Text(line).font(.system(.caption, design: .monospaced)).textSelection(.enabled) }
                }
            }.navigationTitle("Diagnostics").toolbar { Button("Done") { dismiss() } }
        }.navigationViewStyle(.stack)
    }
}

struct DeveloperDiagnosticsView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var entries: [[String: Any]] = []
    @State private var health: [String: Any] = [:]
    @State private var search = ""
    @State private var category = "All"
    @State private var severity = "All"
    @State private var pid = ""
    @State private var syscall = ""
    @State private var level = UserDefaults.standard.object(forKey: "DiagnosticLevel") == nil ? 2 : UserDefaults.standard.integer(forKey: "DiagnosticLevel")
    @State private var retention = UserDefaults.standard.object(forKey: "DiagnosticRetention") == nil ? 4 : UserDefaults.standard.integer(forKey: "DiagnosticRetention")
    @State private var share: ExportItem?
    @State private var failure: String?
    private let tick = Timer.publish(every: 2, on: .main, in: .common).autoconnect()
    private var filtered: [[String: Any]] { entries.filter { entry in
        (category == "All" || entry["category"] as? String == category) &&
        (severity == "All" || entry["level"] as? String == severity) &&
        (pid.isEmpty || (entry["pid"] as? Int) == Int(pid) || (entry["tid"] as? Int) == Int(pid)) &&
        (search.isEmpty || String(describing: entry["message"] ?? "").localizedCaseInsensitiveContains(search))
    } }
    var body: some View {
        NavigationView {
            List {
                Section("Runtime health") { ForEach(health.keys.sorted(), id: \.self) { key in HStack { Text(key); Spacer(); Text(String(describing: health[key] ?? "unavailable")).foregroundColor(.secondary).textSelection(.enabled) } } }
                Section("Logging") {
                    Picker("Level", selection: $level) { ForEach(DiagnosticLevel.allCases, id: \.rawValue) { Text($0.label).tag($0.rawValue) } }
                    Text("TRACE records successful calls and has substantial overhead. Logs rotate at 1 MiB.").font(.caption)
                    Stepper("Previous log files: \(retention)", value: $retention, in: 1...16).onChange(of: retention) { value in DiagnosticCenter.setRetention(value) }
                    TextField("Guest PID/TID (optional)", text: $pid).keyboardType(.numberPad)
                    TextField("Syscall number (optional)", text: $syscall).keyboardType(.numberPad)
                    Button("Apply tracing filters") { LRRuntimeBridge.configureDiagnostics(Int32(level), process: Int32(pid) ?? -1, syscall: Int32(syscall) ?? -1, errorsOnly: level < 4) }
                }
                Section("Live logs") {
                    Picker("Category", selection: $category) { Text("All").tag("All"); ForEach(Array(Set(entries.compactMap { $0["category"] as? String })).sorted(), id: \.self) { Text($0).tag($0) } }
                    Picker("Severity", selection: $severity) { Text("All").tag("All"); ForEach(DiagnosticLevel.allCases, id: \.rawValue) { Text($0.label).tag($0.label) } }
                    TextField("Search messages", text: $search)
                    ForEach(Array(filtered.enumerated()), id: \.offset) { _, entry in
                        VStack(alignment: .leading) {
                            Text("\(entry["level"] ?? "") · \(entry["category"] ?? "") · PID \(entry["pid"] ?? 0)").font(.caption).foregroundColor(.secondary)
                            Text(entry["message"] as? String ?? "").font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                        }
                    }
                }
                Section {
                    Button("Copy Filtered Logs") { UIPasteboard.general.string = filtered.map { String(describing: $0["message"] ?? "") }.joined(separator: "\n") }
                    Button("Export Full Diagnostic Bundle") {
                        Task.detached {
                            do { let url = try LRRuntimeBridge.exportFullDiagnosticBundle(); await MainActor.run { share = ExportItem(url: url) } }
                            catch { await MainActor.run { failure = error.localizedDescription } }
                        }
                    }
                    Text("Export includes redacted diagnostic records and recent display logs. Browser profiles, cookies, keys, input payloads and arbitrary application output are excluded.").font(.caption)
                    if let failure { Text(failure).foregroundColor(.red) }
                }
            }.navigationTitle("Developer Diagnostics").toolbar { Button("Done") { dismiss() } }
        }.navigationViewStyle(.stack).onAppear(perform: refresh).onReceive(tick) { _ in refresh() }.sheet(item: $share) { ShareView(item: $0) }
    }
    private func refresh() { entries = LRRuntimeBridge.diagnosticEntries().compactMap { $0 as? [String: Any] }; health = LRRuntimeBridge.healthSummary() as? [String: Any] ?? [:] }
}

struct StorageManagementView: View {
    let environments: [LinuxEnvironment]
    @State private var usage: [String: Int64] = [:]
    @State private var free: Int64?
    @State private var cleaning = false
    var body: some View {
        List {
            Section("Persistent storage") { ForEach(usage.keys.sorted(), id: \.self) { key in HStack { Text(key); Spacer(); Text(ByteCountFormatter.string(fromByteCount: usage[key] ?? 0, countStyle: .file)) } } }
            Section { if let free { Text("Host available: \(ByteCountFormatter.string(fromByteCount: free, countStyle: .file))") }
                Button("Clean Temporary Files") { cleaning = true }
                Text("Removes only LinuxRunner's inactive installation staging files. User files, environments, packages and browser profiles are preserved.").font(.caption)
            }
        }.navigationTitle("Storage").onAppear(perform: measure)
        .confirmationDialog("Remove inactive temporary installation files?", isPresented: $cleaning) { Button("Clean Temporary Files") {
            let staging = LRRuntimeBridge.managedStorageURL().appendingPathComponent("installer-temporary")
            // Installer creates this namespace exclusively; no rootfs/exchange paths.
            if !UserDefaults.standard.bool(forKey: "InstallerActive") { try? FileManager.default.removeItem(at: staging) }
            measure()
        } }
    }
    private func measure() {
        let items = environments
        Task.detached(priority: .utility) {
            var values: [String: Int64] = [:]
            for e in items { values["\(e.name) rootfs"] = StorageUsage.bytes(LRRuntimeBridge.rootURL(e.rootfsName)); values["\(e.name) shared files"] = StorageUsage.bytes(LRRuntimeBridge.exchangeURL(e.rootfsName)); values["\(e.name) package cache (part of rootfs)"] = StorageUsage.bytes(LRRuntimeBridge.rootURL(e.rootfsName).appendingPathComponent("data/var/cache")) }
            let base = LRRuntimeBridge.managedStorageURL()
            values["Diagnostics"] = StorageUsage.bytes(base.deletingLastPathComponent().appendingPathComponent("LinuxRunnerDiagnostics"))
            values["Installation downloads"] = StorageUsage.bytes(base.appendingPathComponent("downloads"))
            values["Installation staging"] = StorageUsage.bytes(base.appendingPathComponent("installer-temporary"))
            values["Total persistent container"] = StorageUsage.bytes(base)
            values["Bundled application"] = StorageUsage.bytes(Bundle.main.bundleURL)
            let available = try? base.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage
            await MainActor.run { usage = values; free = available }
        }
    }
}

struct ExecutionEngineView: View {
    @State private var status = LRRuntimeBridge.executionBackendStatus() as? [String: Any] ?? [:]
    @State private var report = LRRuntimeBridge.jitStatusReport() as? [String: Any] ?? [:]
    @State private var mode = LRRuntimeBridge.executionMode()
    private let ticker = Timer.publish(every: 2, on: .main, in: .common).autoconnect()
    @State private var requested = false
    @State private var message: String?
    private var compiled: Bool { status["compiled"] as? Bool ?? false }
    private var active: Bool { (status["active"] as? String) == "jit" }
    var body: some View {
        List {
            Section {
                HStack {
                    Image(systemName: jitStateIcon).foregroundColor(jitStateColor)
                    Text(report["state"] as? String ?? "Interpreter Active").bold()
                }
                Text(report["detail"] as? String ?? "").font(.footnote).foregroundColor(.secondary)
                if compiled {
                    LabeledRow("Running", active ? "ARM64 JIT (with interpreter for untranslated code)" : "ARM64 Interpreter")
                    LabeledRow("Debugger attached", (report["debugger"] as? Bool ?? false) ? "Yes" : "No")
                    LabeledRow("Guest blocks translated", "\((report["blocks"] as? NSNumber)?.uint64Value ?? 0)")
                    LabeledRow("Native code", "\((report["code_kb"] as? NSNumber)?.uint64Value ?? 0) KB")
                    LabeledRow("iOS", report["os"] as? String ?? "")
                }
            } header: { Text("JIT Status") } footer: {
                Text("JIT Active is shown only after LinuxRunner ran test code in its JIT region and installed translated guest code that the runtime now executes. StikDebug reporting that JIT was assigned is not enough on its own.")
            }
            if compiled {
                Section {
                    Picker("Mode", selection: $mode) {
                        Text("Interpreter").tag("interpreter")
                        Text("Automatic").tag("automatic")
                        Text("JIT").tag("jit")
                    }.onChange(of: mode) { LRRuntimeBridge.setExecutionMode($0) }
                } footer: {
                    Text("Interpreter always works. Automatic and JIT start the JIT only after iOS allows it — LinuxRunner launched or attached by StikDebug — and otherwise keep the interpreter. The active backend above is what actually runs.")
                }
                if !active && (mode != "interpreter" || (report["debugger"] as? Bool ?? false)) {
                    Section {
                        Button("Attach with StikDebug") { requestAttachment() }
                        Button("Activate JIT Now") { activate() }
                        if let message { Text(message).font(.footnote).foregroundColor(.orange) }
                    } footer: {
                        Text(LRRuntimeBridge.requiresPreparedJITRegion()
                             ? "iOS 26 and later: StikDebug attaches with its universal script; return here and LinuxRunner prepares its code region while it is attached. Support on iOS 26 has not been verified on a device yet."
                             : "StikDebug attaches and detaches; return here and LinuxRunner checks whether iOS now allows its code region.")
                    }
                }
            } else {
                Section { Text("This build is interpreter-only. The JIT-capable LinuxRunner build adds the JIT option here; the interpreter remains the default there too.").font(.footnote) }
            }
        }.navigationTitle("Execution Engine")
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)) { _ in
            if requested { requested = false; activate() } else { refresh() }
        }
        .onReceive(ticker) { _ in refresh() }
    }
    private var jitStateIcon: String {
        switch report["state"] as? String {
        case "JIT Active": return "bolt.fill"
        case "JIT Initializing", "JIT Available": return "bolt"
        case "JIT Failed", "JIT Unavailable": return "exclamationmark.triangle"
        case "Waiting for JIT Authorization": return "hourglass"
        default: return "cpu"
        }
    }
    private var jitStateColor: Color {
        switch report["state"] as? String {
        case "JIT Active": return .green
        case "JIT Failed", "JIT Unavailable": return .orange
        default: return .secondary
        }
    }
    private func refresh() {
        status = LRRuntimeBridge.executionBackendStatus() as? [String: Any] ?? [:]
        report = LRRuntimeBridge.jitStatusReport() as? [String: Any] ?? [:]
    }
    private func activate() {
        var detail: NSString?
        if mode == "interpreter" { mode = "automatic"; LRRuntimeBridge.setExecutionMode(mode) }
        if !LRRuntimeBridge.activateJIT(&detail) { message = detail as String? }
        refresh()
    }
    private func requestAttachment() {
        guard let identifier = Bundle.main.bundleIdentifier,
              var components = URLComponents(string: "stikdebug://enable-jit") else { return }
        var items = [URLQueryItem(name: "bundle-id", value: identifier), URLQueryItem(name: "pid", value: String(ProcessInfo.processInfo.processIdentifier))]
        if LRRuntimeBridge.requiresPreparedJITRegion() { items.append(URLQueryItem(name: "script-name", value: "universal")) }
        components.queryItems = items
        guard let url = components.url, UIApplication.shared.canOpenURL(url) else {
            message = "StikDebug is not installed or not set up. Install it, pair this device, then try again."
            return
        }
        requested = true
        LRRuntimeBridge.logCategory("JIT", event: "StikDebug attachment requested")
        UIApplication.shared.open(url, options: [:]) { opened in if !opened { requested = false; message = "StikDebug could not be opened." } }
    }
}

private struct LabeledRow: View {
    let label: String, value: String
    init(_ label: String, _ value: String) { self.label = label; self.value = value }
    var body: some View { HStack { Text(label).foregroundColor(.secondary); Spacer(); Text(value).multilineTextAlignment(.trailing) }.font(.footnote) }
}

private struct ApplicationGridView: View {
    @ObservedObject var manager: EnvironmentManager
    let run: (LinuxApplication) -> Void
    @State private var selectedRoot = LRRuntimeBridge.activeRootName()
    @State private var search = ""
    @State private var category = "All"
    @State private var favorites = Set<String>()
    @State private var recent = [String]()
    private var environment: LinuxEnvironment? { manager.environments.first { $0.rootfsName == selectedRoot } }
    private var available: [LinuxApplication] {
        guard let environment, manager.active(environment) else { return [] }
        var seen = Set<String>()
        return (manager.graphical + environment.applications.filter { $0.displayMode == .application }).filter { seen.insert($0.launchKey).inserted }
    }
    private var categories: [String] { ["All", "Favorites", "Recent"] + Set(available.flatMap { $0.categories ?? [] }).sorted() }
    private var filtered: [LinuxApplication] {
        available.filter { app in
            (category == "All" || category == "Favorites" && favorites.contains(app.launchKey) || category == "Recent" && recent.contains(app.launchKey) || (app.categories ?? []).contains(category)) &&
            (search.isEmpty || [app.name, app.genericName ?? "", app.comment ?? ""].contains { $0.localizedCaseInsensitiveContains(search) })
        }.sorted { a, b in
            if category == "Recent" { return (recent.firstIndex(of: a.launchKey) ?? 999) < (recent.firstIndex(of: b.launchKey) ?? 999) }
            return a.name.localizedStandardCompare(b.name) == .orderedAscending
        }
    }
    private var preferenceKey: String { "LinuxRunner.launcher." + selectedRoot }
    private func loadPreferences() {
        favorites = Set(UserDefaults.standard.stringArray(forKey: preferenceKey + ".favorites") ?? [])
        recent = UserDefaults.standard.stringArray(forKey: preferenceKey + ".recent") ?? []
    }
    private func favorite(_ app: LinuxApplication) {
        if favorites.contains(app.launchKey) { favorites.remove(app.launchKey) } else { favorites.insert(app.launchKey) }
        UserDefaults.standard.set(Array(favorites).sorted(), forKey: preferenceKey + ".favorites")
    }
    private func launch(_ app: LinuxApplication) {
        guard let environment, manager.active(environment) else { return }
        recent.removeAll { $0 == app.launchKey }; recent.insert(app.launchKey, at: 0)
        recent = Array(recent.prefix(20)); UserDefaults.standard.set(recent, forKey: preferenceKey + ".recent")
        run(app)
    }
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                Picker("Environment", selection: $selectedRoot) {
                    ForEach(manager.environments) { Text($0.name).tag($0.rootfsName) }
                }.pickerStyle(.menu)
                HStack {
                    Label(environment.map { manager.active($0) ? "Runtime running" : "Environment stopped" } ?? "No environment", systemImage: "display")
                    Spacer()
                    if manager.discovering { ProgressView() }
                    Button { manager.refresh() } label: { Image(systemName: "arrow.clockwise") }.accessibilityLabel("Refresh Applications")
                }.font(.caption).foregroundColor(.secondary)
                if let environment, !manager.active(environment) {
                    Text("Select this environment for the next launch, then reopen LinuxRunner. Its installed packages and files are preserved.").font(.callout)
                    Button("Use on Next Launch") { LRRuntimeBridge.setNextRootName(environment.rootfsName) }
                } else {
                    Picker("Category", selection: $category) { ForEach(categories, id: \.self) { Text($0).tag($0) } }.pickerStyle(.menu)
                    if filtered.isEmpty && !manager.discovering {
                        Text(available.isEmpty ? "No graphical desktop entries found. Install an application through App Manager or create an advanced launcher." : "No applications match this filter.").foregroundColor(.secondary)
                    }
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 140), spacing: 14)], spacing: 14) {
                        ForEach(filtered, id: \.launchKey) { app in
                            Button { launch(app) } label: {
                                VStack(spacing: 12) {
                                    if let image = manager.applicationIcons[app.launchKey] {
                                        Image(uiImage: image).resizable().scaledToFit().frame(width: 48, height: 48)
                                    } else {
                                        Image(systemName: "app.fill").font(.system(size: 38)).frame(width: 48, height: 48).foregroundColor(.accentColor)
                                    }
                                    Text(app.name).font(.headline).foregroundColor(.primary).lineLimit(2)
                                    Text(app.genericName ?? "Linux application").font(.caption).foregroundColor(.secondary).lineLimit(1)
                                    if favorites.contains(app.launchKey) { Image(systemName: "star.fill").font(.caption).foregroundColor(.yellow) }
                                }.frame(maxWidth: .infinity, minHeight: 142).padding(12)
                                    .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
                            }.buttonStyle(.plain).accessibilityIdentifier("launch-" + app.name)
                                .contextMenu {
                                    Button("Launch") { launch(app) }
                                    Button(favorites.contains(app.launchKey) ? "Remove Favorite" : "Add Favorite") { favorite(app) }
                                    Text(app.command)
                                }
                        }
                    }
                    Text("Discovery identifies installed launchers; it does not certify application compatibility. Applications launch into the shared desktop. Returning to LinuxRunner retains its windows; Stop Desktop ends the owned session.").font(.footnote).foregroundColor(.secondary)
                    if let environment {
                        NavigationLink("App Manager", destination: PackageView(environment: environment, enabled: manager.active(environment), run: run, refreshApplications: manager.refresh))
                        NavigationLink("Advanced Command Launcher", destination: LauncherEditor { manager.add(application: $0, to: environment) })
                    }
                }
            }.padding()
        }.background(Color(uiColor: .systemGroupedBackground))
            .navigationTitle("Applications").searchable(text: $search, prompt: "Search installed applications")
            .onAppear { loadPreferences(); manager.refresh() }
            .onChange(of: selectedRoot) { _ in category = "All"; loadPreferences(); manager.refresh() }
    }
}

// Desktop controls share the existing viewer/session; this handle never retains it.
@MainActor private final class DesktopControlHandle: ObservableObject {
    weak var controller: LRGUIViewController?
    init(_ controller: LRGUIViewController) { self.controller = controller }
}
private struct DesktopControlSettings: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var handle: DesktopControlHandle
    @State private var state: [String: Any] = [:]
    @State private var health: [String: Any] = [:]
    @State private var pending: String?
    @AppStorage("LinuxRunnerDesktop.externalDisplay") private var externalOutput = true
    @AppStorage("LinuxRunnerInput.touchpadGain") private var touchpadGain = 1.5
    @AppStorage("LinuxRunnerInput.mouseGain") private var mouseGain = 1.0
    @AppStorage("LinuxRunnerInput.scrollGain") private var scrollGain = 1.0
    private let tick = Timer.publish(every: 2, on: .main, in: .common).autoconnect()
    let openDesktop: (() -> Void)?
    init(controller: LRGUIViewController, openDesktop: (() -> Void)? = nil) { _handle = StateObject(wrappedValue: DesktopControlHandle(controller)); self.openDesktop = openDesktop }
    private func action(_ name: String) {
        let controller = handle.controller
        dismiss()
        // Presenting another native sheet waits until this one is dismissed.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { if name == "start", let openDesktop { openDesktop() } else { controller?.performDesktopAction(name) } }
    }
    private func refresh() { state = handle.controller?.desktopControlState() as? [String: Any] ?? [:]; health = LRRuntimeBridge.healthSummary() as? [String: Any] ?? [:] }
    private func value(_ key: String) -> String { state[key] as? String ?? "Not available" }
    private var alive: Bool { state["alive"] as? Bool == true }
    var body: some View {
        NavigationView {
            Form {
                Section("Status") {
                    LabeledContentRow("Session", value: value("state"))
                    LabeledContentRow("Host thermal state", value: health["host_thermal_state"] as? String ?? "Not reported")
                    LabeledContentRow("Distribution", value: LRRuntimeBridge.activeEnvironmentIsArch() ? "Arch Linux ARM" : LRRuntimeBridge.activeEnvironmentIsDebian() ? "Debian ARM64" : "Alpine ARM64")
                    LabeledContentRow("Desktop", value: value("desktopEnvironment"))
                    LabeledContentRow("Linux user", value: value("user"))
                    LabeledContentRow("Display server", value: value("displayServer"))
                    if health["memory_measured"] as? Bool == true, let bytes = health["host_resident_bytes"] as? NSNumber {
                        LabeledContentRow("App memory", value: ByteCountFormatter.string(fromByteCount: bytes.int64Value, countStyle: .memory))
                        Text("Host app resident memory, including the interpreter and guest workloads.").font(.caption).foregroundColor(.secondary)
                    }
                }
                Section("Display") {
                    LabeledContentRow("Received framebuffer", value: value("framebuffer"))
                    NavigationLink("Resolution & Viewer Preferences", destination: DesktopPreferencesView())
                    HStack { Button("Fit", action: { action("fit") }); Spacer(); Button("Actual Size", action: { action("actual") }); Spacer(); Button("Full Screen", action: { action("fullscreen") }) }
                    Toggle("External Desktop Output", isOn: $externalOutput)
                    LabeledContentRow("External scene", value: state["externalConnected"] as? Bool == true ? "Connected" : "Not connected")
                    if let maximum = state["screenMaximumRefresh"] as? Int, maximum > 0 { Text("iOS screen capability: up to \(maximum) Hz. This is not measured Linux frame rate.").font(.caption).foregroundColor(.secondary) }
                    if let modes = state["screenModes"] as? [String], !modes.isEmpty { DisclosureGroup("iOS Screen Modes · Read Only") { ForEach(modes, id: \.self) { Text($0).font(.caption) }; Text("Host screen capabilities; these are not additional guest resolution or refresh-rate controls.").font(.caption).foregroundColor(.secondary) } }
                    Button("Wireless Display (AirPlay)…") { action("wireless") }
                    Text("Fit preserves aspect ratio. Guest resolution applies at the next desktop start; the current Xvfb session is retained while changing viewer scaling.").font(.caption).foregroundColor(.secondary)
                }
                Section("Input") {
                    Button(state["touchpad"] as? Bool == true ? "Show Desktop on This Device" : state["trackpadRequested"] as? Bool == true ? "Cancel Trackpad Connection" : "Use iPhone as Trackpad") { action("touchpad") }
                    if state["trackpadReady"] as? Bool != true { Text("Enables external output and opens trackpad controls when a separate display and desktop frame are available.").font(.caption).foregroundColor(.secondary) }
                    Text("Touchpad sensitivity: \(touchpadGain, specifier: "%.2g")×").font(.caption)
                    Slider(value: $touchpadGain, in: 0.25...4, step: 0.25).accessibilityLabel("Touchpad sensitivity")
                    Text("Mouse sensitivity: \(mouseGain, specifier: "%.2g")×").font(.caption)
                    Slider(value: $mouseGain, in: 0.25...4, step: 0.25).accessibilityLabel("Mouse sensitivity")
                    Text("Scrolling sensitivity: \(scrollGain, specifier: "%.2g")×").font(.caption)
                    Slider(value: $scrollGain, in: 0.25...4, step: 0.25).accessibilityLabel("Scrolling sensitivity")
                    NavigationLink("Keyboard & Terminal", destination: TerminalSettingsView())
                    Button("Paste Clipboard Text") { action("clipboard") }
                    Text("Slide to move, tap to click, hold and slide to drag; two fingers scroll or tap for right click in touchpad mode. Clipboard text is sent only when requested.").font(.caption).foregroundColor(.secondary)
                }
                Section("Desktop Session") {
                    Button(alive ? "Reconnect Viewer" : "Start Desktop") { action("start") }.disabled(!LRRuntimeBridge.isRunning())
                    Button("Restart Desktop") { pending = "restart" }.disabled(!alive)
                    Button("Stop Desktop", role: .destructive) { pending = "stop" }.disabled(!alive)
                    NavigationLink("Desktop Environment", destination: DesktopEnvironmentView())
                    Button("Install / Repair Desktop Components") { action("install") }.disabled(LRRuntimeBridge.installationRunning())
                    NavigationLink("Desktop Logs", destination: DesktopLogsContent())
                }
                Section { NavigationLink("File Sharing", destination: DesktopFileSharingDestination()) }
                Section("Advanced") {
                    LabeledContentRow("Presentation", value: value("presenter"))
                    NavigationLink("Guest Graphics Backend", destination: GraphicsSettingsView())
                    Text(value("statusDetail")).font(.caption).textSelection(.enabled)
                    Text("Changing the viewer does not replace X11 or create another Linux session. No GPU utilization or wireless performance is inferred from connection state.").font(.caption).foregroundColor(.secondary)
                }
            }.navigationTitle("Desktop Settings").toolbar { Button("Done") { dismiss() } }
        }.navigationViewStyle(.stack).onAppear(perform: refresh).onReceive(tick) { _ in refresh() }
            .alert("\(pending == "restart" ? "Restart" : "Stop") Desktop?", isPresented: Binding(get: { pending != nil }, set: { if !$0 { pending = nil } })) {
                Button("Cancel", role: .cancel) { pending = nil }
                Button("Continue", role: .destructive) { let operation = pending; pending = nil; if let operation { action(operation) } }
            } message: { Text("Save documents first. This closes applications in this desktop. Linux files and other terminal sessions are retained.") }
    }
}
private struct LabeledContentRow: View {
    let title: String
    let value: String
    init(_ title: String, value: String) { self.title = title; self.value = value }
    var body: some View { HStack(alignment: .firstTextBaseline) { Text(title); Spacer(); Text(value).foregroundColor(.secondary).multilineTextAlignment(.trailing).textSelection(.enabled) } }
}
private struct DesktopLogsContent: View {
    @State private var entries: [[String: Any]] = []
    @State private var search = ""
    @State private var component = "All"
    @State private var severity = "All"
    @State private var loading = false
    @State private var failure: String?
    @State private var share: ExportItem?
    private var filtered: [[String: Any]] { entries.filter { entry in
        (component == "All" || entry["category"] as? String == component) &&
        (severity == "All" || entry["level"] as? String == severity) &&
        (search.isEmpty || (entry["message"] as? String ?? "").localizedCaseInsensitiveContains(search))
    } }
    private func refresh() {
        guard !loading else { return }; loading = true
        Task.detached {
            let records = LRRuntimeBridge.desktopDiagnosticEntries().compactMap { $0 as? [String: Any] }
            await MainActor.run { entries = records; loading = false }
        }
    }
    private func timestamp(_ entry: [String: Any]) -> String {
        let seconds = (entry["timestamp"] as? NSNumber)?.doubleValue ?? 0
        guard seconds > 0 else { return "Time unavailable" }
        let time = Date(timeIntervalSince1970: seconds).formatted(date: .omitted, time: .standard)
        return entry["file_time"] as? Bool == true ? "File updated " + time : time
    }
    private var copiedText: String { filtered.map { "\(timestamp($0)) · \($0["level"] ?? "") · \($0["category"] ?? "")\n\($0["message"] ?? "")" }.joined(separator: "\n\n") }
    var body: some View {
        List {
            Section("Filters") {
                Picker("Component", selection: $component) { Text("All").tag("All"); ForEach(Array(Set(entries.compactMap { $0["category"] as? String })).sorted(), id: \.self) { Text($0).tag($0) } }
                Picker("Severity", selection: $severity) { Text("All").tag("All"); ForEach(["ERROR", "WARNING", "INFO", "DEBUG", "TRACE"], id: \.self) { Text($0).tag($0) } }
                if loading { ProgressView("Reading bounded diagnostic tails…") }
                Text("Application output is filtered and redacted. Log-tail timestamps are file modification times, not invented per-line event times.").font(.caption).foregroundColor(.secondary)
            }
            Section("Desktop Diagnostics") {
                ForEach(Array(filtered.enumerated()), id: \.offset) { _, entry in
                    VStack(alignment: .leading, spacing: 4) {
                        Text("\(timestamp(entry)) · \(entry["level"] ?? "") · \(entry["category"] ?? "")").font(.caption).foregroundColor(.secondary)
                        Text(entry["message"] as? String ?? "").font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                    }
                }
                if !loading && filtered.isEmpty { Text("No matching diagnostic records. Start the desktop or refresh after a failure.").foregroundColor(.secondary) }
            }
            Section("Share") {
                Button("Copy Filtered Logs") { UIPasteboard.general.string = copiedText }
                Button("Export Diagnostic Bundle") {
                    Task.detached {
                        do { let url = try LRRuntimeBridge.exportFullDiagnosticBundle(); await MainActor.run { share = ExportItem(url: url) } }
                        catch { await MainActor.run { failure = error.localizedDescription } }
                    }
                }
                if let failure { Text(failure).foregroundColor(.orange) }
            }
        }.navigationTitle("Desktop Logs").searchable(text: $search, prompt: "Search diagnostic messages")
            .toolbar { Button("Refresh", action: refresh) }.onAppear(perform: refresh)
            .sheet(item: $share) { ShareView(item: $0) }
    }
}
private struct DesktopLogsScreen: View {
    @Environment(\.dismiss) private var dismiss
    var body: some View { NavigationView { DesktopLogsContent().toolbar { Button("Done") { dismiss() } } }.navigationViewStyle(.stack) }
}

private struct DesktopPreferencesView: View {
    @State private var resolution = 3
    @AppStorage("LinuxRunnerDesktop.autoFullscreen") private var autoFullscreen = true
    @AppStorage("LinuxRunnerDesktop.metalPresentation") private var metalPresentation = false
    private var key: String { "LinuxRunnerDesktop.resolution." + LRRuntimeBridge.activeRootName() }
    var body: some View { Form {
        Section("Next Desktop Start") {
            Picker("Resolution", selection: $resolution) { Text("640 × 480").tag(0); Text("800 × 600").tag(1); Text("1024 × 768").tag(2); Text("960 × 540 · Wide").tag(3); Text("Fit Screen").tag(4) }
            Toggle("Immersive Landscape", isOn: $autoFullscreen)
            Text("Applies after saving documents and stopping the current desktop. The existing session is not resized or interrupted.").font(.caption)
        }
        Section("Interaction") {
            Text("Landscape uses the available display while preserving aspect ratio. Pinch to zoom; two fingers pan while zoomed, or scroll while fitted. Long press provides right click.")
            Text("Desktop Settings provides Fit, Actual Size and Full Screen. Use the restore button to bring controls back.")
        }
        Section("Framebuffer Presentation") {
            Toggle("Metal Presentation · Experimental", isOn: $metalPresentation)
            Text("Applies when reopening the viewer. Presents the received framebuffer through Metal, with Core Graphics fallback. Linux applications still use software rendering and the ARM64 interpreter. Physical-device performance has not yet been verified.").font(.caption)
        }
        Section("Execution") { Text("ARM64 interpreter; JIT is not required. The renderer for Linux applications is chosen in Settings › Graphics.").font(.caption) }
    }.navigationTitle("Display & Gestures").onAppear { resolution = UserDefaults.standard.object(forKey: key) == nil ? 3 : min(4, max(0, UserDefaults.standard.integer(forKey: key))) }.onChange(of: resolution) { UserDefaults.standard.set($0, forKey: key) } }
}
/// Graphics renderer for Linux applications, plus live evidence of what actually ran on the GPU.
// Folders the user picks in Files (iPhone, iCloud Drive, other providers or an
// external drive) appear in Linux at /mnt/<name>. Access is security-scoped and
// remembered with a bookmark; only folders picked here are reachable.
// Public host information only: no SSID entitlement, scan, probe or namespace.
@MainActor private final class HostNetworkStatus: ObservableObject {
    @Published var status = "Waiting for system path"
    @Published var connection = "Unknown"
    @Published var details = ""
    @Published var addresses: [String] = []
    @Published var availableInterfaces: [String] = []
    private var monitor: NWPathMonitor?
    private var generation: UInt64 = 0
    func start() {
        guard monitor == nil else { refreshAddresses(); return }
        generation &+= 1; let generation = self.generation
        let monitor = NWPathMonitor(); self.monitor = monitor
        monitor.pathUpdateHandler = { [weak self] path in
            let names: [(NWInterface.InterfaceType, String)] = [(.wifi,"Wi-Fi"),(.cellular,"Cellular"),(.wiredEthernet,"Ethernet"),(.loopback,"Loopback"),(.other,"Other / VPN")]
            let used = names.filter { path.usesInterfaceType($0.0) }.map { $0.1 }.joined(separator: ", ")
            let state = path.status == .satisfied ? "Available" : path.status == .requiresConnection ? "Connection required" : "Unavailable"
            let interfaces = path.availableInterfaces.map { interface in "\(interface.name) · index \(interface.index) · \(names.first(where: { $0.0 == interface.type })?.1 ?? "Other")" }.sorted()
            let details = "IPv4: \(path.supportsIPv4 ? "yes" : "no") · IPv6: \(path.supportsIPv6 ? "yes" : "no") · DNS: \(path.supportsDNS ? "yes" : "no")" + (path.isExpensive ? " · Metered" : "") + (path.isConstrained ? " · Low Data Mode" : "")
            DispatchQueue.main.async { [weak self] in
                guard let self = self, self.monitor != nil, self.generation == generation else { return }
                self.status = state; self.connection = used.isEmpty ? "No selected interface" : used
                self.details = details; self.availableInterfaces = interfaces; self.refreshAddresses()
            }
        }
        monitor.start(queue: DispatchQueue(label: "dev.linuxrunner.network-status"))
        refreshAddresses()
    }
    func stop() { generation &+= 1; monitor?.cancel(); monitor = nil }
    deinit { monitor?.cancel() }
    func refreshAddresses() {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0 else { addresses = ["Interface snapshot unavailable"]; return }
        defer { freeifaddrs(list) }
        var result = Set<String>(), cursor = list
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            guard let name = entry.pointee.ifa_name, let address = entry.pointee.ifa_addr,
                  address.pointee.sa_family == UInt8(AF_INET) || address.pointee.sa_family == UInt8(AF_INET6) else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(address, socklen_t(address.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                result.insert("\(String(cString: name)): \(String(cString: host))")
            }
        }
        addresses = result.sorted()
    }
}
private struct NetworkDiagnosticsView: View {
    @StateObject private var network = HostNetworkStatus()
    var body: some View {
        List {
            Section {
                HStack { Text("System path"); Spacer(); Text(network.status).foregroundStyle(.secondary) }
                HStack { Text("Connection"); Spacer(); Text(network.connection).foregroundStyle(.secondary) }
                Text(network.details).font(.caption)
            } header: { Text("iOS Network Path") } footer: { Text("Path availability is not a test of a particular server or of local-network permission. Changes are reported by iOS while this screen is open.") }
            Section {
                ForEach(network.addresses, id: \.self) { Text($0).font(.caption.monospaced()).textSelection(.enabled) }
                Button("Refresh Addresses", action: network.refreshAddresses)
            } header: { Text("Host Interface Addresses") } footer: { Text("These are real host addresses, shared with iOS and other apps; LinuxRunner has no separate network namespace. Review addresses before sharing a screenshot. No network requests are sent by this screen.") }
            Section("Interfaces Available to iOS") {
                ForEach(network.availableInterfaces, id: \.self) { Text($0).font(.caption.monospaced()) }
                Text("An Ethernet adapter recognized by iOS can carry LinuxRunner connections through the host route. This list does not identify its USB chipset or grant Linux direct adapter ownership. The active connection type appears above.").font(.caption).foregroundColor(.secondary)
            }
            Section {
                Text("Linux TCP and UDP sockets use host sockets. Linux interface queries expose a read-only compatibility view. Host routing and local-network permission remain controlled by iOS.")
                Text("Nearby Wi-Fi scanning and raw interface control are unavailable through ordinary public iOS APIs. This screen does not request location access or a Wi-Fi entitlement.")
            } header: { Text("Capabilities") }
        }.navigationTitle("Network").onAppear { network.start() }.onDisappear { network.stop() }
    }
}

// Linux programs' sound plays on the iPhone while a desktop or app session
// runs (PulseAudio in the session → loopback stream → AVAudioEngine).
// What external devices reach Linux, detected live through public iOS APIs.
// iOS gives apps no raw USB access, so each category says which path it uses.
private struct DevicesView: View {
    private struct Row: Identifiable { let id = UUID(); let name: String; let symbol: String; let detected: String; let linux: String; let works: Bool }
    @State private var rows: [Row] = []
    @State private var input: [String: Any] = [:]
    @AppStorage("LinuxRunnerInput.mouseMode") private var mouseMode = "auto"
    @AppStorage("LinuxRunnerDesktop.externalDisplay") private var externalOutput = true
    @State private var wirelessHelp = false
    private let timer = Timer.publish(every: 2, on: .main, in: .common).autoconnect()
    var body: some View {
        List {
            Section {
                ForEach(rows) { row in
                    VStack(alignment: .leading, spacing: 3) {
                        HStack { Image(systemName: row.symbol).frame(width: 24); Text(row.name).fontWeight(.semibold); Spacer(); Text(row.detected).font(.caption).foregroundStyle(.secondary) }
                        Text(row.linux).font(.caption).foregroundStyle(row.works ? Color.primary : Color.secondary)
                    }
                }
            }
            Section {
                Picker("Mouse Mode", selection: $mouseMode) {
                    Text("Automatic").tag("auto"); Text("Touch Gestures").tag("touch"); Text("Mouse Pointer").tag("pointer")
                }
                Text("Automatic uses the mouse pointer behaviour when a mouse, trackpad or AssistiveTouch is active: a click presses the Linux button at the pointer, holding and dragging works, double-click works. Touch Gestures keeps tap = click, long press = right click.").font(.caption).foregroundStyle(.secondary)
                ForEach(inputLines, id: \.self) { line in Text(line).font(.caption.monospaced()) }
            } header: { Text("Mouse and Pointer") } footer: { Text("Live input sources of the open desktop. Open the desktop, use the mouse, return here (or export the Open Desktop report) to see which events iOS delivered.") }
            Section {
                Toggle("External Desktop Output", isOn: $externalOutput)
                Button("Connect Wirelessly with AirPlay…") { wirelessHelp = true }
                Text("Shows the Linux desktop in a separate connected-display window. Keyboard, mouse and touch input stay on this device. Closing the viewer keeps output running while LinuxRunner stays in the foreground. Disconnecting returns control to this device; it does not stop Linux.").font(.caption).foregroundStyle(.secondary)
                Text("Display resolution is selected when a session starts. Stop and restart the desktop with Fit Screen selected to size it for a newly connected monitor. Software rendering is capped at 1600 × 1200.").font(.caption).foregroundStyle(.secondary)
                Text("For a connected separate display, choose Desktop → Use This Device as Touchpad. AirPlay mirroring alone shows the same screen on both devices.").font(.caption).foregroundStyle(.secondary)
            } header: { Text("External Monitor · USB-C or AirPlay") }
            Section {} footer: { Text("Linux cannot directly control raw USB devices. A network adapter recognized by iOS can carry LinuxRunner’s host-socket traffic; it does not become a guest-owned USB device. Devices iOS supports use the public interfaces listed above.") }
        }
        .navigationTitle("Devices").onAppear(perform: refresh).onReceive(timer) { _ in refresh() }
        .alert("Wireless Linux Desktop", isPresented: $wirelessHelp) {
            Button("OK", role: .cancel) { }
        } message: {
            Text("Connect to the same Wi-Fi as an AirPlay-compatible TV, Apple TV or Mac. Open iOS Control Center → Screen Mirroring and choose it. Then open the Linux desktop. A separate display uses External Desktop Output; otherwise choose Desktop → Wireless Display → Use Full-Screen Desktop. Receiver compatibility varies. Miracast-only receivers are not supported.")
        }
    }
    private var inputLines: [String] {
        var lines = ["pointer mode: \(input["pointer_mode"] ?? "?")", "AssistiveTouch: \((input["assistive_touch"] as? Bool ?? false) ? "on" : "off")",
                     "GameController mice: \(input["gc_mice"] ?? "none")", "latency (event → X11): \(input["latency_ms"] ?? "none")"]
        if let counts = input["counts"] as? [String: NSNumber] { lines += counts.keys.sorted().map { "\($0): \(counts[$0]!)" } }
        return lines
    }
    private func refresh() {
        input = LRRuntimeBridge.inputDiagnostics() as? [String: Any] ?? [:]
        let folders = LRRuntimeBridge.sharedFolders()
        let available = folders.filter { $0["available"] == "yes" }.count
        let outputs = AVAudioSession.sharedInstance().currentRoute.outputs.map { $0.portName }.joined(separator: ", ")
        let controllers = GCController.controllers().map { $0.vendorName ?? "Controller" }
        rows = [
            Row(name: "Keyboard", symbol: "keyboard", detected: GCKeyboard.coalesced != nil ? "Connected" : "None", linux: "Works: key presses go to the Linux desktop and terminal (USB and Bluetooth keyboards).", works: true),
            Row(name: "Mouse / Trackpad", symbol: "computermouse", detected: GCMouse.mice().isEmpty ? "None" : "Connected", linux: "Works through iOS pointer events: pointer, clicks and scrolling on the desktop. On iPhone, iOS provides a pointer only through AssistiveTouch.", works: true),
            Row(name: "USB Drives and Files Locations", symbol: "externaldrive", detected: folders.isEmpty ? "No shared folders" : "\(available) of \(folders.count) available", linux: "Works for folders you share in Settings › File Sharing › Add or Manage Shared Folders (pick a folder on the drive). They appear at /mnt/<name>.", works: true),
            Row(name: "Audio Output (incl. USB audio)", symbol: "hifispeaker", detected: outputs.isEmpty ? "None" : outputs, linux: "Works: Linux sound plays through whatever iOS currently uses, including USB audio interfaces and headphones.", works: true),
            Row(name: "Microphone (incl. USB)", symbol: "mic", detected: "—", linux: "Not available yet: Linux programs cannot record audio in this version.", works: false),
            Row(name: "Game Controllers", symbol: "gamecontroller", detected: controllers.isEmpty ? "None" : controllers.joined(separator: ", "), linux: "Detected by iOS, but not yet passed to Linux programs as joysticks.", works: false),
            Row(name: "Serial and Other Raw USB", symbol: "cable.connector", detected: "—", linux: "Unavailable: LinuxRunner has no public iOS interface for raw USB control.", works: false),
        ]
    }
}

// Terminal & Keyboard in LinuxRunner's own design, over the same stored
// terminal preferences the terminal reads (UserPreferences).
// Native screens opened from Objective-C (the terminal's settings button and
// Command-comma), replacing the inherited Linux iOS Kit settings storyboard.
@objc(LRNativeScreens) final class LRNativeScreens: NSObject {
    @objc static func terminalSettings() -> UIViewController {
        let host = UIHostingController(rootView: NavigationView { TerminalSettingsView() }.navigationViewStyle(.stack))
        host.modalPresentationStyle = .formSheet
        return host
    }
}

// Open-source components and their licenses (native; replaces the inherited
// About storyboard). Bundled notice files are shown verbatim.
private struct NoticesView: View {
    @Environment(\.dismiss) private var dismiss
    private let components: [(String, String)] = [
        ("iSH", "LinuxRunner's runtime is derived from iSH. GNU GPL version 3 (contributions after 0e3a4144 also GPL version 2), with the iSH App Store terms in LICENSE.IOS."),
        ("ios-linuxkit (Rui Carmo)", "LinuxRunner's direct upstream, itself derived from iSH. Same licence terms as iSH."),
        ("Linux kernel headers", "GNU GPL version 2 with the Linux syscall note."),
        ("xterm.js, hterm, ghostty-web", "Terminal frontends: MIT (xterm.js), BSD 3-Clause (hterm, The Chromium OS Authors), ghostty-web (see project notices)."),
        ("Fira Code and JetBrains Mono (Nerd Fonts)", "SIL Open Font License 1.1."),
        ("Alpine Linux minirootfs 3.24.2 (bundled)", "BusyBox, apk-tools, alpine-baselayout, scanelf: GPL-2.0; musl: MIT; OpenSSL: Apache-2.0; zlib: Zlib; ca-certificates: MPL-2.0 and MIT. Source: Alpine aports."),
        ("libarchive", "BSD 2-Clause License."),
        ("glslang and SPIRV-Cross", "BSD 3-Clause / Apache 2.0 (see their notices below)."),
        ("Mesa (guest graphics, installed from Debian)", "MIT License."),
        ("Debian and Alpine packages", "Installed from the distributions' official repositories under each package's own license (/usr/share/doc/<package>/copyright in Linux)."),
    ]
    private var bundled: [(String, String)] {
        guard let support = Bundle.main.url(forResource: "GUISupport", withExtension: "bundle") else { return [] }
        return ["gpu-bridge-notice.txt", "shader-compiler-notices.txt", "mesa-demos-notice.txt", "terminal-notices.txt"].compactMap { name in
            (try? String(contentsOf: support.appendingPathComponent(name))).map { (name, $0) }
        }
    }
    var body: some View {
        NavigationView {
            List {
                Section { ForEach(components, id: \.0) { item in VStack(alignment: .leading, spacing: 4) { Text(item.0).font(.headline); Text(item.1).font(.footnote).foregroundStyle(.secondary) } } }
                ForEach(bundled, id: \.0) { item in Section(item.0) { Text(item.1).font(.caption.monospaced()).textSelection(.enabled) } }
            }
            .navigationTitle("Notices & Credits")
            .toolbar { Button("Done") { dismiss() } }
        }.navigationViewStyle(.stack)
    }
}

private struct TerminalSettingsView: View {
    private let prefs = UserPreferences.shared()
    @State private var fontSize = 12.0
    @State private var cursor = 0
    @State private var blink = true
    @State private var scheme = 0
    @State private var capsLock = 0
    @State private var optionEscape = false
    @State private var backtickEscape = false
    @State private var controlSpace = false
    @State private var hideExtraKeys = false
    var body: some View {
        Form {
            Section("Appearance") {
                Stepper(value: $fontSize, in: 8...32, step: 1) { HStack { Text("Font Size"); Spacer(); Text("\(Int(fontSize)) pt").foregroundStyle(.secondary) } }
                    .onChange(of: fontSize) { prefs.fontSize = NSNumber(value: $0) }
                Picker("Cursor", selection: $cursor) { Text("Block").tag(0); Text("Bar").tag(1); Text("Underline").tag(2) }
                    .onChange(of: cursor) { if let v = CursorStyle(rawValue: $0) { prefs.cursorStyle = v } }
                Toggle("Blinking Cursor", isOn: $blink).onChange(of: blink) { prefs.blinkCursor = $0 }
                Picker("Colors", selection: $scheme) { Text("Match System").tag(0); Text("Light").tag(1); Text("Dark").tag(2) }
                    .onChange(of: scheme) { prefs.colorScheme = .init(rawValue: $0) ?? prefs.colorScheme }
            }
            Section {
                Picker("Caps Lock", selection: $capsLock) { Text("Caps Lock").tag(0); Text("Control").tag(1); Text("Escape").tag(2) }
                    .onChange(of: capsLock) { if let v = CapsLockMapping(rawValue: $0) { prefs.capsLockMapping = v } }
                Toggle("Option Sends Escape", isOn: $optionEscape).onChange(of: optionEscape) { prefs.optionMapping = $0 ? OptionMapEsc : OptionMapNone }
                Toggle("Backtick Sends Escape", isOn: $backtickEscape).onChange(of: backtickEscape) { prefs.backtickMapEscape = $0 }
                Toggle("Ctrl-Space Goes to Linux", isOn: $controlSpace).onChange(of: controlSpace) { prefs.overrideControlSpace = $0 }
                Toggle("Hide Extra Keys with Hardware Keyboard", isOn: $hideExtraKeys).onChange(of: hideExtraKeys) { prefs.hideExtraKeysWithExternalKeyboard = $0 }
            } header: { Text("Keyboard") } footer: { Text("The terminal's key bar provides Esc, Tab, Ctrl, Alt and arrow keys on screen. Changes apply to open terminals.") }
        }
        .navigationTitle("Terminal & Keyboard")
        .onAppear {
            fontSize = prefs.fontSize.doubleValue; cursor = prefs.cursorStyle.rawValue; blink = prefs.blinkCursor; scheme = prefs.colorScheme.rawValue
            capsLock = prefs.capsLockMapping.rawValue; optionEscape = prefs.optionMapping == OptionMapEsc; backtickEscape = prefs.backtickMapEscape
            controlSpace = prefs.overrideControlSpace; hideExtraKeys = prefs.hideExtraKeysWithExternalKeyboard
        }
    }
}

private struct AudioSettingsView: View {
    @AppStorage("LinuxRunnerAudio.enabled") private var enabled = true
    @State private var audio: [String: Any] = [:]
    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    var body: some View {
        List {
            Section {
                Toggle("Linux Sound", isOn: $enabled)
            } footer: { Text("Plays sound from Linux programs (browsers, media players, games) through the iPhone, mixed with other apps. Applies to the next desktop or app session. Sound stops while LinuxRunner is in the background.") }
            Section("Current Session") {
                valueRow("Status", value: audio["state"] as? String ?? "No running session")
                if let played = audio["played_ms"] as? Int, played > 0 {
                    valueRow("Played", value: String(format: "%.1f s", Double(played) / 1000))
                    valueRow("Dropped to keep latency low", value: String(format: "%.1f s", Double(audio["dropped_ms"] as? Int ?? 0) / 1000))
                    valueRow("Underruns", value: "\(audio["underruns"] as? Int ?? 0)")
                }
                if let error = audio["last_error"] as? String, !error.isEmpty { Text(error).font(.footnote).foregroundStyle(.orange) }
            }
            Section("Microphone") { Text("Not available yet: Linux programs cannot record from the iPhone microphone in this version.").font(.footnote).foregroundStyle(.secondary) }
        }
        .navigationTitle("Audio")
        .onAppear(perform: refresh).onReceive(timer) { _ in refresh() }
    }
    private func valueRow(_ title: String, value: String) -> some View { HStack { Text(title); Spacer(); Text(value).foregroundStyle(.secondary).multilineTextAlignment(.trailing) } }
    private func refresh() { audio = LRRuntimeBridge.desktopSessionSummary()["audio"] as? [String: Any] ?? [:] }
}

private struct SharedFoldersView: View {
    @State private var folders: [[String: String]] = LRRuntimeBridge.sharedFolders()
    @State private var picking = false
    @State private var message: String?
    @State private var pendingRemoval: [String: String]?
    @State private var checks: [String: String] = [:]
    @State private var checking: String?
    var body: some View {
        List {
            Section {
                Button { picking = true } label: { Label("Add Folder…", systemImage: "folder.badge.plus") }.disabled(!LRRuntimeBridge.isRunning())
                if !LRRuntimeBridge.isRunning() { Text("Start the Linux runtime to add folders.").font(.footnote).foregroundStyle(.secondary) }
            } footer: { Text("Linux programs see each folder at the path shown, with read and write access where Files allows it. New files there belong to the default Linux user. Removing a folder here only removes the link; the files stay where they are.") }
            Section("Shared Folders") {
                if folders.isEmpty { Text("No folders shared yet.").foregroundStyle(.secondary) }
                ForEach(folders, id: \.self) { folder in
                    VStack(alignment: .leading, spacing: 3) {
                        HStack { Image(systemName: folder["available"] == "yes" ? "folder.fill" : "exclamationmark.triangle.fill").foregroundStyle(folder["available"] == "yes" ? Color.accentColor : Color.orange); Text(folder["displayName"] ?? folder["name"] ?? "") }
                        Text(folder["mountPoint"] ?? "").font(.caption.monospaced()).textSelection(.enabled)
                        if folder["available"] != "yes" { Text("Unavailable: the folder was moved, its drive is disconnected, or access was revoked. Reconnect it, or remove and add it again.").font(.caption).foregroundStyle(.orange) }
                        if let result = checks[folder["name"] ?? ""] { Text(result).font(.caption.monospaced()).foregroundStyle(.secondary) }
                        Button(checking == folder["name"] ? "Checking…" : "Check Access") {
                            let name = folder["name"] ?? ""; checking = name
                            LRRuntimeBridge.checkSharedFolder(name) { summary in checks[name] = summary; checking = nil }
                        }.font(.caption).buttonStyle(.borderless).disabled(checking != nil || !LRRuntimeBridge.isRunning())
                    }.swipeActions { Button("Remove", role: .destructive) { pendingRemoval = folder } }
                }
            }
            if let message { Section { Text(message).font(.footnote) } }
        }
        .navigationTitle("Shared Folders")
        .onAppear { folders = LRRuntimeBridge.sharedFolders() }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else { return }
                do { try LRRuntimeBridge.addSharedFolder(url); folders = LRRuntimeBridge.sharedFolders(); message = "Shared at " + (folders.first { $0["path"] == url.path }?["mountPoint"] ?? "/mnt") + "." }
                catch { message = error.localizedDescription }
            case .failure(let error): message = error.localizedDescription
            }
        }
        .alert("Remove Shared Folder?", isPresented: Binding(get: { pendingRemoval != nil }, set: { if !$0 { pendingRemoval = nil } })) {
            Button("Cancel", role: .cancel) { pendingRemoval = nil }
            Button("Remove", role: .destructive) { if let name = pendingRemoval?["name"] { LRRuntimeBridge.removeSharedFolder(name) }; pendingRemoval = nil; folders = LRRuntimeBridge.sharedFolders() }
        } message: { Text("Linux programs lose access to \(pendingRemoval?["mountPoint"] ?? "this folder"). The files themselves are not deleted.") }
    }
}

private struct DesktopEnvironmentView: View {
    @AppStorage private var environment: String
    init() {
        let key = "LinuxRunnerDesktop.environment." + LRRuntimeBridge.activeRootName()
        let previous = LRRuntimeBridge.activeEnvironmentIsDebian() ? UserDefaults.standard.string(forKey: "LinuxRunnerDesktop.environment") : nil
        _environment = AppStorage(wrappedValue: previous ?? "linuxrunner", key)
    }
    @State private var installed = LRRuntimeBridge.plasmaInstalled()
    @State private var lxqtInstalled = LRRuntimeBridge.lxqtInstalled()
    @State private var enlightenmentInstalled = LRRuntimeBridge.enlightenmentInstalled()
    @State private var wayfireInstalled = LRRuntimeBridge.wayfireInstalled()
    @State private var hyprlandInstalled = LRRuntimeBridge.hyprlandInstalled()
    @State private var working: String?
    @State private var lxqtWorking: String?
    @State private var enlightenmentWorking: String?
    @State private var wayfireWorking: String?
    @State private var hyprlandWorking: String?
    @State private var failure: String?
    var body: some View { Form {
        Section {
            Picker("Desktop", selection: $environment) {
                Text("LinuxRunner Desktop").tag("linuxrunner")
                Text("LXQt").tag("lxqt").disabled(!lxqtInstalled)
                Text("Enlightenment").tag("enlightenment").disabled(!enlightenmentInstalled || LRRuntimeBridge.activeEnvironmentIsDebian())
                Text("Wayfire").tag("wayfire").disabled(!wayfireInstalled)
                Text("Hyprland · Experimental").tag("hyprland").disabled(!hyprlandInstalled)
                Text("KDE Plasma · Experimental").tag("plasma").disabled(!installed)
            }
            Text(environment == "hyprland"
                 ? "Hyprland (tiling Wayland compositor with animations and rounded windows; Waybar panel with an Apps launcher, foot terminal). Drawn on the CPU on LinuxRunner's virtual display device, so it is the most demanding desktop. If it is not installed, Open Desktop starts the LinuxRunner Desktop."
                 : environment == "wayfire"
                 ? "Wayfire (Wayland compositor with animations, window previews and workspaces; wf-shell panel and dock, foot terminal) drawn in software inside LinuxRunner's display. If it is not installed, Open Desktop starts the LinuxRunner Desktop."
                 : environment == "enlightenment"
                 ? "Enlightenment (X11 session with its own software compositor: shelf, main menu on a desktop click, Terminology terminal) from the distribution's own packages. If it is not installed, Open Desktop starts the LinuxRunner Desktop."
                 : environment == "lxqt"
                 ? "LXQt 2 (X11 session: LXQt panel and menu, PCManFM-Qt desktop, QTerminal, Openbox) from the distribution's own packages. Lightweight; if it is not installed, Open Desktop starts the LinuxRunner Desktop."
                 : environment == "plasma"
                 ? "KDE Plasma 6 (X11 session: KWin, Plasma panel and desktop, Dolphin, Konsole) from Debian's own packages. It needs more memory and starts more slowly than the LinuxRunner Desktop. If it is not installed, Open Desktop starts the LinuxRunner Desktop."
                 : LRRuntimeBridge.activeEnvironmentIsArch() ? "Arch X11 desktop: Openbox window manager, LXQt panel and PCManFM-Qt file manager." : "Openbox window manager, panel and file manager: the lightest and most tested desktop.").font(.caption)
        } header: { Text("Open Desktop Starts") } footer: { Text("Applies to the next desktop start. Stop a running desktop first to switch.") }
        Section {
            if installed { Label("KDE Plasma installed", systemImage: "checkmark.circle") }
            else if LRRuntimeBridge.activeEnvironmentIsArch() { Text("Arch's current Plasma 6.8 packages require Wayland. LinuxRunner currently supports X11; use LinuxRunner Desktop. Plasma on Arch has not been validated.").foregroundColor(.secondary) }
            else if !LRRuntimeBridge.activeEnvironmentIsDebian() { Text("KDE Plasma installation is available for Debian environments.").foregroundColor(.secondary) }
            else {
                Button(working == nil ? "Install KDE Plasma" : "Installing…") { install() }.disabled(working != nil || !LRRuntimeBridge.isRunning())
                if let working { Text(working).font(.caption).foregroundColor(.secondary) }
            }
            if let failure { Text(failure).font(.footnote).foregroundColor(.orange) }
        } header: { Text("KDE Plasma") } footer: {
            Text("About 550 official Debian packages (about 240 MB download, 930 MB installed); network needed once. The LinuxRunner Desktop stays installed as the fallback.")
        }
        Section {
            if lxqtInstalled { Label("LXQt installed", systemImage: "checkmark.circle") }
            else {
                Button(lxqtWorking == nil ? "Install LXQt" : "Installing…") { installLXQt() }.disabled(lxqtWorking != nil || working != nil || !LRRuntimeBridge.isRunning())
                if let lxqtWorking { Text(lxqtWorking).font(.caption).foregroundColor(.secondary) }
            }
        } header: { Text("LXQt") } footer: {
            Text("The LXQt desktop from the distribution's own packages; network needed once. The LinuxRunner Desktop stays installed as the fallback.")
        }
        Section {
            // Debian's Enlightenment shows only its splash, then a black screen
            // (same with the earlier runtime); Arch's works. Not offered on Debian.
            if LRRuntimeBridge.activeEnvironmentIsDebian() { Label("Not available on Debian", systemImage: "xmark.circle").foregroundColor(.secondary) }
            else if enlightenmentInstalled { Label("Enlightenment installed", systemImage: "checkmark.circle") }
            else {
                Button(enlightenmentWorking == nil ? "Install Enlightenment" : "Installing…") { installEnlightenment() }.disabled(enlightenmentWorking != nil || lxqtWorking != nil || working != nil || !LRRuntimeBridge.isRunning())
                if let enlightenmentWorking { Text(enlightenmentWorking).font(.caption).foregroundColor(.secondary) }
            }
        } header: { Text("Enlightenment") } footer: {
            Text(LRRuntimeBridge.activeEnvironmentIsDebian() ? "Enlightenment does not start correctly on Debian yet (it stops at its splash screen). Use LXQt or the LinuxRunner Desktop, or Enlightenment on Arch Linux ARM." : "A large download (mostly Enlightenment's themes and wallpapers); network needed once. The LinuxRunner Desktop stays installed as the fallback.")
        }
        Section {
            if wayfireInstalled { Label("Wayfire installed", systemImage: "checkmark.circle") }
            else if !(LRRuntimeBridge.activeEnvironmentIsDebian() || LRRuntimeBridge.activeEnvironmentIsArch()) { Text("Wayfire is available for Debian and Arch Linux ARM environments.").foregroundColor(.secondary) }
            else {
                Button(wayfireWorking == nil ? "Install Wayfire" : "Installing…") { installWayfire() }.disabled(wayfireWorking != nil || enlightenmentWorking != nil || lxqtWorking != nil || working != nil || !LRRuntimeBridge.isRunning())
                if let wayfireWorking { Text(wayfireWorking).font(.caption).foregroundColor(.secondary) }
            }
        } header: { Text("Wayfire") } footer: {
            Text("Wayland desktop from the distribution's packages (Arch: about 70 MB download); network needed once. Effects that need a GPU (cube, blur) are off. The LinuxRunner Desktop stays installed as the fallback.")
        }
        Section {
            if hyprlandInstalled { Label("Hyprland installed", systemImage: "checkmark.circle") }
            else if !LRRuntimeBridge.activeEnvironmentIsArch() { Text("Hyprland is packaged by Arch Linux ARM only.").foregroundColor(.secondary) }
            else {
                Button(hyprlandWorking == nil ? "Install Hyprland" : "Installing…") { installHyprland() }.disabled(hyprlandWorking != nil || wayfireWorking != nil || enlightenmentWorking != nil || lxqtWorking != nil || working != nil || !LRRuntimeBridge.isRunning())
                if let hyprlandWorking { Text(hyprlandWorking).font(.caption).foregroundColor(.secondary) }
            }
        } header: { Text("Hyprland · Experimental") } footer: {
            Text("Rendered on the CPU (no GPU acceleration yet); blur is off. Network needed once. The LinuxRunner Desktop stays installed as the fallback.")
        }
    }.navigationTitle("Desktop Environment").onAppear { installed = LRRuntimeBridge.plasmaInstalled(); lxqtInstalled = LRRuntimeBridge.lxqtInstalled(); enlightenmentInstalled = LRRuntimeBridge.enlightenmentInstalled(); wayfireInstalled = LRRuntimeBridge.wayfireInstalled(); hyprlandInstalled = LRRuntimeBridge.hyprlandInstalled() } }
    private func installHyprland() {
        failure = nil; hyprlandWorking = "Starting"
        LRRuntimeBridge.installHyprland({ hyprlandWorking = $0 }) { error in
            hyprlandWorking = nil; hyprlandInstalled = LRRuntimeBridge.hyprlandInstalled()
            if let error { failure = error.localizedDescription } else if hyprlandInstalled { environment = "hyprland" }
        }
    }
    private func installWayfire() {
        failure = nil; wayfireWorking = "Starting"
        LRRuntimeBridge.installWayfire({ wayfireWorking = $0 }) { error in
            wayfireWorking = nil; wayfireInstalled = LRRuntimeBridge.wayfireInstalled()
            if let error { failure = error.localizedDescription } else if wayfireInstalled { environment = "wayfire" }
        }
    }
    private func installEnlightenment() {
        failure = nil; enlightenmentWorking = "Starting"
        LRRuntimeBridge.installEnlightenment({ enlightenmentWorking = $0 }) { error in
            enlightenmentWorking = nil; enlightenmentInstalled = LRRuntimeBridge.enlightenmentInstalled()
            if let error { failure = error.localizedDescription } else if enlightenmentInstalled { environment = "enlightenment" }
        }
    }
    private func installLXQt() {
        failure = nil; lxqtWorking = "Starting"
        LRRuntimeBridge.installLXQt({ lxqtWorking = $0 }) { error in
            lxqtWorking = nil; lxqtInstalled = LRRuntimeBridge.lxqtInstalled()
            if let error { failure = error.localizedDescription } else if lxqtInstalled { environment = "lxqt" }
        }
    }
    private func install() {
        failure = nil; working = "Starting"
        LRRuntimeBridge.installPlasma({ working = $0 }) { error in
            working = nil; installed = LRRuntimeBridge.plasmaInstalled()
            if let error { failure = error.localizedDescription } else if installed { environment = "plasma" }
        }
    }
}
private struct GraphicsSettingsView: View {
    @AppStorage("LinuxRunnerGraphics.mode") private var mode = "mesa"
    @State private var gpuMode = LRRuntimeBridge.gpuMode()
    @State private var gpu = LRRuntimeBridge.gpuStatus() as? [String: Any] ?? [:]
    @AppStorage("LinuxRunner.developerMode") private var developerMode = false
    @State private var summary: [String: Any] = [:]
    private let refresh = Timer.publish(every: 2, on: .main, in: .common).autoconnect()
    private var processes: [[String: String]] { (summary["processes"] as? [[String: String]]) ?? [] }
    var body: some View { Form {
        Section {
            Picker("Renderer", selection: $mode) {
                Text("Software (Mesa)").tag("mesa")
                Text("GPU-Resident Metal · Experimental").tag("mesa-metal-resident")
                if developerMode || mode == "mesa-metal-verify" { Text("Metal Verification · Diagnostics").tag("mesa-metal-verify") }
            }
            Text(description).font(.caption)
        } header: { Text("Linux Applications") } footer: {
            Text("Applies to the next desktop start and to applications opened from LinuxRunner. A launcher with its own graphics choice keeps it. Browsers keep software rendering.")
        }
        if LRRuntimeBridge.virglCompiledIn() {
            Section {
                Picker("GPU", selection: $gpuMode) {
                    Text("Automatic").tag("automatic")
                    Text("Metal GPU · Experimental").tag("metal")
                    Text("Software Rendering").tag("software")
                }.onChange(of: gpuMode) { LRRuntimeBridge.setGPUMode($0); gpu = LRRuntimeBridge.gpuStatus() as? [String: Any] ?? [:] }
                HStack { Text("Active renderer"); Spacer(); Text(gpu["active"] as? String ?? "Software").foregroundColor((gpu["device"] as? Bool ?? false) ? .green : .secondary) }
                Text(gpu["detail"] as? String ?? "").font(.caption).foregroundColor(.secondary)
                if (gpu["restartNeeded"] as? Bool ?? false) || (gpuMode != "metal" && (gpu["device"] as? Bool ?? false)) {
                    Text("Takes effect after LinuxRunner restarts.").font(.caption).foregroundColor(.orange)
                }
            } header: { Text("Linux GPU Device") } footer: {
                Text("Metal GPU gives Linux a virtual GPU: Mesa's own virgl driver renders on the iPhone GPU through Metal (ANGLE). Today Hyprland composites on it; other desktops keep software rendering. If the GPU does not start, LinuxRunner falls back to software and shows why. Automatic uses software rendering until Metal is validated on iPhone.")
            }
        }
        Section("Current Session") {
            if summary["session"] as? Bool == true {
                Text(routingText).font(.callout)
            } else { Text("No GUI session has started since LinuxRunner opened.").foregroundColor(.secondary) }
        }
        if !processes.isEmpty {
            Section {
                ForEach(processes.indices, id: \.self) { index in ProcessGraphicsRow(values: processes[index]) }
            } header: { Text("OpenGL Processes") } footer: {
                Text("Metal draws are guest OpenGL draws executed by the GPU. Mesa draws were rasterised in software. Presenting the desktop through Metal is not counted as acceleration.")
            }
        }
    }.navigationTitle("Graphics").onAppear { summary = LRRuntimeBridge.graphicsSummary() }.onReceive(refresh) { _ in summary = LRRuntimeBridge.graphicsSummary() } }
    private var description: String {
        switch mode {
        case "mesa-metal-resident": return "Supported OpenGL draws run on the iPhone GPU through Metal, with images kept on the GPU between draws. Unsupported operations fall back to software Mesa. Tested on Mac with Blender; device results are still being collected."
        case "mesa-metal-verify": return "Development diagnostics: every Metal draw is compared with Mesa. Much slower; use only to investigate rendering differences."
        default: return "All Linux graphics are rendered by Mesa on the CPU. Most compatible; slowest for 3D applications."
        }
    }
    private var routingText: String {
        guard let line = summary["routing"] as? String else { return "Waiting for the session to report its renderer…" }
        let fields = Dictionary(uniqueKeysWithValues: line.split(separator: " ").compactMap { part -> (String, String)? in
            let pair = part.split(separator: "=", maxSplits: 1); return pair.count == 2 ? (String(pair[0]), String(pair[1])) : nil })
        let active = fields["active"] ?? "?"
        if let reason = fields["reason"] { return "Requested \(fields["requested"] ?? "?"), using software: " + reason.replacingOccurrences(of: "-", with: " ") }
        return active == "mesa" ? "Software rendering (Mesa)" : (active == "mesa-metal-resident" ? "GPU-resident Metal routing active" : "Metal verification routing active")
    }
}
private struct ProcessGraphicsRow: View {
    let values: [String: String]
    private func number(_ key: String) -> Double { Double(values[key] ?? "") ?? 0 }
    var body: some View {
        let metal = number("metal_draws"), mesa = number("mesa_draws"), total = metal + mesa
        let age = Date().timeIntervalSince1970 - number("updated_unix")
        VStack(alignment: .leading, spacing: 4) {
            HStack { Text(values["command"] ?? "process").font(.headline); Spacer(); Text(age < 5 ? String(format: "%.1f fps", number("fps")) : "idle").foregroundColor(.secondary) }
            ProgressView(value: total > 0 ? metal / total : 0).tint(.green)
            Text(String(format: "GPU %@ draws · software %@ (%.0f%% on Metal)", format(metal), format(mesa), total > 0 ? 100 * metal / total : 0)).font(.caption)
            Text(String(format: "Transfers: %@ uploads (%.1f MB), %@ readbacks (%.1f MB) · GPU %.0f ms", format(number("uploads")), number("upload_bytes") / 1_048_576, format(number("writebacks")), number("writeback_bytes") / 1_048_576, number("gpu_ms"))).font(.caption).foregroundColor(.secondary)
            Text("Shaders: \(values["program_compiles"] ?? "0") compiled, \(values["program_cache_hits"] ?? "0") cache hits").font(.caption).foregroundColor(.secondary)
            if let reasons = values["reasons"], reasons != "none" { Text("Main fallbacks: " + reasons.replacingOccurrences(of: ",", with: ", ")).font(.caption2).foregroundColor(.secondary).lineLimit(3) }
        }.accessibilityElement(children: .combine)
    }
    private func format(_ value: Double) -> String { value >= 10_000 ? String(format: "%.0fk", value / 1000) : String(format: "%.0f", value) }
}
private struct LinuxRunnerAboutView: View {
    @State private var notices = false
    var body: some View { List {
        Section { Text("LinuxRunner").font(.system(size: 30, weight: .bold, design: .rounded)); Text("Local Linux ARM64 userspace for iPhone.").foregroundColor(.secondary) }
        Section("Developer Build") {
            Text("Version \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown") (\(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "unknown"))")
            Text("Source: " + (Bundle.main.object(forInfoDictionaryKey: "LinuxRunnerSourceCommit") as? String ?? "unknown")).font(.caption).textSelection(.enabled)
            Text("Linux userspace → ARM64 interpreter → Linux ABI compatibility → iOS/XNU").font(.caption)
        }
        Section { Button("Third-Party Notices & Credits") { notices = true }; Text("LinuxRunner includes open-source components. Their licenses and attribution remain available; no kernel or virtual machine is booted.").font(.caption).foregroundColor(.secondary) }
    }.navigationTitle("About").sheet(isPresented: $notices) { NoticesView() } }
}

struct GuestCrashListView: View {
    @State private var reports: [GuestCrashReport] = []
    var body: some View {
        List {
            if reports.isEmpty { Text("No guest application failures recorded.").foregroundColor(.secondary) }
            ForEach(reports) { report in
                NavigationLink(destination: GuestCrashDetailView(report: report)) {
                    VStack(alignment: .leading, spacing: 5) {
                        Text(report.summary).font(.subheadline)
                        Text(report.timestamp, style: .date).font(.caption).foregroundColor(.secondary)
                        Text("PID \(report.metadata["pid"] ?? "?") · TID \(report.metadata["tid"] ?? "?")").font(.caption).foregroundColor(.secondary)
                    }
                }
            }
            Section { Text(GuestCrashCenter.storageStatus()).font(.footnote).foregroundColor(.secondary); Text("Keeps the latest 20 failures across restarts. Handled guest signals are not crashes. Nonzero exits are labeled separately. Reports omit arbitrary application output and webpage contents.").font(.footnote).foregroundColor(.secondary) }
        }.navigationTitle("Application Crashes")
            .onAppear { reports = GuestCrashCenter.reports() }
            .onReceive(NotificationCenter.default.publisher(for: Notification.Name("LRGuestCrashRecorded"))) { _ in reports = GuestCrashCenter.reports() }
    }
}
struct GuestCrashDetailView: View {
    let report: GuestCrashReport
    var dismissible = false
    @Environment(\.dismiss) private var dismiss
    @State private var share: ExportItem?
    @State private var error: String?
    var body: some View {
        List {
            Section {
                Text(report.summary).font(.headline).textSelection(.enabled)
                Button("Copy Error") { UIPasteboard.general.string = report.summary }
                Button("Copy Full Report") { UIPasteboard.general.string = report.fullText }
                Button("Share Report") { do { share = ExportItem(url: try GuestCrashCenter.export(report)) } catch { self.error = error.localizedDescription } }
                if let error = error { Text(error).foregroundColor(.red) }
            }
            Section("Full Report") { Text(report.fullText).font(.system(.caption, design: .monospaced)).textSelection(.enabled) }
        }.navigationTitle("Crash Details")
            .toolbar { if dismissible { Button("Done") { dismiss() } } }
            .sheet(item: $share) { ShareView(item: $0) }
    }
}

#if targetEnvironment(simulator)
/// Simulator-only workflow driver for reproducing user sequences without
/// touches: LR_SIM_SCRIPT="wait:10;desktop;wait:60;report;close;guitest;…".
/// Calls the same functions as the buttons; never compiled for devices.
enum SimulatorWorkflow {
    private static var started = false
    private static var reports = 0
    private static var installer: DebianInstaller?
    static func start(desktop: @escaping () -> Void, guiTest: @escaping () -> Void, manager: EnvironmentManager,
                      terminal: @escaping () -> Void = {}, type: @escaping (String) -> Void = { _ in }, tab: @escaping (Int) -> Void = { _ in }) {
        guard !started, let script = ProcessInfo.processInfo.environment["LR_SIM_SCRIPT"] else { return }
        started = true
        var delay = 0.0
        for step in script.split(separator: ";").map(String.init) {
            if step.hasPrefix("wait:") { delay += Double(step.dropFirst(5)) ?? 0; continue }
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                LRRuntimeBridge.desktopTrace("simulator", result: "info", detail: "workflow step \(step)")
                if step.hasPrefix("tab:") { tab(Int(step.dropFirst(4)) ?? 0); return }
                if step.hasPrefix("type:") { type(String(step.dropFirst(5)) + "\n"); return }
                switch step {
                case "network-contract":
                    let model = HostNetworkStatus(); LRRuntimeBridge.desktopTrace("network-test", result: "info", detail: "before start"); model.start(); LRRuntimeBridge.desktopTrace("network-test", result: "info", detail: "after start")
                    DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
                        var result: [String: Any] = ["environment": "iOS Simulator native public Network/getifaddrs APIs", "status": model.status == "Waiting for system path" || model.addresses.isEmpty ? "FAIL" : "PASS", "path": model.status, "connection": model.connection, "address_count": model.addresses.count, "available_interface_count": model.availableInterfaces.count, "details": model.details, "physical_device": false]
                        let abi = LRRuntimeBridge.networkABIContract(); result["linuxABI"] = abi
                        if abi["status"] as? String != "PASS" { result["status"] = "FAIL" }
                        LRRuntimeBridge.desktopTrace("network-test", result: "info", detail: "timer fired"); model.stop(); LRRuntimeBridge.desktopTrace("network-test", result: "info", detail: "monitor stopped")
                        if let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first,
                           let data = try? JSONSerialization.data(withJSONObject: result, options: .prettyPrinted) {
                            let url = documents.appendingPathComponent("Diagnostics/network-contract.json")
                            try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                            try? data.write(to: url, options: .atomic)
                        }
                    }
                case "desktop-files-contract":
                    LRRuntimeBridge.runDesktopFilesContract { result in
                        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first, let data = try? JSONSerialization.data(withJSONObject: result, options: .prettyPrinted) else { return }
                        let url = documents.appendingPathComponent("Diagnostics/desktop-files-contract.json")
                        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                        try? data.write(to: url, options: .atomic)
                    }
                case "external-contract":
                    LRRuntimeBridge.runExternalDisplayContract { result in
                        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first,
                              let data = try? JSONSerialization.data(withJSONObject: result, options: .prettyPrinted) else { return }
                        let url = documents.appendingPathComponent("Diagnostics/external-display-contract.json")
                        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                        try? data.write(to: url, options: .atomic)
                        LRRuntimeBridge.desktopTrace("simulator", result: result["status"] as? String == "PASS" ? "ok" : "fail", detail: "external display component contract completed")
                    }
                case "terminal": terminal()
                case "taskmanager":
                    // The desktop menu's Task Manager, over whatever is in front.
                    var top = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.keyWindow }.first?.rootViewController
                    while let next = top?.presentedViewController { top = next }
                    top?.present(LinuxRunnerShell.makeTaskManagerController(), animated: true)
                case "jit-status", "gpu-status":
                    // Settings › Execution Engine or › Graphics, over whatever is in front.
                    var top = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.keyWindow }.first?.rootViewController
                    while let next = top?.presentedViewController { top = next }
                    let page: AnyView = step == "jit-status" ? AnyView(ExecutionEngineView()) : AnyView(GraphicsSettingsView())
                    let host = UIHostingController(rootView: NavigationView { page }.navigationViewStyle(.stack))
                    host.overrideUserInterfaceStyle = .dark
                    top?.present(host, animated: true)
                case "desktop": desktop()
                case "guitest": guiTest()
                case "install-debian":
                    // The app's own Debian installer (download, verify, bootstrap), then
                    // selected for the next launch, as the setup wizard does.
                    let debian = DebianInstaller(); installer = debian
                    debian.install { environment in
                        manager.acceptInstalled(environment); LRRuntimeBridge.setNextRootName(environment.rootfsName)
                        LRRuntimeBridge.desktopTrace("simulator", result: "ok", detail: "debian installed root=\(environment.rootfsName)")
                    }
                case "close": NotificationCenter.default.post(name: Notification.Name("LRSimulatorClose"), object: nil)
                case "retry": NotificationCenter.default.post(name: Notification.Name("LRSimulatorRetry"), object: nil)
                case "share":
                    // Same call the Files picker result goes to (the picker UI itself is skipped).
                    if let path = ProcessInfo.processInfo.environment["LR_SIM_SHARE_PATH"] {
                        do { try LRRuntimeBridge.addSharedFolder(URL(fileURLWithPath: path, isDirectory: true)); LRRuntimeBridge.desktopTrace("simulator", result: "ok", detail: "shared \(path)") }
                        catch { LRRuntimeBridge.desktopTrace("simulator", result: "fail", detail: "share failed: \(error.localizedDescription)") }
                    }
                case "user":
                    // A normal default Linux user, as on the iPhone (account tools install if missing).
                    let name = ProcessInfo.processInfo.environment["LR_SIM_USER"] ?? "lrsim"
                    LRRuntimeBridge.createLinuxUser(name, password: "lrsim-password", administrator: false, progress: { _ in }) { error in
                        if let error { LRRuntimeBridge.desktopTrace("simulator", result: "fail", detail: "user failed: \(error.localizedDescription)"); return }
                        try? LRRuntimeBridge.setDefaultLinuxUser(name); LRRuntimeBridge.desktopTrace("simulator", result: "ok", detail: "default user \(name)")
                    }
                case "checkshare":
                    for folder in LRRuntimeBridge.sharedFolders() {
                        let name = folder["name"] ?? ""
                        LRRuntimeBridge.checkSharedFolder(name) { summary in
                            guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
                            let line = "\(folder["mountPoint"] ?? "") available=\(folder["available"] ?? "?")\n\(summary)\n"
                            let file = documents.appendingPathComponent("Diagnostics/share-check-\(name).txt")
                            try? line.write(to: file, atomically: true, encoding: .utf8)
                            LRRuntimeBridge.desktopTrace("simulator", result: "info", detail: "share check \(name) done")
                        }
                    }
                case "report":
                    LRRuntimeBridge.exportDesktopReport { url in
                        guard let url, let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
                        reports += 1
                        let destination = documents.appendingPathComponent("Diagnostics/simulator-report-\(reports).txt")
                        try? FileManager.default.removeItem(at: destination)
                        try? FileManager.default.copyItem(at: url, to: destination)
                    }
                default: break
                }
            }
            delay += 0.5
        }
    }
}
#endif

// MARK: - Installation progress (account tools, desktop components, KDE Plasma)
// LRRuntimeBridge posts LRInstallProgressNotification from the real installer
// state: stage lines, Debian's configured/total packages, apt's own percentages.
// The card shows a percentage only when the installer measured one.
final class InstallMonitor: ObservableObject {
    struct State {
        var title: String, stage: String, detail: String, fraction: Double, elapsed: Double, phase: String, error: String?, log: String?
    }
    static let shared = InstallMonitor()
    @Published var state: State?
    private var hide: DispatchWorkItem?
    private init() {
        NotificationCenter.default.addObserver(forName: Notification.Name("LRInstallProgressNotification"), object: nil, queue: .main) { [weak self] note in
            guard let self, let info = note.userInfo else { return }
            let state = State(title: info["title"] as? String ?? "Installing", stage: info["stage"] as? String ?? "",
                              detail: info["detail"] as? String ?? "", fraction: (info["fraction"] as? NSNumber)?.doubleValue ?? -1,
                              elapsed: (info["elapsed"] as? NSNumber)?.doubleValue ?? 0, phase: info["phase"] as? String ?? "running",
                              error: info["error"] as? String, log: info["log"] as? String)
            self.hide?.cancel()
            self.state = state
            if state.phase == "succeeded" {
                let work = DispatchWorkItem { [weak self] in if self?.state?.phase == "succeeded" { self?.state = nil } }
                self.hide = work
                DispatchQueue.main.asyncAfter(deadline: .now() + 4, execute: work)
            }
        }
    }
    func dismiss() { state = nil }
}

struct InstallProgressCard: View {
    @ObservedObject private var monitor = InstallMonitor.shared
    @State private var showLog = false
    private func clock(_ seconds: Double) -> String { String(format: "%d:%02d", Int(seconds) / 60, Int(seconds) % 60) }
    var body: some View {
        if let state = monitor.state {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Image(systemName: state.phase == "failed" ? "exclamationmark.triangle.fill" : state.phase == "succeeded" ? "checkmark.circle.fill" : "arrow.down.circle")
                        .foregroundColor(state.phase == "failed" ? .orange : state.phase == "succeeded" ? .green : .accentColor)
                        .accessibilityHidden(true)
                    Text(state.title).font(.headline)
                    Spacer()
                    Text(clock(state.elapsed)).font(.caption.monospacedDigit()).foregroundColor(.secondary).accessibilityLabel("Elapsed \(Int(state.elapsed)) seconds")
                }
                if state.phase == "succeeded" {
                    Text("Installation complete").font(.subheadline)
                } else {
                    Text(state.phase == "failed" ? "Stopped at: \(state.stage)" : "Current step: \(state.stage)").font(.subheadline)
                    if state.phase == "running" {
                        if state.fraction >= 0 {
                            ProgressView(value: state.fraction).accessibilityValue("\(Int(state.fraction * 100)) percent")
                            Text("\(Int(state.fraction * 100)) %").font(.caption.monospacedDigit()).foregroundColor(.secondary)
                        } else {
                            ProgressView().frame(maxWidth: .infinity, alignment: .leading)
                        }
                        if !state.detail.isEmpty { Text(state.detail).font(.caption).foregroundColor(.secondary).lineLimit(2) }
                        Text("Please keep LinuxRunner open.").font(.caption).foregroundColor(.secondary)
                    }
                    if let error = state.error {
                        Text(error).font(.footnote).foregroundColor(.orange).fixedSize(horizontal: false, vertical: true)
                        if let log = state.log, !log.isEmpty {
                            Button(showLog ? "Hide Details" : "Show Details") { showLog.toggle() }.font(.footnote)
                            if showLog { ScrollView { Text(log).font(.caption2.monospaced()).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }.frame(maxHeight: 160) }
                        }
                        Button("Dismiss") { monitor.dismiss() }.font(.footnote)
                    }
                }
            }
            .padding(14)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .padding(.horizontal, 12).padding(.bottom, 56)
            .accessibilityElement(children: .contain)
            .transition(.move(edge: .bottom).combined(with: .opacity))
        }
    }
}

// MARK: - Task Manager
// Live guest processes from the runtime's own process table (LRRuntimeBridge
// taskSnapshot). CPU: change in each process's user+system time over the
// sampling interval, as a percentage of one CPU core (host-measured thread
// time). Memory: private anonymous guest pages per process (shared mappings
// are listed separately because every process mapping them would count them),
// plus the whole app's footprint as iOS accounts it. No value is estimated.
// The app's own dark style: black background, iOS dark grouped cards, white
// text and the app accent blue.
private enum TaskStyle {
    static let base = Color.black
    static let panel = Color(red: 0x1c / 255, green: 0x1c / 255, blue: 0x1e / 255)
    static let line = Color(red: 0x2c / 255, green: 0x2c / 255, blue: 0x2e / 255)
    static let text = Color.white
    static let dim = Color(red: 0x8e / 255, green: 0x8e / 255, blue: 0x93 / 255)
    static let frost = Color(red: 0.36, green: 0.68, blue: 0.98)
    static let blue = Color(red: 0.12, green: 0.30, blue: 0.52)
    static let green = Color(red: 0x30 / 255, green: 0xd1 / 255, blue: 0x58 / 255)
    static let yellow = Color(red: 0xff / 255, green: 0xd6 / 255, blue: 0x0a / 255)
    static let red = Color(red: 0xff / 255, green: 0x45 / 255, blue: 0x3a / 255)
}

private struct GuestProcess: Identifiable {
    let id: Int, ppid: Int, uid: Int, name: String, state: String, threads: Int
    let cpuMs: Int, privateKB: Int, sharedKB: Int, mappedKB: Int
    var cpuPercent: Double = 0
    init(_ d: [String: Any]) {
        id = d["pid"] as? Int ?? 0; ppid = d["ppid"] as? Int ?? 0; uid = d["uid"] as? Int ?? 0
        name = d["name"] as? String ?? "?"; state = d["state"] as? String ?? "?"; threads = d["threads"] as? Int ?? 0
        cpuMs = d["cpu_ms"] as? Int ?? 0; privateKB = d["anonymous_kb"] as? Int ?? 0
        sharedKB = d["shared_kb"] as? Int ?? 0; mappedKB = d["mapped_kb"] as? Int ?? 0
    }
    var stateName: String { ["R": "Running", "S": "Sleeping", "T": "Stopped", "Z": "Zombie"][state] ?? state }
}

@MainActor private final class TaskSampler: ObservableObject {
    @Published var processes: [GuestProcess] = []
    @Published var cpuHistory: [Double] = []
    @Published var memoryHistory: [Double] = []
    @Published var footprint: UInt64 = 0
    private var previous: [Int: Int] = [:]
    private var previousTime = Date()
    static let interval = 2.0
    func sample() {
        let now = Date(), elapsedMs = max(now.timeIntervalSince(previousTime) * 1000, 1)
        var list = LRRuntimeBridge.taskSnapshot().map(GuestProcess.init)
        for i in list.indices {
            if let before = previous[list[i].id] { list[i].cpuPercent = max(0, Double(list[i].cpuMs - before) / elapsedMs * 100) }
        }
        previous = Dictionary(uniqueKeysWithValues: list.map { ($0.id, $0.cpuMs) })
        previousTime = now
        processes = list
        footprint = LRRuntimeBridge.appFootprintBytes()
        cpuHistory = Array((cpuHistory + [list.reduce(0) { $0 + $1.cpuPercent }]).suffix(60))
        memoryHistory = Array((memoryHistory + [Double(list.reduce(0) { $0 + $1.privateKB }) / 1024]).suffix(60))
    }
}

private struct Sparkline: View {
    let values: [Double]; let color: Color
    var body: some View {
        GeometryReader { geo in
            let top = max(values.max() ?? 1, 1)
            Path { p in
                for (i, v) in values.enumerated() {
                    let point = CGPoint(x: geo.size.width * CGFloat(i) / CGFloat(max(values.count - 1, 1)),
                                        y: geo.size.height * (1 - CGFloat(v / top)))
                    if i == 0 { p.move(to: point) } else { p.addLine(to: point) }
                }
            }.stroke(color, style: StrokeStyle(lineWidth: 2, lineJoin: .round))
        }
    }
}

struct TaskManagerView: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var sampler = TaskSampler()
    @State private var tab = 0
    @State private var search = ""
    @State private var sort = 0   // 0 CPU, 1 memory, 2 PID, 3 name
    @State private var selected: GuestProcess?
    private let timer = Timer.publish(every: TaskSampler.interval, on: .main, in: .common).autoconnect()

    private var shown: [GuestProcess] {
        let filtered = search.isEmpty ? sampler.processes : sampler.processes.filter { $0.name.localizedCaseInsensitiveContains(search) || String($0.id) == search }
        switch sort {
        case 1: return filtered.sorted { $0.privateKB > $1.privateKB }
        case 2: return filtered.sorted { $0.id < $1.id }
        case 3: return filtered.sorted { $0.name.lowercased() < $1.name.lowercased() }
        default: return filtered.sorted { $0.cpuPercent > $1.cpuPercent }
        }
    }
    var body: some View {
        NavigationView {
            ZStack {
                TaskStyle.base.ignoresSafeArea()
                VStack(spacing: 12) {
                    Picker("", selection: $tab) { Text("Overview").tag(0); Text("Processes").tag(1); Text("Performance").tag(2) }
                        .pickerStyle(.segmented).padding(.horizontal)
                    switch tab {
                    case 1: processList
                    case 2: performance
                    default: overview
                    }
                }.padding(.top, 8)
            }
            .navigationTitle("Task Manager").navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
        .navigationViewStyle(.stack)
        .onAppear { sampler.sample() }
        .onReceive(timer) { _ in sampler.sample() }
        .sheet(item: $selected) { ProcessDetailView(process: $0, sampler: sampler) }
    }

    private func card<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title.uppercased()).font(.caption2.weight(.semibold)).foregroundColor(TaskStyle.dim)
            content()
        }
        .frame(maxWidth: .infinity, alignment: .leading).padding(14)
        .background(RoundedRectangle(cornerRadius: 14).fill(TaskStyle.panel))
    }
    private func metric(_ value: String, _ label: String, _ color: Color = TaskStyle.text) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.system(.title2, design: .rounded).weight(.semibold)).foregroundColor(color)
            Text(label).font(.caption).foregroundColor(TaskStyle.dim)
        }
    }
    private static func bytes(_ kb: Double) -> String { kb >= 1024 * 1024 ? String(format: "%.1f GB", kb / 1048576) : kb >= 1024 ? String(format: "%.0f MB", kb / 1024) : String(format: "%.0f KB", kb) }

    private var overview: some View {
        let execution = LRRuntimeBridge.executionBackendStatus()
        let jit = (execution["active"] as? String) == "jit"
        return ScrollView { VStack(spacing: 12) {
            card("Linux") {
                HStack(spacing: 24) {
                    metric(String(format: "%.0f%%", sampler.cpuHistory.last ?? 0), "CPU (100% = 1 core)", TaskStyle.frost)
                    metric(Self.bytes(Double(sampler.processes.reduce(0) { $0 + $1.privateKB })), "Private guest memory", TaskStyle.green)
                }
                HStack(spacing: 24) {
                    metric("\(sampler.processes.count)", "Processes")
                    metric("\(sampler.processes.reduce(0) { $0 + $1.threads })", "Threads")
                }
            }
            card("LinuxRunner") {
                metric(sampler.footprint > 0 ? Self.bytes(Double(sampler.footprint) / 1024) : "Unavailable", "App memory (iOS footprint, includes the runtime)", TaskStyle.yellow)
                row("Distribution", LRRuntimeBridge.activeRootName() ?? "None")
                row("Execution", (LRRuntimeBridge.jitStatusReport() as? [String: Any])?["state"] as? String ?? (jit ? "JIT" : "Interpreter"))
                row("Graphics", (LRRuntimeBridge.gpuStatus() as? [String: Any])?["active"] as? String ?? "Software")
                row("GPU activity", "Unavailable")
            }
        }.padding(.horizontal) }
    }
    private func row(_ label: String, _ value: String) -> some View {
        HStack { Text(label).foregroundColor(TaskStyle.dim); Spacer(); Text(value).foregroundColor(TaskStyle.text) }.font(.callout)
    }

    private var processList: some View {
        VStack(spacing: 8) {
            HStack {
                TextField("Search name or PID", text: $search).textFieldStyle(.roundedBorder)
                Menu { Picker("Sort", selection: $sort) { Text("CPU").tag(0); Text("Memory").tag(1); Text("PID").tag(2); Text("Name").tag(3) } }
                    label: { Image(systemName: "arrow.up.arrow.down").foregroundColor(TaskStyle.frost) }
            }.padding(.horizontal)
            List(shown) { p in
                Button { selected = p } label: {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(p.name).foregroundColor(TaskStyle.text).font(.body.weight(.medium))
                            Text("PID \(p.id) · \(p.stateName) · \(p.threads) thread\(p.threads == 1 ? "" : "s")").font(.caption).foregroundColor(TaskStyle.dim)
                        }
                        Spacer()
                        VStack(alignment: .trailing, spacing: 2) {
                            Text(String(format: "%.1f%%", p.cpuPercent)).foregroundColor(p.cpuPercent > 50 ? TaskStyle.yellow : TaskStyle.frost).font(.callout.monospacedDigit())
                            Text(Self.bytes(Double(p.privateKB))).font(.caption.monospacedDigit()).foregroundColor(TaskStyle.dim)
                        }
                    }
                }.listRowBackground(TaskStyle.panel)
            }.listStyle(.plain)
        }
    }

    private var performance: some View {
        ScrollView { VStack(spacing: 12) {
            card("CPU · last 2 minutes") {
                Sparkline(values: sampler.cpuHistory, color: TaskStyle.frost).frame(height: 90)
                Text(String(format: "Now %.0f%% · peak %.0f%% (100%% = one core)", sampler.cpuHistory.last ?? 0, sampler.cpuHistory.max() ?? 0)).font(.caption).foregroundColor(TaskStyle.dim)
            }
            card("Private guest memory · last 2 minutes") {
                Sparkline(values: sampler.memoryHistory, color: TaskStyle.green).frame(height: 90)
                Text(String(format: "Now %.0f MB", sampler.memoryHistory.last ?? 0)).font(.caption).foregroundColor(TaskStyle.dim)
            }
            card("Processes") {
                metric("\(sampler.processes.count)", "running guest processes")
                Text("GPU utilisation is not measurable through public iOS APIs and is not shown.").font(.caption).foregroundColor(TaskStyle.dim)
            }
        }.padding(.horizontal) }
    }
}

private struct DetailRow: View {   // LabeledContent needs iOS 16
    let label: String, value: String
    init(_ label: String, value: String) { self.label = label; self.value = value }
    var body: some View { HStack { Text(label); Spacer(); Text(value).foregroundColor(.secondary) } }
}

private struct ProcessDetailView: View {
    let process: GuestProcess
    @ObservedObject var sampler: TaskSampler
    @Environment(\.dismiss) private var dismiss
    @State private var confirm: Bool?   // false: terminate, true: force
    @State private var result: String?
    var body: some View {
        NavigationView {
            List {
                Section("Process") {
                    DetailRow("Name", value: process.name); DetailRow("PID", value: "\(process.id)")
                    DetailRow("Parent PID", value: "\(process.ppid)"); DetailRow("User ID", value: "\(process.uid)")
                    DetailRow("State", value: process.stateName); DetailRow("Threads", value: "\(process.threads)")
                }
                Section("Resources") {
                    DetailRow("CPU now", value: String(format: "%.1f%% of a core", process.cpuPercent))
                    DetailRow("CPU time", value: String(format: "%.1f s", Double(process.cpuMs) / 1000))
                    DetailRow("Private memory", value: "\(process.privateKB / 1024) MB")
                    DetailRow("Shared mappings", value: "\(process.sharedKB / 1024) MB")
                    DetailRow("Mapped address space", value: "\(process.mappedKB / 1024) MB")
                }
                Section {
                    Button("End Process (SIGTERM)") { confirm = false }.disabled(process.id <= 1)
                    Button("Force Kill (SIGKILL)", role: .destructive) { confirm = true }.disabled(process.id <= 1)
                    if let result { Text(result).font(.caption).foregroundColor(.secondary) }
                } footer: { Text(process.id <= 1 ? "PID 1 runs the Linux session and cannot be ended here." : "Ending a desktop component (window manager, panel, compositor) can close the desktop session.") }
            }
            .navigationTitle(process.name).navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .confirmationDialog(confirm == true ? "Force kill \(process.name)?" : "End \(process.name)?", isPresented: Binding(get: { confirm != nil }, set: { if !$0 { confirm = nil } }), titleVisibility: .visible) {
                Button(confirm == true ? "Force Kill" : "End Process", role: .destructive) {
                    let force = confirm == true
                    result = LRRuntimeBridge.terminateProcess(Int32(process.id), force: force) ? (force ? "SIGKILL sent." : "SIGTERM sent.") : "The process has already exited."
                    sampler.sample()
                }
            } message: { Text(confirm == true ? "The process stops immediately without saving." : "The process is asked to quit and may save its state.") }
        }
    }
}
