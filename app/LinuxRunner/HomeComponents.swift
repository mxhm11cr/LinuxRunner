import SwiftUI
import os

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
                // Short titles stay on one line ("App Store"); longer ones use
                // two ("CPU & Memory", "Task Manager") instead of truncating.
                Text(title).font(.headline).foregroundColor(.primary).lineLimit(title.count > 10 ? 2 : 1).minimumScaleFactor(0.85)
                    .multilineTextAlignment(.leading).fixedSize(horizontal: false, vertical: true)
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
        .init(id: "ubuntu", name: "Ubuntu 24.04", summary: "Ubuntu Base · Experimental", detail: "APT, i3 and experimental Ubuntu GNOME", experimental: true),
        .init(id: "pop", name: "Pop!_OS 24.04", summary: "By System76 · Experimental", detail: "Ubuntu base with Pop!_OS's repository and look", experimental: true),
        .init(id: "zorin", name: "Zorin OS 18", summary: "Windows-like · Experimental", detail: "Ubuntu base with Zorin OS's repositories and look", experimental: true),
        .init(id: "void", name: "Void Linux", summary: "Independent · Experimental", detail: "Independent rolling release with xbps; very light", experimental: true),
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
        case "ubuntu": return "ubuntu"
        case "kali": return "kali"
        case "steamos": return "steamos"
        case "pop": return "pop"
        case "zorin": return "zorin"
        case "void": return "void"
        case "opensuse", "opensuse-tumbleweed": return "opensuse"
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

/// A desktop environment's mark, in the same rounded tile as DistroBadge:
/// the icon theme's logo where one exists (DesktopLogos.bundle/SOURCES.md),
/// otherwise a symbol tile drawn by the app.
struct DesktopBadge: View {
    let desktopID: String
    var size: CGFloat = 46
    private static var cache: [String: UIImage] = [:]
    private var logo: UIImage? {
        guard ["linuxrunner", "gnome", "plasma", "lxqt", "xfce"].contains(desktopID) else { return nil }
        if let image = Self.cache[desktopID] { return image }
        guard let bundle = Bundle.main.url(forResource: "DesktopLogos", withExtension: "bundle"),
              let image = UIImage(contentsOfFile: bundle.appendingPathComponent(desktopID + ".png").path) else { return nil }
        Self.cache[desktopID] = image
        return image
    }
    private var tile: (symbol: String, colors: [Color]) {
        switch desktopID {
        case "i3": return ("square.grid.2x2.fill", [Color(red: 0.20, green: 0.24, blue: 0.30), Color(red: 0.36, green: 0.44, blue: 0.54)])
        case "wayfire": return ("flame.fill", [Color(red: 1.00, green: 0.58, blue: 0.20), Color(red: 0.90, green: 0.25, blue: 0.20)])
        case "hyprland": return ("square.grid.3x2.fill", [Color(red: 0.20, green: 0.80, blue: 0.90), Color(red: 0.25, green: 0.40, blue: 0.95)])
        case "enlightenment": return ("sparkles", [Color(red: 0.62, green: 0.36, blue: 0.95), Color(red: 0.95, green: 0.35, blue: 0.62)])
        case "mate": return ("leaf.fill", [Color(red: 0.45, green: 0.75, blue: 0.30), Color(red: 0.20, green: 0.55, blue: 0.25)])
        case "cinnamon": return ("circle.grid.cross.fill", [Color(red: 0.85, green: 0.55, blue: 0.30), Color(red: 0.60, green: 0.35, blue: 0.20)])
        case "cosmic": return ("sparkle", [Color(red: 0.28, green: 0.75, blue: 0.85), Color(red: 0.55, green: 0.40, blue: 0.95)])
        case "budgie": return ("circle.hexagongrid.fill", [Color(red: 0.30, green: 0.60, blue: 0.95), Color(red: 0.55, green: 0.35, blue: 0.85)])
        default: return ("macwindow.on.rectangle", [Color(red: 0.25, green: 0.50, blue: 1.00), Color(red: 0.36, green: 0.30, blue: 0.90)])
        }
    }
    var body: some View {
        Group {
            if let logo, desktopID == "linuxrunner" {
                // The app's own icon: full-bleed, like on the Home Screen.
                Image(uiImage: logo).resizable().interpolation(.high).scaledToFill()
                    .clipShape(RoundedRectangle(cornerRadius: size * 0.28, style: .continuous))
            } else if let logo {
                RoundedRectangle(cornerRadius: size * 0.28, style: .continuous).fill(Color(white: 0.96))
                    .overlay(Image(uiImage: logo).resizable().interpolation(.high).scaledToFit().padding(size * 0.12))
            } else {
                RoundedRectangle(cornerRadius: size * 0.28, style: .continuous)
                    .fill(LinearGradient(colors: tile.colors, startPoint: .topLeading, endPoint: .bottomTrailing))
                    .overlay(Image(systemName: tile.symbol).font(.system(size: size * 0.42, weight: .semibold)).foregroundColor(.white))
            }
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }
}

/// One desktop in the picker: mark, name, badges, what it is, and its state
/// (selected, ready to use, install, or installing with live progress).
struct DesktopChoiceCard: View {
    let option: DesktopOption
    let distro: String
    let selected: Bool
    let installed: Bool
    let progress: String?
    let enabled: Bool
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(alignment: .top, spacing: 14) {
                DesktopBadge(desktopID: option.id, size: 52)
                VStack(alignment: .leading, spacing: 5) {
                    Text(option.title).font(.headline).foregroundColor(.primary).lineLimit(1).minimumScaleFactor(0.8)
                    if !option.badges(distro).isEmpty {
                        HStack(spacing: 6) {
                            ForEach(option.badges(distro), id: \.self) { badge in
                                StatusChip(text: badge, color: badge == "Recommended" ? .accentColor : badge == "Experimental" ? .orange : .green)
                                    .fixedSize()
                            }
                        }
                    }
                    Text(option.detail).font(.footnote).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
                    if let progress {
                        HStack(spacing: 6) { ProgressView().scaleEffect(0.7); Text(progress).font(.caption).foregroundColor(.secondary).lineLimit(2) }
                    } else if !installed {
                        Label("Install", systemImage: "arrow.down.circle.fill").font(.caption.weight(.semibold)).foregroundColor(.accentColor)
                    }
                }
                Spacer(minLength: 4)
                Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                    .font(.title2).foregroundColor(selected ? .accentColor : Color(.tertiaryLabel))
            }
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
            .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).strokeBorder(selected ? Color.accentColor : .clear, lineWidth: 2))
            .contentShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .opacity(enabled || selected ? 1 : 0.55)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .accessibilityHint(installed ? "Selects this desktop" : "Installs this desktop")
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

// MARK: - CPU & Memory (Settings › Graphics & Performance)

/// What Linux programs are told about CPU cores and memory. Programs size their
/// worker threads (make -j, Blender, browsers, Java) and heaps from these, and
/// guest threads run on separate CPU cores. Applied at the next launch.
struct SystemResourcesView: View {
    @AppStorage(LRSystemCPUCoresKey) private var cores = 0
    @AppStorage(LRSystemMemoryMBKey) private var memoryMB = 0
    private let hostCores = max(1, min(16, ProcessInfo.processInfo.activeProcessorCount))
    /// Advertised RAM tier: iOS reports a little less than the marketed amount
    /// (a 12 GB iPhone can report under 11.5 GiB), so classify instead of rounding.
    private let deviceGB: Int = {
        let gib = Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824
        switch gib {
        case 14...: return 16
        case 10...: return 12
        case 7...: return 8
        case 5...: return 6
        case 3.5...: return 4
        default: return max(1, Int(gib.rounded()))
        }
    }()
    private let appliedCores = ProcessInfo.processInfo.environment["LINUXRUNNER_CPU_CORES"].flatMap(Int.init) ?? 0
    private let appliedMemoryMB = ProcessInfo.processInfo.environment["LINUXRUNNER_MEMORY_MB"].flatMap(Int.init) ?? 0

    /// 12 GB → 8, 16 → 8 (runtime maximum), 8 → 4, 6 → 3, 4 → 2: the rest stays for iOS and the app.
    private var maximumGB: Int {
        let limit = deviceGB >= 12 ? deviceGB - 4 : deviceGB >= 8 ? deviceGB / 2 : deviceGB >= 6 ? 3 : max(1, deviceGB / 2)
        return min(8, max(1, limit))
    }
    /// What iOS lets LinuxRunner use right now (footprint plus what is still
    /// available); nil where iOS reports no app limit (Simulator, Mac).
    private var iosAllowanceGB: Double? {
        let available = os_proc_available_memory()
        guard available > 0 else { return nil }
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let footprint = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
        } == KERN_SUCCESS ? Double(info.phys_footprint) : 0
        return (footprint + Double(available)) / 1_073_741_824
    }
    private var pendingRestart: Bool { cores != appliedCores || memoryMB != appliedMemoryMB }

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                header
                card(title: "CPU Cores", symbol: "cpu.fill", colors: [.indigo, .purple],
                     value: cores == 0 ? "Automatic · 4" : "\(cores) of \(hostCores)",
                     detail: "Linux programs start this many worker threads, and each runs on its own core. More cores speed up compiling, rendering and browsers; Automatic keeps 4, which suits most apps.") {
                    chips(options: [0] + Array(1...hostCores), selection: $cores) { $0 == 0 ? "Auto" : "\($0)" }
                }
                card(title: "Linux Memory", symbol: "memorychip.fill", colors: [.teal, .blue],
                     value: memoryMB == 0 ? "Automatic" : "\(memoryMB / 1024) GB",
                     detail: "The memory Linux programs see. Java, browsers and games size their heaps from it. Up to \(maximumGB) GB on this \(deviceGB) GB device; the rest stays for iOS and LinuxRunner.") {
                    chips(options: [0] + Array(1...maximumGB).map { $0 * 1024 }, selection: $memoryMB) { $0 == 0 ? "Auto" : "\($0 / 1024) GB" }
                    if memoryMB > 0, let allowance = iosAllowanceGB, Double(memoryMB) / 1024 > allowance {
                        Label(String(format: "iOS currently allows LinuxRunner about %.1f GB. Programs that use more than that can be closed by iOS.", allowance), systemImage: "exclamationmark.triangle.fill")
                            .font(.footnote).foregroundStyle(.orange).frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                if pendingRestart {
                    Label("Applies the next time LinuxRunner starts. Close it from the app switcher and open it again.", systemImage: "arrow.clockwise.circle.fill")
                        .font(.footnote.weight(.medium)).foregroundStyle(.blue)
                        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
                        .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Color.blue.opacity(0.12)))
                }
            }
            .padding(.horizontal, 20).padding(.vertical, 12).padding(.bottom, 96)
        }
        .background(Color(.systemGroupedBackground).ignoresSafeArea())
        .navigationTitle("CPU & Memory")
        .navigationBarTitleDisplayMode(.inline)
    }

    private var header: some View {
        HStack(spacing: 14) {
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(LinearGradient(colors: [.indigo, .teal], startPoint: .topLeading, endPoint: .bottomTrailing))
                .frame(width: 54, height: 54)
                .overlay(Image(systemName: "speedometer").font(.system(size: 24, weight: .semibold)).foregroundColor(.white))
            VStack(alignment: .leading, spacing: 3) {
                Text(UIDevice.current.model).font(.headline)
                Text("\(hostCores) CPU cores · \(deviceGB) GB memory").font(.subheadline).foregroundStyle(.secondary)
                if let allowance = iosAllowanceGB {
                    Text(String(format: "iOS allows LinuxRunner about %.1f GB now", allowance)).font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 20, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
    }

    private func card<Content: View>(title: String, symbol: String, colors: [Color], value: String, detail: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                RoundedRectangle(cornerRadius: 9, style: .continuous)
                    .fill(LinearGradient(colors: colors, startPoint: .topLeading, endPoint: .bottomTrailing))
                    .frame(width: 34, height: 34)
                    .overlay(Image(systemName: symbol).font(.system(size: 16, weight: .semibold)).foregroundColor(.white))
                Text(title).font(.headline)
                Spacer()
                Text(value).font(.subheadline.weight(.semibold).monospacedDigit()).foregroundStyle(.secondary)
            }
            content()
            Text(detail).font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 20, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
    }

    private func chips(options: [Int], selection: Binding<Int>, label: @escaping (Int) -> String) -> some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 58), spacing: 8)], spacing: 8) {
            ForEach(options, id: \.self) { option in
                let selected = selection.wrappedValue == option
                Button { withAnimation(.easeOut(duration: 0.2)) { selection.wrappedValue = option } } label: {
                    Text(label(option))
                        .font(.subheadline.weight(selected ? .semibold : .regular).monospacedDigit())
                        .frame(maxWidth: .infinity, minHeight: 38)
                        .foregroundColor(selected ? .white : .primary)
                        .background(Capsule().fill(selected ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(Color(.tertiarySystemGroupedBackground))))
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
    }
}

// MARK: - Shared settings design (GPU, Devices, File Sharing)
// Cards in the style of CPU & Memory: gradient icon tile, headline, short
// value, content, and long explanations folded into "More info".
struct LRIconTile: View {
    let symbol: String; let colors: [Color]; var size: CGFloat = 34
    var body: some View {
        RoundedRectangle(cornerRadius: size * 0.27, style: .continuous)
            .fill(LinearGradient(colors: colors, startPoint: .topLeading, endPoint: .bottomTrailing))
            .frame(width: size, height: size)
            .overlay(Image(systemName: symbol).font(.system(size: size * 0.47, weight: .semibold)).foregroundColor(.white))
    }
}
struct LRCard<Content: View>: View {
    let title: String; let symbol: String; let colors: [Color]; var value: String? = nil
    @ViewBuilder let content: () -> Content
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                LRIconTile(symbol: symbol, colors: colors)
                Text(title).font(.headline)
                Spacer()
                if let value { Text(value).font(.subheadline.weight(.semibold)).foregroundStyle(.secondary).lineLimit(1) }
            }
            content()
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 20, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
    }
}
struct LRChoice: Identifiable { let id: String; let title: String; let subtitle: String; let symbol: String }
struct LRChoiceList: View {
    let choices: [LRChoice]; @Binding var selection: String
    var body: some View {
        VStack(spacing: 8) {
            ForEach(choices) { choice in
                let selected = selection == choice.id
                Button { withAnimation(.easeOut(duration: 0.2)) { selection = choice.id } } label: {
                    HStack(spacing: 12) {
                        Image(systemName: choice.symbol).font(.system(size: 16, weight: .semibold))
                            .foregroundColor(selected ? .white : .accentColor)
                            .frame(width: 34, height: 34)
                            .background(Circle().fill(selected ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(Color.accentColor.opacity(0.14))))
                        VStack(alignment: .leading, spacing: 2) {
                            Text(choice.title).font(.subheadline.weight(.semibold)).foregroundColor(.primary)
                            Text(choice.subtitle).font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                        Spacer(minLength: 4)
                        Image(systemName: selected ? "checkmark.circle.fill" : "circle").font(.title3)
                            .foregroundColor(selected ? .accentColor : Color(.tertiaryLabel))
                    }
                    .padding(10)
                    .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color(.tertiarySystemGroupedBackground)))
                    .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).stroke(selected ? Color.accentColor.opacity(0.6) : .clear, lineWidth: 1.5))
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
    }
}
struct LRStatusPill: View {
    let text: String; let color: Color
    var body: some View {
        Text(text).font(.caption2.weight(.semibold)).lineLimit(1)
            .padding(.horizontal, 8).padding(.vertical, 4)
            .foregroundColor(color).background(Capsule().fill(color.opacity(0.15)))
    }
}
struct LRMoreInfo<Content: View>: View {
    var title = "More info"
    @ViewBuilder let content: () -> Content
    @State private var open = false
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button { withAnimation(.easeOut(duration: 0.2)) { open.toggle() } } label: {
                HStack(spacing: 4) {
                    Text(title).font(.footnote.weight(.semibold))
                    Image(systemName: "chevron.down").font(.caption2.weight(.bold)).rotationEffect(.degrees(open ? 180 : 0))
                }.foregroundColor(.accentColor)
            }.buttonStyle(.plain)
            if open { content().font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
        }
    }
}
struct LRNavRow: View {
    let title: String; let subtitle: String?; let symbol: String; let colors: [Color]
    var body: some View {
        HStack(spacing: 12) {
            LRIconTile(symbol: symbol, colors: colors, size: 32)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.semibold)).foregroundColor(.primary)
                if let subtitle { Text(subtitle).font(.caption).foregroundColor(.secondary).lineLimit(1) }
            }
            Spacer(minLength: 4)
            Image(systemName: "chevron.right").font(.caption.weight(.semibold)).foregroundColor(Color(.tertiaryLabel))
        }
        .padding(.vertical, 4).contentShape(Rectangle())
    }
}
