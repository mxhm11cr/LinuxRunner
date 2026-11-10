import SwiftUI

/// Compact desktop status for the Home header. "Ready" is reported only when
/// the session says the desktop passed its readiness checks and painted a
/// frame; a running X server alone is "Starting".
struct HomeStatus: View {
    enum Phase: Equatable { case off, stopped, starting, ready, degraded }
    let phase: Phase
    var open: (() -> Void)?
    var stop: (() -> Void)?
    private var color: Color {
        switch phase { case .ready: return .green; case .degraded: return .yellow; case .starting: return .orange; case .stopped: return .gray; case .off: return .red }
    }
    private var title: String {
        switch phase { case .ready: return "Ready"; case .degraded: return "Limited"; case .starting: return "Starting"; case .stopped: return "Stopped"; case .off: return "Linux off" }
    }
    var body: some View {
        Menu {
            if let open { Button { open() } label: { Label(phase == .stopped ? "Open Desktop" : "Show Desktop", systemImage: "desktopcomputer") } }
            if let stop { Button(role: .destructive) { stop() } label: { Label("Stop Desktop", systemImage: "power") } }
        } label: {
            HStack(spacing: 8) {
                ZStack {
                    RoundedRectangle(cornerRadius: 8, style: .continuous).fill(color.opacity(0.18)).frame(width: 28, height: 28)
                    if phase == .starting { ProgressView().scaleEffect(0.6).tint(color) }
                    else { Image(systemName: phase == .off ? "power" : "desktopcomputer").font(.system(size: 13, weight: .semibold)).foregroundColor(color) }
                }
                Text(title).font(.subheadline.weight(.semibold)).foregroundColor(.primary)
                Circle().fill(color).frame(width: 7, height: 7)
            }
            .padding(.leading, 6).padding(.trailing, 12).padding(.vertical, 6)
            .background(Capsule().fill(Color(.secondarySystemGroupedBackground)))
        }
        .disabled(open == nil && stop == nil)
        .accessibilityLabel("Desktop status: \(title)")
        .animation(.easeInOut(duration: 0.25), value: phase)
    }
}

/// Rounded-square app tile with a label underneath.
struct HomeTile: View {
    let title: String
    let symbol: String
    let colors: [Color]
    var large = false
    var badge = false
    @Environment(\.horizontalSizeClass) private var sizeClass
    var body: some View {
        if !large && sizeClass == .regular {
            // iPad: the four small tiles become wide cards that fill the row
            // instead of small icons separated by large gaps.
            HStack(spacing: 12) {
                icon(size: 44)
                Text(title).font(.subheadline.weight(.semibold)).foregroundColor(.primary).lineLimit(1).minimumScaleFactor(0.8)
                Spacer(minLength: 0)
            }
            .padding(12).frame(maxWidth: .infinity)
            .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
        } else if large {
            HStack(spacing: 12) {
                icon(size: 46)
                Text(title).font(.headline).foregroundColor(.primary).lineLimit(1).minimumScaleFactor(0.8)
                Spacer(minLength: 0)
            }
            .padding(14).frame(maxWidth: .infinity)
            .background(RoundedRectangle(cornerRadius: 20, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
        } else {
            VStack(spacing: 7) {
                icon(size: 60)
                Text(title).font(.caption.weight(.medium)).foregroundColor(.primary).lineLimit(1).minimumScaleFactor(0.8)
            }.frame(maxWidth: .infinity)
        }
    }
    private func icon(size: CGFloat) -> some View {
        RoundedRectangle(cornerRadius: size * 0.27, style: .continuous)
            .fill(LinearGradient(colors: colors, startPoint: .topLeading, endPoint: .bottomTrailing))
            .frame(width: size, height: size)
            .overlay(Image(systemName: symbol).font(.system(size: size * 0.42, weight: .semibold)).foregroundColor(.white))
            .overlay(alignment: .topTrailing) {
                if badge { Circle().fill(Color.green).frame(width: 12, height: 12).overlay(Circle().stroke(Color(.systemBackground), lineWidth: 2)).offset(x: 3, y: -3) }
            }
            .shadow(color: colors.last?.opacity(0.25) ?? .clear, radius: 6, y: 3)
    }
}

struct HomeTileStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.94 : 1)
            .opacity(configuration.isPressed ? 0.85 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.7), value: configuration.isPressed)
    }
}

/// Running and recent long tasks with their real state and elapsed time.
struct HomeTasks: View {
    @ObservedObject var center: LongTaskCenter
    var body: some View {
        if !center.entries.isEmpty {
            VStack(alignment: .leading, spacing: 10) {
                Text("Tasks").font(.headline)
                ForEach(center.entries.prefix(4)) { entry in
                    TimelineView(.periodic(from: .now, by: 1)) { context in
                        HStack(spacing: 12) {
                            ZStack {
                                RoundedRectangle(cornerRadius: 9, style: .continuous).fill(color(entry).opacity(0.18)).frame(width: 34, height: 34)
                                if entry.ended == nil && entry.state != .pausedByIOS { ProgressView().scaleEffect(0.7).tint(color(entry)) }
                                else { Image(systemName: symbol(entry)).foregroundColor(color(entry)).font(.system(size: 15, weight: .semibold)) }
                            }
                            VStack(alignment: .leading, spacing: 2) {
                                Text(entry.title).font(.subheadline.weight(.semibold)).lineLimit(1)
                                Text(entry.detail).font(.caption).foregroundColor(.secondary).lineLimit(2)
                                if let fraction = entry.fraction, entry.ended == nil { ProgressView(value: fraction).tint(color(entry)) }
                            }
                            Spacer(minLength: 6)
                            VStack(alignment: .trailing, spacing: 2) {
                                Text(entry.state.rawValue).font(.caption2.weight(.semibold)).foregroundColor(color(entry))
                                Text(elapsed(entry, now: context.date)).font(.caption2.monospacedDigit()).foregroundColor(.secondary)
                            }
                        }
                        .padding(12)
                        .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
                    }
                }
            }
        }
    }
    private func color(_ e: LongTaskCenter.Entry) -> Color {
        switch e.state { case .completed: return .green; case .failed: return .red; case .pausedByIOS: return .orange; case .backgroundGranted: return .indigo; case .running: return .blue }
    }
    private func symbol(_ e: LongTaskCenter.Entry) -> String {
        switch e.state { case .completed: return "checkmark"; case .failed: return "xmark"; case .pausedByIOS: return "pause.fill"; default: return "hourglass" }
    }
    private func elapsed(_ e: LongTaskCenter.Entry, now: Date) -> String {
        let seconds = Int((e.ended ?? now).timeIntervalSince(e.started))
        return seconds >= 3600 ? String(format: "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60) : String(format: "%d:%02d", seconds / 60, seconds % 60)
    }
}

/// Live preview of the running desktop on Home. It shows the session's own
/// frames (no second X server, no fake animation), refreshes at most 5 times a
/// second and only while visible, and only when a new frame arrived.
struct DesktopPreviewCard: View {
    let phase: HomeStatus.Phase
    let stateText: String
    var open: () -> Void
    @State private var image: UIImage?
    @State private var generation: UInt64 = 0
    @State private var visible = false
    private let tick = Timer.publish(every: 0.2, on: .main, in: .common).autoconnect()
    var body: some View {
        Button(action: open) {
            ZStack {
                RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Color(white: 0.06))
                if let image, phase != .off, phase != .stopped {
                    Image(uiImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                        .padding(6)
                } else if let last = image ?? DesktopPreviewCard.lastFrame {
                    // The last frame of your desktop, dimmed: what Open Desktop brings back.
                    Image(uiImage: last).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous)).padding(6)
                        .saturation(0.6).opacity(0.45)
                    caption
                } else {
                    MiniDesktopArt().clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous)).padding(6)
                    caption
                }
                if phase == .starting {
                    VStack { Spacer(); HStack(spacing: 8) { ProgressView().tint(.white); Text(stateText).font(.caption.weight(.medium)).foregroundColor(.white) }
                        .padding(.horizontal, 12).padding(.vertical, 7).background(Capsule().fill(.black.opacity(0.65))).padding(10) }
                }
            }
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
            .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).stroke(Color.white.opacity(0.08), lineWidth: 1))
        }
        .buttonStyle(HomeTileStyle())
        .accessibilityLabel(phase == .ready || phase == .degraded ? "Live desktop preview. Opens the desktop." : "Desktop. \(stateText)")
        .onAppear { visible = true; LRRuntimeBridge.setDesktopPreviewWanted(true); refresh() }
        .onDisappear { visible = false; LRRuntimeBridge.setDesktopPreviewWanted(false) }
        .onReceive(tick) { _ in if visible { refresh() } }
        .onChange(of: phase) { p in if p == .stopped || p == .off { if let image { DesktopPreviewCard.remember(image) }; image = nil; generation = 0 } else { LRRuntimeBridge.setDesktopPreviewWanted(visible) } }
    }
    private func refresh() {
        guard phase != .stopped, phase != .off else { return }
        var g = generation
        if let next = LRRuntimeBridge.desktopPreviewImageIfChanged(since: &g) { image = next; generation = g; DesktopPreviewCard.remember(next) }
    }
    // Last desktop frame, kept across launches (Caches; never synced or shared).
    private static let lastFrameURL = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("LinuxRunnerDesktopLastFrame.jpg")
    static var lastFrame: UIImage? = UIImage(contentsOfFile: lastFrameURL.path)
    private static var lastSaved = Date.distantPast
    static func remember(_ image: UIImage) {
        lastFrame = image
        guard Date().timeIntervalSince(lastSaved) > 10 else { return }
        lastSaved = Date()
        DispatchQueue.global(qos: .utility).async { try? image.jpegData(compressionQuality: 0.7)?.write(to: lastFrameURL, options: .atomic) }
    }
    private var caption: some View {
        VStack { Spacer(); HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(phase == .off ? "Linux is not running" : phase == .starting ? "Starting desktop…" : "Desktop stopped").font(.subheadline.weight(.semibold)).foregroundColor(.white)
                Text(phase == .off ? "Tap to start Linux" : "Tap to start your desktop").font(.caption).foregroundColor(.white.opacity(0.7))
            }
            Spacer()
        }.padding(.horizontal, 16).padding(.top, 14).padding(.bottom, 30) }
        .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

/// A drawn miniature desktop (wallpaper, two windows, a panel) for when no
/// frame of the user's desktop exists yet. Pure shapes: no images, no icons.
struct MiniDesktopArt: View {
    var body: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            ZStack(alignment: .topLeading) {
                LinearGradient(colors: [Color(red: 0.07, green: 0.12, blue: 0.30), Color(red: 0.20, green: 0.10, blue: 0.38), Color(red: 0.05, green: 0.05, blue: 0.12)], startPoint: .topLeading, endPoint: .bottomTrailing)
                Circle().fill(Color(red: 0.35, green: 0.45, blue: 1.0).opacity(0.35)).frame(width: w * 0.7).blur(radius: 40).offset(x: w * 0.35, y: -h * 0.2)
                window(width: w * 0.46, height: h * 0.48).offset(x: w * 0.08, y: h * 0.12)
                window(width: w * 0.40, height: h * 0.42).offset(x: w * 0.46, y: h * 0.30)
                VStack { Spacer()
                    HStack(spacing: 6) {
                        Capsule().fill(Color.white.opacity(0.55)).frame(width: 22, height: 6)
                        ForEach(0..<3) { _ in RoundedRectangle(cornerRadius: 2).fill(Color.white.opacity(0.25)).frame(width: 14, height: 6) }
                        Spacer()
                        Capsule().fill(Color.white.opacity(0.35)).frame(width: 26, height: 6)
                    }.padding(.horizontal, 10).frame(height: max(14, h * 0.08)).background(Color.black.opacity(0.45))
                }
            }
        }
    }
    private func window(width: CGFloat, height: CGFloat) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 4) { ForEach(0..<3) { _ in Circle().fill(Color.white.opacity(0.35)).frame(width: 5, height: 5) }; Spacer() }
                .padding(.horizontal, 7).frame(height: 12).background(Color.white.opacity(0.14))
            Rectangle().fill(Color.white.opacity(0.07))
        }
        .frame(width: width, height: height)
        .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 6, style: .continuous).stroke(Color.white.opacity(0.12), lineWidth: 0.5))
        .shadow(color: .black.opacity(0.35), radius: 8, y: 4)
    }
}

// MARK: - Distributions

/// Distributions LinuxRunner can actually install, in setup order.
struct CatalogDistribution: Identifiable {
    let id: String, name: String, summary: String, detail: String
    let experimental: Bool
    static let all: [CatalogDistribution] = [
        .init(id: "debian", name: "Debian 13", summary: "Recommended", detail: "Stable, widest app support, all desktops", experimental: false),
        .init(id: "arch", name: "Arch Linux ARM", summary: "Rolling release", detail: "pacman, X11 and Wayland desktops, KDE Plasma", experimental: false),
        .init(id: "fedora", name: "Fedora 44", summary: "DNF userspace", detail: "Lightweight desktop and KDE Plasma", experimental: false),
        .init(id: "kali", name: "Kali Linux", summary: "Security tools", detail: "Kali's Xfce desktop and tool sets", experimental: false),
        .init(id: "alpine", name: "Alpine 3.24", summary: "Small and fast", detail: "Mainly for the terminal", experimental: false),
        .init(id: "steamos", name: "SteamOS ARM64", summary: "Valve SteamFrame image", detail: "Downloads Valve's ARM64 recovery image; not official SteamOS support, no Steam games", experimental: true),
    ]
}

/// The distribution's own logo (DistroLogos.bundle, see its SOURCES.md) on a
/// light tile, so cut-out marks (Alpine, Fedora) keep their real colours.
struct DistroBadge: View {
    let distributionID: String?
    var size: CGFloat = 46
    private static var cache: [String: UIImage] = [:]
    private var file: String {
        switch distributionID ?? "alpine" {
        case "debian": return "debian"
        case "arch", "archarm": return "arch"
        case "fedora": return "fedora"
        case "kali": return "kali"
        case "steamos": return "steamos"
        default: return "alpine"
        }
    }
    private var logo: UIImage? {
        if let image = Self.cache[file] { return image }
        guard let bundle = Bundle.main.url(forResource: "DistroLogos", withExtension: "bundle"),
              let image = UIImage(contentsOfFile: bundle.appendingPathComponent(file + ".png").path) else { return nil }
        Self.cache[file] = image
        return image
    }
    var body: some View {
        RoundedRectangle(cornerRadius: size * 0.28, style: .continuous)
            .fill(Color(white: 0.96))
            .frame(width: size, height: size)
            .overlay {
                if let logo { Image(uiImage: logo).resizable().interpolation(.high).scaledToFit().padding(size * 0.14) }
                else { Image(systemName: "shippingbox.fill").foregroundColor(.gray) }
            }
            .accessibilityHidden(true)
    }
}

struct StatusChip: View {
    let text: String; let color: Color
    var body: some View {
        Text(text).font(.caption2.weight(.semibold)).foregroundColor(color)
            .padding(.horizontal, 8).padding(.vertical, 3)
            .background(Capsule().fill(color.opacity(0.15)))
    }
}

struct InstalledDistributionCard: View {
    let name: String, distributionID: String?, version: String, size: String, desktop: String?
    let active: Bool, nextLaunch: Bool
    var body: some View {
        HStack(spacing: 14) {
            DistroBadge(distributionID: distributionID)
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(name).font(.headline).foregroundColor(.primary).lineLimit(1)
                    if active { StatusChip(text: "Active", color: .green) }
                    else if nextLaunch { StatusChip(text: "Next launch", color: .accentColor) }
                }
                Text([version, size].filter { !$0.isEmpty }.joined(separator: " · ")).font(.caption).foregroundColor(.secondary).lineLimit(1)
                if let desktop { Label(desktop, systemImage: "macwindow").font(.caption).foregroundColor(.secondary).lineLimit(1) }
            }
            Spacer(minLength: 0)
            Image(systemName: "chevron.right").font(.footnote.weight(.semibold)).foregroundColor(.secondary.opacity(0.6))
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).stroke(active ? Color.green.opacity(0.55) : Color.clear, lineWidth: 1.5))
        .accessibilityElement(children: .combine)
    }
}

struct CatalogDistributionCard: View {
    let item: CatalogDistribution
    let installed: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                DistroBadge(distributionID: item.id, size: 38)
                Spacer()
                if item.experimental { StatusChip(text: "Experimental", color: .orange) }
            }
            Text(item.name).font(.subheadline.weight(.semibold)).foregroundColor(.primary)
            Text(item.summary).font(.caption).foregroundColor(.secondary)
            Text(item.detail).font(.caption2).foregroundColor(.secondary).lineLimit(3).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            Label(installed ? "Add Another" : "Install", systemImage: installed ? "plus" : "arrow.down.circle")
                .font(.caption.weight(.semibold)).foregroundColor(.accentColor)
        }
        .padding(14).frame(maxWidth: .infinity, minHeight: 168, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
        .accessibilityElement(children: .combine)
    }
}

/// Settings row label: an iOS-style coloured icon tile and a title.
struct SettingsLabel: View {
    let title: String, symbol: String, color: Color
    init(_ title: String, _ symbol: String, _ color: Color) { self.title = title; self.symbol = symbol; self.color = color }
    var body: some View {
        Label {
            Text(title)
        } icon: {
            RoundedRectangle(cornerRadius: 7, style: .continuous).fill(color)
                .frame(width: 29, height: 29)
                .overlay(Image(systemName: symbol).font(.system(size: 14, weight: .semibold)).foregroundColor(.white))
        }
    }
}
