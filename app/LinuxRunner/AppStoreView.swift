import SwiftUI
import UIKit

// Linux App Store: a native front end for the active distribution's own
// package manager (APT on Debian/Kali, DNF on Fedora, pacman on Arch, APK on
// Alpine). Every list comes from package-manager.sh running inside the
// environment; nothing here is a static stand-in for repository data.
// Changes are reviewed as the package manager's simulated transaction first,
// then applied exactly as reviewed.

struct StoreItem: Identifiable, Equatable {
    var id: String { package }
    let package, version, architecture, status, summary, name, category: String
    let graphical: Bool
    var installed: Bool { status == "installed" || status.hasPrefix("ii") }
    static func parseCatalog(_ text: String) -> [StoreItem] {
        text.components(separatedBy: .newlines).compactMap { line in
            let f = line.components(separatedBy: "\t")
            guard f.count >= 8, PackageOperation.validName(f[0]) else { return nil }
            return StoreItem(package: f[0], version: f[1], architecture: f[2], status: f[3], summary: f[4], name: f[5], category: f[6], graphical: f[7] == "1")
        }
    }
    init(package: String, version: String, architecture: String, status: String, summary: String, name: String, category: String, graphical: Bool) {
        self.package = package; self.version = version; self.architecture = architecture; self.status = status
        self.summary = summary; self.name = name; self.category = category; self.graphical = graphical
    }
    init(record: LinuxPackageRecord) {
        self.init(package: record.name, version: record.version, architecture: record.architecture, status: record.status,
                  summary: record.summary, name: record.name, category: "", graphical: false)
    }
}

struct PlanChange: Identifiable, Hashable {
    let id = UUID()
    let action, package, version: String
}

enum StoreOperationState: Equatable { case idle, running(String), failed(String), succeeded(String) }

@MainActor final class AppStoreModel: ObservableObject {
    @Published var catalog: [StoreItem] = []
    @Published var installed: [LinuxPackageRecord] = []
    @Published var searchResults: [StoreItem] = []
    @Published var plan: [PlanChange] = []
    @Published var planText = ""
    @Published var details = ""
    @Published var updatesText = ""
    @Published var log = ""
    @Published var stage = ""
    @Published var state: StoreOperationState = .idle
    @Published var loadedRoot = ""
    private var runner: TerminalViewController?
    private var timer: Timer?
    private var queue: [(String, String, (Bool) -> Void)] = []

    var root: String { LRRuntimeBridge.activeRootName() }
    var busy: Bool { if case .running = state { return true }; return runner != nil }
    var packageManager: String {
        switch LRRuntimeBridge.activeDistributionID() ?? "" {
        case "fedora": return "DNF"
        case "arch", "archarm": return "pacman"
        case "alpine": return "APK"
        case "debian", "kali": return "APT"
        default: return LRRuntimeBridge.activeEnvironmentIsFedora() ? "DNF" : LRRuntimeBridge.activeEnvironmentIsArch() ? "pacman" : LRRuntimeBridge.activeEnvironmentIsDebian() ? "APT" : "APK"
        }
    }
    private var support: URL { LRRuntimeBridge.exchangeURL(root).appendingPathComponent(".linuxrunner/gui") }
    private func read(_ name: String, _ bytes: UInt) -> String {
        LRRuntimeBridge.boundedDiagnosticTail(support.appendingPathComponent(name), maxBytes: bytes)
    }

    /// Distribution switch (or first appearance): every list belongs to one root.
    func load(force: Bool = false) {
        guard LRRuntimeBridge.isRunning() else { return }
        if loadedRoot != root || force {
            if loadedRoot != root { catalog = []; installed = []; searchResults = []; plan = []; details = ""; updatesText = "" }
            loadedRoot = root
            enqueue("catalog", "") { [weak self] _ in self?.catalog = StoreItem.parseCatalog(self?.read("package-catalog.tsv", 262144) ?? "") }
            enqueue("installed", "") { [weak self] _ in self?.installed = LinuxPackageRecord.parse(self?.read("package-state.tsv", 262144) ?? "") }
        }
    }
    func refreshRepositories() {
        enqueue("update", "") { [weak self] ok in if ok { self?.load(force: true) } }
    }
    func search(_ text: String) {
        guard PackageOperation.validName(text) else { searchResults = []; return }
        enqueue("search", text) { [weak self] _ in
            let records = LinuxPackageRecord.parse(self?.read("package-state.tsv", 262144) ?? "")
            self?.searchResults = records.prefix(200).map(StoreItem.init(record:))
        }
    }
    func loadDetails(_ package: String) {
        details = ""
        enqueue("details", package) { [weak self] _ in self?.details = self?.read("package-details.txt", 16384) ?? "" }
    }
    func loadUpdates() {
        enqueue("updates", "") { [weak self] _ in
            let text = self?.read("package-operation.log", 32768) ?? ""
            self?.updatesText = text.components(separatedBy: .newlines).filter { !$0.hasPrefix("PACKAGE_") && !$0.isEmpty }.joined(separator: "\n")
        }
    }
    func prepare(_ kind: String, _ package: String, completion: @escaping (Bool) -> Void) {
        plan = []; planText = ""
        enqueue("plan-" + kind, package) { [weak self] ok in
            guard let self else { return }
            self.planText = self.read("package-plan.txt", 32768)
            self.plan = self.read("package-plan.tsv", 65536).components(separatedBy: .newlines).compactMap { line in
                let f = line.components(separatedBy: "\t")
                return f.count >= 3 ? PlanChange(action: f[0], package: f[1], version: f[2]) : nil
            }
            completion(ok)
        }
    }
    func apply(_ kind: String, _ package: String, manager: EnvironmentManager) {
        LRRuntimeBridge.beginAwake("app-store")
        let verb = kind == "remove" ? "Removing" : kind == "upgrade" ? "Updating" : "Installing"
        let token = LongTaskCenter.shared.begin(title: "\(verb) \(package)", detail: "\(packageManager) · \(LRRuntimeBridge.activeRootName())", userStarted: true)
        enqueue("apply-" + kind, package) { [weak self] ok in
            LRRuntimeBridge.endAwake("app-store")
            LongTaskCenter.shared.end(token, success: ok, message: ok ? "\(package) \(kind == "remove" ? "removed" : kind == "upgrade" ? "updated" : "installed")." : "The package manager reported an error. Open the App Store for details.")
            self?.load(force: true)
            if ok { manager.refresh() }
        }
    }

    private func enqueue(_ operation: String, _ value: String, _ done: @escaping (Bool) -> Void) {
        queue.append((operation, value, done))
        if runner == nil { next() }
    }
    private func next() {
        guard runner == nil, !queue.isEmpty else { return }
        let (operation, value, done) = queue.removeFirst()
        do {
            _ = try LRRuntimeBridge.prepareApplicationSupport()
            let command = try PackageOperation.command(operation, value: value)
            let resultURL = support.appendingPathComponent("package-result")
            try? FileManager.default.removeItem(at: resultURL)
            guard let session = LRRuntimeBridge.terminal(forCommand: command, directory: "/root") as TerminalViewController? else { throw EnvironmentError.invalidCommand }
            runner = session
            let label = Self.label(operation, value)
            state = .running(label); stage = label; log = ""
            LRRuntimeBridge.logCategory("PACKAGE", event: "app-store operation=\(operation) root=\(root)")
            let started = Date()
            timer = Timer.scheduledTimer(withTimeInterval: 0.6, repeats: true) { [weak self] t in
                Task { @MainActor in
                    guard let self else { t.invalidate(); return }
                    let code = self.read("package-result", 64).trimmingCharacters(in: .whitespacesAndNewlines)
                    let tail = self.read("package-output.tmp", 6144)
                    if !tail.isEmpty { self.log = tail }
                    if let line = tail.components(separatedBy: .newlines).last(where: { $0.hasPrefix("PACKAGE_STAGE: ") }) { self.stage = String(line.dropFirst(15)) }
                    // No result within 30 minutes and no live output: report, never guess.
                    let stalled = Date().timeIntervalSince(started) > 1800 && code.isEmpty
                    guard (!code.isEmpty && code != "running") || stalled else { return }
                    t.invalidate(); self.timer = nil; self.runner = nil
                    self.log = self.read("package-operation.log", 16384)
                    let ok = code == "0"
                    self.state = ok ? .succeeded(label) : .failed(stalled ? "\(label): no result after 30 minutes. Check the package log." : Self.failure(code, self.log))
                    done(ok)
                    self.next()
                }
            }
        } catch {
            state = .failed(error.localizedDescription); done(false); next()
        }
    }
    /// Never offer to remove the base system: core packages in the plan, or the
    /// package manager itself flagging essential/protected packages.
    static func protectedRemoval(plan: [PlanChange], text: String) -> String? {
        let core: Set<String> = ["bash", "dash", "coreutils", "libc6", "glibc", "musl", "busybox", "apt", "dpkg", "rpm", "dnf", "dnf5", "pacman", "apk-tools", "systemd", "filesystem", "base", "base-files", "login", "passwd", "util-linux", "sudo", "openssl", "ca-certificates", "tar", "gzip", "sed", "grep", "findutils"]
        let hits = plan.filter { $0.action == "remove" && core.contains($0.package) }.map(\.package)
        if !hits.isEmpty { return "This would remove core system packages (\(hits.prefix(4).joined(separator: ", "))). LinuxRunner will not do that from the App Store; use the terminal if you really mean it." }
        let lower = text.lowercased()
        if lower.contains("essential packages will be removed") || lower.contains("protected packages") || lower.contains("is protected") {
            return "The package manager marks this change as removing essential or protected system packages. It was not applied."
        }
        return nil
    }
    static func label(_ operation: String, _ value: String) -> String {
        switch operation {
        case "catalog": return "Loading apps"
        case "installed": return "Reading installed packages"
        case "update": return "Refreshing repositories"
        case "updates": return "Checking for updates"
        case "search": return "Searching for \(value)"
        case "details": return "Reading \(value)"
        case "plan-install": return "Preparing to install \(value)"
        case "plan-remove": return "Checking what removing \(value) affects"
        case "plan-upgrade": return "Preparing to update \(value)"
        case "apply-install": return "Installing \(value)"
        case "apply-remove": return "Removing \(value)"
        case "apply-upgrade": return "Updating \(value)"
        default: return operation
        }
    }
    static func failure(_ code: String, _ log: String) -> String {
        let lines = log.components(separatedBy: .newlines).filter { $0.hasPrefix("E:") || $0.hasPrefix("error:") || $0.hasPrefix("Error") || $0.contains("No match") || $0.contains("Unable to locate") || $0.contains("PACKAGE_LOCK") || $0.contains("conflict") }
        let reason = lines.suffix(3).joined(separator: "\n")
        return "The package manager reported an error (exit \(code))." + (reason.isEmpty ? "" : "\n" + reason)
    }
}

// MARK: - Views

struct AppStoreView: View {
    @ObservedObject var manager: EnvironmentManager
    var run: (LinuxApplication) -> Void
    @StateObject private var model = AppStoreModel()
    @State private var section = 0
    @State private var query = ""
    @State private var selected: StoreItem?
    @State private var showAdvanced = false
    @State private var searchToken = UUID()
    private var environment: LinuxEnvironment? { manager.environments.first(where: manager.active) }

    var body: some View {
        Group {
            if !LRRuntimeBridge.isRunning() || environment == nil {
                StoreEmpty(icon: "power", title: "Linux is not running", detail: "Start the Linux runtime on the Home screen to browse and install apps.")
            } else {
                List {
                    Section { header }
                    if !query.isEmpty && section != 1 { searchSection }
                    else if section == 0 { discover }
                    else if section == 1 { installedSection }
                    else { updatesSection }
                }.listStyle(.insetGrouped)
            }
        }
        .navigationTitle("App Store")
        .searchable(text: $query, prompt: "Search \(model.packageManager) packages")
        .onSubmit(of: .search) { if section != 1 { model.search(query.trimmingCharacters(in: .whitespaces)) } }
        .onChange(of: query) { text in
            // Debounced: search the repositories 0.6 s after typing stops.
            let trimmed = text.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty { model.searchResults = []; return }
            guard section != 1, trimmed.count >= 2 else { return }
            searchToken = UUID(); let token = searchToken
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { if token == searchToken && query.trimmingCharacters(in: .whitespaces) == trimmed { model.search(trimmed) } }
        }
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Menu {
                    Button { model.refreshRepositories() } label: { Label("Refresh Repositories", systemImage: "arrow.clockwise") }
                    if let environment { Button { showAdvanced = true } label: { Label("Repair Center & Advanced", systemImage: "wrench.and.screwdriver") }.disabled(model.busy)
                        .background(NavigationLink(destination: PackageView(environment: environment, manager: manager, run: run), isActive: $showAdvanced) { EmptyView() }.hidden()) }
                } label: { Image(systemName: "ellipsis.circle") }.disabled(model.busy && !showAdvanced)
            }
        }
        .safeAreaInset(edge: .bottom) { StoreStatusBar(model: model) }
        .sheet(item: $selected) { item in
            StoreDetailView(item: item, model: model, manager: manager, launch: launchable(item), run: run)
        }
        .onAppear { model.load() }
    }
    private var header: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                RoundedRectangle(cornerRadius: 12, style: .continuous).fill(LinearGradient(colors: [.blue, .indigo], startPoint: .topLeading, endPoint: .bottomTrailing))
                    .frame(width: 44, height: 44).overlay(Image(systemName: "bag.fill").foregroundColor(.white).font(.title3))
                VStack(alignment: .leading, spacing: 2) {
                    Text(environment?.name ?? "Linux").font(.headline)
                    Text("\(model.packageManager) · \(environment?.distribution ?? "") repositories").font(.caption).foregroundColor(.secondary)
                }
            }
            Picker("Section", selection: $section) { Text("Discover").tag(0); Text("Installed").tag(1); Text("Updates").tag(2) }
                .pickerStyle(.segmented)
                .onChange(of: section) { if $0 == 2 && model.updatesText.isEmpty { model.loadUpdates() } }
        }.padding(.vertical, 4)
    }
    /// Store sections; the curated catalog's categories map onto these.
    static func section(_ category: String) -> String {
        switch category { case "Media": return "Multimedia"; case "Accessories", "System": return "Utilities"; default: return category }
    }
    private var categories: [String] {
        let order = ["Internet", "Graphics", "Multimedia", "Development", "Utilities", "Office", "Network", "Embedded", "Security"]
        let present = Set(model.catalog.map { Self.section($0.category) })
        return order.filter(present.contains) + present.subtracting(order).sorted()
    }
    private var featured: [StoreItem] {
        let picks = ["firefox-esr", "firefox", "blender", "gimp", "libreoffice", "libreoffice-fresh", "inkscape", "vlc", "geany"]
        return picks.compactMap { p in model.catalog.first { $0.package == p } }.prefix(6).map { $0 }
    }
    @ViewBuilder private var discover: some View {
        if model.catalog.isEmpty {
            Section {
                if model.busy { HStack { ProgressView(); Text(model.stage).foregroundColor(.secondary) } }
                else {
                    Text("No catalog apps found in this distribution's package lists. If the repositories were never refreshed, refresh them first.").font(.callout).foregroundColor(.secondary)
                    Button { model.refreshRepositories() } label: { Label("Refresh Repositories", systemImage: "arrow.clockwise") }
                }
            }
        }
        if !featured.isEmpty {
            Section("Featured") {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 14) {
                        ForEach(featured) { item in
                            Button { selected = item } label: {
                                VStack(alignment: .leading, spacing: 8) {
                                    StoreIcon(image: icon(item).image, symbol: icon(item).symbol, tint: icon(item).tint, size: 56)
                                    Text(item.name).font(.subheadline.weight(.semibold)).foregroundColor(.primary).lineLimit(1)
                                    Text(item.installed ? "Installed" : item.version).font(.caption2).foregroundColor(.secondary).lineLimit(1)
                                }.frame(width: 96, alignment: .leading)
                            }.buttonStyle(.plain)
                        }
                    }.padding(.vertical, 6)
                }
            }
        }
        ForEach(categories, id: \.self) { category in
            Section(category) {
                ForEach(model.catalog.filter { Self.section($0.category) == category }) { item in
                    Button { selected = item } label: { StoreRow(item: item, icon: icon(item)) }.buttonStyle(.plain)
                }
            }
        }
        if !model.catalog.isEmpty {
            Section { Text("Listed apps are the ones \(environment?.distribution ?? "this distribution")'s repositories offer right now. Search finds every other package. Installation uses \(model.packageManager); running an app on LinuxRunner is not guaranteed for every package.").font(.footnote).foregroundColor(.secondary) }
        }
    }
    @ViewBuilder private var searchSection: some View {
        Section("Results for “\(query)”") {
            if model.searchResults.isEmpty {
                Text(model.busy ? model.stage : "Press Search to look up “\(query)” in \(model.packageManager).").foregroundColor(.secondary)
            }
            ForEach(model.searchResults) { item in
                Button { selected = catalogEntry(item) } label: { StoreRow(item: catalogEntry(item), icon: icon(catalogEntry(item))) }.buttonStyle(.plain)
            }
        }
    }
    @ViewBuilder private var installedSection: some View {
        Section("Apps") {
            if manager.graphical.isEmpty { Text(manager.discovering ? "Looking for apps…" : "No graphical apps with desktop entries yet.").foregroundColor(.secondary) }
            ForEach(manager.graphical.filter { query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) }) { app in
                HStack(spacing: 12) {
                    StoreIcon(image: manager.applicationIcons[app.launchKey], symbol: "app.fill", tint: .blue)
                    VStack(alignment: .leading) { Text(app.name).font(.body.weight(.medium)); if let comment = app.comment { Text(comment).font(.caption).foregroundColor(.secondary).lineLimit(1) } }
                    Spacer()
                    Button("Open") { run(app) }.buttonStyle(.bordered).buttonBorderShape(.capsule)
                }
            }
        }
        let filtered = query.isEmpty ? model.installed : model.installed.filter { $0.name.localizedCaseInsensitiveContains(query) }
        Section("All Packages (\(filtered.count))") {
            ForEach(filtered.prefix(2000)) { record in
                Button { selected = catalogEntry(StoreItem(record: record)) } label: {
                    VStack(alignment: .leading, spacing: 2) { Text(record.name).font(.callout); Text(record.version).font(.caption2).foregroundColor(.secondary) }
                }.buttonStyle(.plain)
            }
        }
    }
    @ViewBuilder private var updatesSection: some View {
        Section("Available Updates") {
            if model.updatesText.isEmpty { Text(model.busy ? model.stage : "No update information yet.").foregroundColor(.secondary) }
            else { Text(model.updatesText).font(.system(.caption, design: .monospaced)).textSelection(.enabled) }
            Button { model.refreshRepositories(); model.loadUpdates() } label: { Label("Refresh and Check Again", systemImage: "arrow.clockwise") }.disabled(model.busy)
        }
        Section { Text("Open a package from Installed or search to update it. Arch updates the whole system together, as rolling releases require.").font(.footnote).foregroundColor(.secondary) }
    }
    private func catalogEntry(_ item: StoreItem) -> StoreItem {
        guard let known = model.catalog.first(where: { $0.package == item.package }) else {
            let isInstalled = item.installed || model.installed.contains { $0.name == item.package }
            return StoreItem(package: item.package, version: item.version, architecture: item.architecture, status: isInstalled ? "installed" : item.status, summary: item.summary, name: item.name, category: item.category, graphical: item.graphical)
        }
        return known
    }
    private func launchable(_ item: StoreItem) -> LinuxApplication? {
        guard item.installed else { return nil }
        return manager.graphical.first { app in
            let exe = (app.command as NSString).lastPathComponent
            return exe == item.package || exe == item.package.replacingOccurrences(of: "-esr", with: "") || app.name.localizedCaseInsensitiveCompare(item.name) == .orderedSame
        }
    }
    private func icon(_ item: StoreItem) -> StoreIconSpec {
        if let app = launchable(item), let image = manager.applicationIcons[app.launchKey] { return StoreIconSpec(image: image, symbol: "", tint: .blue) }
        let map: [String: (String, Color)] = ["Internet": ("globe", .blue), "Graphics": ("paintpalette.fill", .pink), "Office": ("doc.richtext.fill", .orange),
            "Media": ("play.rectangle.fill", .red), "Development": ("chevron.left.forwardslash.chevron.right", .indigo), "Accessories": ("wrench.adjustable.fill", .teal),
            "System": ("gauge.with.dots.needle.67percent", .gray), "Network": ("network", .green), "Embedded": ("cpu.fill", .mint), "Security": ("lock.shield.fill", .purple)]
        let entry = map[item.category] ?? (item.graphical ? "macwindow" : "shippingbox.fill", .gray)
        return StoreIconSpec(image: nil, symbol: entry.0, tint: entry.1)
    }
}

struct StoreIconSpec { let image: UIImage?; let symbol: String; let tint: Color }

struct StoreIcon: View {
    let image: UIImage?
    let symbol: String
    let tint: Color
    var size: CGFloat = 44
    var body: some View {
        RoundedRectangle(cornerRadius: size * 0.27, style: .continuous)
            .fill(image == nil ? AnyShapeStyle(LinearGradient(colors: [tint.opacity(0.9), tint], startPoint: .top, endPoint: .bottom)) : AnyShapeStyle(Color(.secondarySystemBackground)))
            .frame(width: size, height: size)
            .overlay {
                if let image { Image(uiImage: image).resizable().scaledToFit().padding(size * 0.14) }
                else { Image(systemName: symbol).font(.system(size: size * 0.45, weight: .semibold)).foregroundColor(.white) }
            }
    }
}

private struct StoreRow: View {
    let item: StoreItem
    let icon: StoreIconSpec
    var body: some View {
        HStack(spacing: 12) {
            StoreIcon(image: icon.image, symbol: icon.symbol, tint: icon.tint)
            VStack(alignment: .leading, spacing: 2) {
                Text(item.name).font(.body.weight(.medium)).foregroundColor(.primary)
                Text(item.summary.isEmpty ? item.package : item.summary).font(.caption).foregroundColor(.secondary).lineLimit(2)
            }
            Spacer(minLength: 8)
            Text(item.installed ? "Installed" : "Get").font(.caption.weight(.bold))
                .padding(.horizontal, 12).padding(.vertical, 5)
                .background(Capsule().fill(item.installed ? Color(.tertiarySystemFill) : Color.accentColor.opacity(0.15)))
                .foregroundColor(item.installed ? .secondary : .accentColor)
        }.padding(.vertical, 2).contentShape(Rectangle())
    }
}

private struct StoreEmpty: View {
    let icon, title, detail: String
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: icon).font(.system(size: 40)).foregroundColor(.secondary)
            Text(title).font(.title3.weight(.semibold))
            Text(detail).font(.callout).foregroundColor(.secondary).multilineTextAlignment(.center)
        }.padding(32).frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct StoreStatusBar: View {
    @ObservedObject var model: AppStoreModel
    var body: some View {
        Group {
            switch model.state {
            case .running:
                HStack(spacing: 10) { ProgressView(); Text(model.stage).font(.footnote).lineLimit(1); Spacer() }
            case .failed(let message):
                HStack(alignment: .top, spacing: 10) { Image(systemName: "exclamationmark.triangle.fill").foregroundColor(.orange); Text(message).font(.footnote).lineLimit(3); Spacer()
                    Button { model.state = .idle } label: { Image(systemName: "xmark") }.buttonStyle(.plain) }
            default: EmptyView()
            }
        }
        .padding(visible ? 12 : 0)
        .background(visible ? AnyShapeStyle(.ultraThinMaterial) : AnyShapeStyle(Color.clear))
    }
    private var visible: Bool {
        switch model.state { case .running, .failed: return true; default: return false }
    }
}

struct StoreDetailView: View {
    let item: StoreItem
    @ObservedObject var model: AppStoreModel
    @ObservedObject var manager: EnvironmentManager
    let launch: LinuxApplication?
    var run: (LinuxApplication) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var reviewing: String?
    @State private var planFailed = false
    @State private var applying: String?
    @State private var showLog = false
    private var installedNow: Bool {
        if let applying, case .succeeded = model.state { return applying != "remove" }
        return item.installed || model.installed.contains { $0.name == item.package }
    }
    var body: some View {
        NavigationView {
            List {
                Section {
                    HStack(spacing: 16) {
                        StoreIcon(image: launch.flatMap { manager.applicationIcons[$0.launchKey] }, symbol: item.graphical ? "macwindow" : "terminal.fill", tint: .indigo, size: 72)
                        VStack(alignment: .leading, spacing: 4) {
                            Text(item.name).font(.title2.weight(.bold))
                            Text(item.package).font(.subheadline).foregroundColor(.secondary)
                            Text(item.graphical ? "Graphical app" : "Command-line package").font(.caption).foregroundColor(.secondary)
                        }
                    }.padding(.vertical, 6)
                    actions
                }
                if let reviewing { planSection(reviewing) }
                if applying != nil { progressSection }
                Section("Information") {
                    row("Version", item.version.isEmpty ? "—" : item.version)
                    row("Architecture", item.architecture.isEmpty ? "—" : item.architecture)
                    row("Status", installedNow ? "Installed" : "Not installed")
                    row("Source", "\(model.packageManager) · configured repositories")
                    row("Distribution", (try? EnvironmentCatalog.read(LRRuntimeBridge.catalogURL()))?.environments.first(where: { $0.rootfsName == LRRuntimeBridge.activeRootName() })?.name ?? (LRRuntimeBridge.activeDistributionID() ?? "Linux"))
                    if !item.summary.isEmpty { Text(item.summary).font(.callout) }
                }
                Section("Package Details") {
                    if model.details.isEmpty { HStack { ProgressView(); Text("Reading package information").foregroundColor(.secondary) } }
                    else { Text(model.details).font(.system(.caption2, design: .monospaced)).textSelection(.enabled) }
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle(item.name).navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .onAppear { model.loadDetails(item.package) }
        }
    }
    @ViewBuilder private var actions: some View {
        HStack(spacing: 10) {
            if installedNow {
                if let launch { Button { dismiss(); run(launch) } label: { Text("Open").frame(maxWidth: .infinity) }.buttonStyle(.borderedProminent) }
                Button { review("upgrade") } label: { Text("Update").frame(maxWidth: .infinity) }.buttonStyle(.bordered)
                Button(role: .destructive) { review("remove") } label: { Text("Remove").frame(maxWidth: .infinity) }.buttonStyle(.bordered)
            } else {
                Button { review("install") } label: { Text("Install").frame(maxWidth: .infinity) }.buttonStyle(.borderedProminent)
            }
        }.disabled(model.busy).controlSize(.large)
    }
    private func review(_ kind: String) {
        reviewing = kind; applying = nil; planFailed = false
        model.prepare(kind, item.package) { ok in planFailed = !ok }
    }
    @ViewBuilder private func planSection(_ kind: String) -> some View {
        Section {
            if case .running = model.state, applying == nil { HStack { ProgressView(); Text(model.stage).foregroundColor(.secondary) } }
            else if planFailed { Text("The package manager could not plan this change.").foregroundColor(.orange); Text(model.planText.isEmpty ? model.log : model.planText).font(.system(.caption2, design: .monospaced)).textSelection(.enabled) }
            else if applying == nil {
                let removals = model.plan.filter { $0.action == "remove" }
                let others = model.plan.filter { $0.action != "remove" }
                if model.plan.isEmpty { Text("Nothing to change: \(item.package) is already in the requested state.").foregroundColor(.secondary) }
                if !others.isEmpty { Text("\(others.count) package\(others.count == 1 ? "" : "s") will be installed or updated").font(.subheadline.weight(.semibold)) }
                ForEach(others) { change in HStack { Image(systemName: change.action == "upgrade" ? "arrow.up.circle" : "plus.circle").foregroundColor(.green); Text(change.package); Spacer(); Text(change.version).font(.caption).foregroundColor(.secondary) } }
                if !removals.isEmpty {
                    Text("\(removals.count) package\(removals.count == 1 ? "" : "s") will be removed").font(.subheadline.weight(.semibold)).foregroundColor(.red)
                    ForEach(removals) { change in HStack { Image(systemName: "minus.circle").foregroundColor(.red); Text(change.package); Spacer(); Text(change.version).font(.caption).foregroundColor(.secondary) } }
                    if kind == "remove" && removals.count > 1 { Text("Other installed packages depend on \(item.package) and are removed with it.").font(.footnote).foregroundColor(.orange) }
                }
                if let reason = AppStoreModel.protectedRemoval(plan: model.plan, text: model.planText) {
                    Label(reason, systemImage: "lock.shield").font(.footnote).foregroundColor(.orange)
                } else if !model.plan.isEmpty {
                    Button(role: kind == "remove" ? .destructive : nil) { applying = kind; model.apply(kind, item.package, manager: manager) } label: {
                        Text(kind == "remove" ? "Remove \(removals.count) Package\(removals.count == 1 ? "" : "s")" : kind == "upgrade" ? "Update" : "Install \(others.count) Package\(others.count == 1 ? "" : "s")").font(.body.weight(.semibold)).frame(maxWidth: .infinity)
                    }
                }
                Button("Cancel") { reviewing = nil }.frame(maxWidth: .infinity)
            }
        } header: { Text("Review Changes") } footer: {
            if applying == nil && !planFailed { Text("This is \(model.packageManager)'s own simulated transaction. Nothing has changed yet.") }
        }
    }
    @ViewBuilder private var progressSection: some View {
        Section("Progress") {
            switch model.state {
            case .running: HStack { ProgressView(); Text(model.stage) }
            case .succeeded: Label(applying == "remove" ? "Removed" : applying == "upgrade" ? "Updated" : "Installed", systemImage: "checkmark.circle.fill").foregroundColor(.green)
            case .failed(let message): Label(message, systemImage: "xmark.octagon.fill").foregroundColor(.red)
            case .idle: EmptyView()
            }
            DisclosureGroup("Package manager output", isExpanded: $showLog) {
                Text(model.log).font(.system(.caption2, design: .monospaced)).textSelection(.enabled)
            }
        }
    }
    private func row(_ title: String, _ value: String) -> some View {
        HStack { Text(title).foregroundColor(.secondary); Spacer(); Text(value).multilineTextAlignment(.trailing) }
    }
}
