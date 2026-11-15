import SwiftUI
import UIKit
import CoreImage

// MARK: - LinuxRunner Modes (Settings › Modes)
// Opt-in ways to use the active distribution. Nothing installs or starts
// until the user taps it; modes.sh (GUISupport) runs the services and
// desktop-profile.sh mode-* installs them from the distribution's packages.

private struct ModeTerminal: Identifiable { let id = UUID(); let controller: TerminalViewController }

struct LRMode: Identifiable {
    let id: String              // agent | rdp | ssh
    let title: String
    let tagline: String
    let symbol: String
    let colors: [Color]
    let installMode: String     // desktop-profile.sh verb
}

struct ModesView: View {
    @State private var status: [String: String] = [:]
    @State private var installing: String?
    @State private var progress: String?
    @State private var busy: String?
    @State private var failure: [String: String] = [:]
    @State private var terminal: ModeTerminal?
    @State private var copied: String?
    @State private var running = LRRuntimeBridge.isRunning()
    @State private var ticks = 0
    private let refreshTimer = Timer.publish(every: 3, on: .main, in: .common).autoconnect()

    static let modes: [LRMode] = [
        LRMode(id: "agent", title: "Agent Mode", tagline: "AI coding agents in your Linux terminal",
               symbol: "sparkles", colors: [Color(red: 0.85, green: 0.45, blue: 0.25), .pink], installMode: "mode-agent"),
        LRMode(id: "rdp", title: "RDP Mode", tagline: "Use this desktop from a PC or Mac",
               symbol: "display.2", colors: [.blue, .indigo], installMode: "mode-rdp"),
        LRMode(id: "ssh", title: "SSH Mode", tagline: "Terminal, VS Code Remote and file transfer",
               symbol: "terminal.fill", colors: [.green, .teal], installMode: "mode-ssh"),
        LRMode(id: "liftoff", title: "Liftoff", tagline: "Build web apps and preview them in Safari",
               symbol: "paperplane.fill", colors: [.purple, .orange], installMode: "mode-liftoff"),
        LRMode(id: "pocket", title: "Pocket Cloud", tagline: "Your iPhone as a Wi-Fi file server",
               symbol: "externaldrive.fill.badge.wifi", colors: [.cyan, .blue], installMode: "mode-pocket"),
        LRMode(id: "arcadia", title: "Arcadia", tagline: "A retro game lab with RetroArch",
               symbol: "gamecontroller.fill", colors: [.pink, .purple], installMode: "mode-arcadia"),
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                hero
                if !running {
                    notice("Start Linux on the Home screen to install or use a mode.", symbol: "power", color: .orange)
                }
                ForEach(Self.modes) { mode in card(mode) }
                Text("Modes are off until you start them and stop when Linux stops. Installs use \(DesktopCatalog.distributionName(DesktopCatalog.activeDistribution()))'s own packages (network needed once). Network modes are reachable only by devices on the same Wi-Fi while LinuxRunner is open or running in the background.")
                    .font(.footnote).foregroundColor(.secondary).padding(.horizontal, 4)
            }
            .padding(16)
        }
        .background(Color(.systemGroupedBackground).ignoresSafeArea())
        .navigationTitle("Modes")
        .onAppear { refresh(runStatus: true) }
        .onReceive(refreshTimer) { _ in ticks += 1; refresh(runStatus: ticks % 3 == 0 && busy == nil) }
        .fullScreenCover(item: $terminal, onDismiss: { refresh(runStatus: true) }) { item in TerminalScreen(controller: item.controller) }
    }

    // MARK: Hero
    private var hero: some View {
        HStack(spacing: 14) {
            ZStack {
                ForEach(Array(Self.modes.filter { ["agent", "liftoff", "arcadia"].contains($0.id) }.enumerated()), id: \.offset) { index, mode in
                    LRIconTile(symbol: mode.symbol, colors: mode.colors, size: 38)
                        .rotationEffect(.degrees(Double(index - 1) * 12))
                        .offset(x: CGFloat(index - 1) * 14, y: index == 1 ? -4 : 4)
                        .shadow(color: .black.opacity(0.18), radius: 4, y: 2)
                }
            }
            .frame(width: 84, height: 64)
            VStack(alignment: .leading, spacing: 4) {
                Text("LinuxRunner Modes").font(.title3.weight(.bold))
                Text("Turn \(DesktopCatalog.distributionName(DesktopCatalog.activeDistribution())) into an AI workstation, a remote desktop, a web lab, a file server or a retro console, only when you want.")
                    .font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 22, style: .continuous)
            .fill(LinearGradient(colors: [Color.purple.opacity(0.22), Color.blue.opacity(0.12), Color(.secondarySystemGroupedBackground)],
                                 startPoint: .topLeading, endPoint: .bottomTrailing)))
    }

    // MARK: Cards
    private func installed(_ mode: LRMode) -> Bool {
        switch mode.id {
        case "agent": return !(status["agents"] ?? "").trimmingCharacters(in: .whitespaces).isEmpty
        case "rdp": return status["rdp_installed"] == "1"
        case "ssh": return status["ssh_installed"] == "1"
        default: return status["\(mode.id)_installed"] == "1"
        }
    }
    private func active(_ mode: LRMode) -> Bool { status[mode.id] == "running" }

    private func pill(_ mode: LRMode) -> (String, Color) {
        if installing == mode.id { return ("Installing", .orange) }
        if active(mode) { return ("Running", .green) }
        if installed(mode) { return ("Ready", .accentColor) }
        return ("Not installed", .secondary)
    }

    @ViewBuilder private func card(_ mode: LRMode) -> some View {
        let state = pill(mode)
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                LRIconTile(symbol: mode.symbol, colors: mode.colors, size: 44)
                VStack(alignment: .leading, spacing: 2) {
                    Text(mode.title).font(.headline)
                    Text(mode.tagline).font(.caption).foregroundColor(.secondary)
                }
                Spacer(minLength: 4)
                HStack(spacing: 5) {
                    if active(mode) { Circle().fill(Color.green).frame(width: 7, height: 7) }
                    LRStatusPill(text: state.0, color: state.1)
                }
            }
            details(mode)
            if installing == mode.id {
                HStack(spacing: 10) {
                    ProgressView()
                    Text(progress ?? "Starting").font(.footnote).foregroundColor(.secondary).lineLimit(2)
                }
            }
            if let message = failure[mode.id] { notice(message, symbol: "exclamationmark.triangle.fill", color: .orange) }
            actions(mode)
            LRMoreInfo(title: "How it works") { Text(help(mode)) }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 22, style: .continuous).fill(Color(.secondarySystemGroupedBackground)))
        .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous)
            .stroke(LinearGradient(colors: mode.colors.map { $0.opacity(active(mode) ? 0.8 : 0) }, startPoint: .leading, endPoint: .trailing), lineWidth: 1.5))
        .animation(.easeOut(duration: 0.2), value: status)
    }

    @ViewBuilder private func details(_ mode: LRMode) -> some View {
        switch mode.id {
        case "agent":
            HStack(spacing: 8) {
                ForEach([("claude", "Claude Code"), ("codex", "Codex CLI"), ("opencode", "OpenCode")], id: \.0) { command, name in
                    let present = (status["agents"] ?? "").split(separator: " ").contains(Substring(command))
                    HStack(spacing: 4) {
                        Image(systemName: present ? "checkmark.circle.fill" : "circle.dashed").font(.caption2)
                        Text(name).font(.caption2.weight(.semibold)).lineLimit(1)
                    }
                    .foregroundColor(present ? .primary : .secondary)
                    .padding(.horizontal, 8).padding(.vertical, 5)
                    .background(Capsule().fill(Color(.tertiarySystemGroupedBackground)))
                }
            }
        case "rdp" where active(mode):
            connectionBox(rows: [("Computer", "\(Self.wifiAddress() ?? "iPhone Wi-Fi address"):\(status["rdp.port"] ?? "3389")"),
                                 ("Password", LRRuntimeBridge.desktopSessionPassword() ?? "Open the desktop first")])
            Text("Connect with any Remote Desktop app, then type this password on the LinuxRunner login screen that appears (leave the app's own user name and password empty). It changes every time the desktop starts.")
                .font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
        case "rdp" where installed(mode) && status["desktop"] != "running":
            notice("Open Desktop first; RDP Mode shares the desktop that is running.", symbol: "macwindow", color: .blue)
        case "ssh" where active(mode):
            let user = LRRuntimeBridge.defaultLinuxUser()
            connectionBox(rows: [("Command", "ssh -p \(status["ssh.port"] ?? "2222") \(user == "root" || user.isEmpty ? "user" : user)@\(Self.wifiAddress() ?? "iPhone-IP")"),
                                 ("Password", "Your Linux user's password")])
        case "liftoff" where active(mode):
            let port = status["liftoff.port"] ?? "8080"
            let lan = "http://\(Self.wifiAddress() ?? "iPhone-IP"):\(port)"
            HStack(alignment: .top, spacing: 12) {
                connectionBox(rows: [("This iPhone", "http://localhost:\(port)"), ("Wi-Fi", lan)])
                if let qr = Self.qrCode(lan) {
                    Image(uiImage: qr).interpolation(.none).resizable().frame(width: 84, height: 84)
                        .padding(6).background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color.white))
                        .accessibilityLabel("QR code for \(lan)")
                }
            }
        case "pocket" where active(mode):
            connectionBox(rows: [("Address", "http://\(Self.wifiAddress() ?? "iPhone-IP"):\(status["pocket.port"] ?? "8081")"),
                                 ("User", status["pocket.user"] ?? "linuxrunner"),
                                 ("Password", status["pocket.password"] ?? "…")])
        case "arcadia" where installed(mode):
            HStack(spacing: 8) {
                ForEach(["NES", "Game Boy", "GBA"], id: \.self) { system in
                    Text(system).font(.caption2.weight(.semibold)).padding(.horizontal, 8).padding(.vertical, 5)
                        .background(Capsule().fill(Color(.tertiarySystemGroupedBackground)))
                }
            }
        default: EmptyView()
        }
    }

    private func connectionBox(rows: [(String, String)]) -> some View {
        VStack(spacing: 0) {
            ForEach(Array(rows.enumerated()), id: \.offset) { index, row in
                HStack {
                    Text(row.0).font(.caption).foregroundColor(.secondary).frame(width: 74, alignment: .leading)
                    Text(row.1).font(.system(.footnote, design: .monospaced).weight(.semibold)).lineLimit(1).minimumScaleFactor(0.7).textSelection(.enabled)
                    Spacer(minLength: 4)
                    Button {
                        UIPasteboard.general.string = row.1
                        withAnimation { copied = row.1 }
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { if copied == row.1 { withAnimation { copied = nil } } }
                    } label: {
                        Image(systemName: copied == row.1 ? "checkmark" : "doc.on.doc").font(.caption.weight(.semibold))
                    }.buttonStyle(.borderless)
                }
                .padding(.horizontal, 12).padding(.vertical, 9)
                if index < rows.count - 1 { Divider().padding(.leading, 12) }
            }
        }
        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color(.tertiarySystemGroupedBackground)))
    }

    @ViewBuilder private func actions(_ mode: LRMode) -> some View {
        let disabled = !running || installing != nil || busy != nil
        HStack(spacing: 10) {
            if !installed(mode) {
                primary("Install", symbol: "arrow.down.circle.fill", colors: mode.colors) { install(mode) }.disabled(disabled)
            } else if mode.id == "arcadia" {
                primary("Play", symbol: "gamecontroller.fill", colors: mode.colors) { launchArcadia() }.disabled(!running)
                secondary("Update", symbol: "arrow.triangle.2.circlepath") { install(mode) }.disabled(disabled)
            } else if mode.id == "liftoff" && active(mode) {
                primary("Open in Safari", symbol: "safari.fill", colors: mode.colors) {
                    if let url = URL(string: "http://localhost:\(status["liftoff.port"] ?? "8080")") { UIApplication.shared.open(url) }
                }
                secondary(busy == mode.id ? "Stopping…" : "Stop", symbol: "stop.fill") { command(["stop", mode.id], mode) }.disabled(disabled)
            } else if mode.id == "agent" {
                primary("Open Agent Terminal", symbol: "terminal.fill", colors: mode.colors) { openAgent() }.disabled(!running)
                secondary("Update", symbol: "arrow.triangle.2.circlepath") { install(mode) }.disabled(disabled)
            } else if active(mode) {
                primary(busy == mode.id ? "Stopping…" : "Stop", symbol: "stop.fill", colors: [.red, .orange]) { command(["stop", mode.id], mode) }.disabled(disabled)
            } else {
                primary(busy == mode.id ? "Starting…" : "Start", symbol: "play.fill", colors: mode.colors) { command(["start", mode.id], mode) }
                    .disabled(disabled || (mode.id == "rdp" && status["desktop"] != "running"))
            }
        }
    }

    private func primary(_ title: String, symbol: String, colors: [Color], action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: symbol).font(.subheadline.weight(.semibold)).foregroundColor(.white)
                .frame(maxWidth: .infinity).padding(.vertical, 11)
                .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(LinearGradient(colors: colors, startPoint: .leading, endPoint: .trailing)))
        }.buttonStyle(.plain)
    }
    private func secondary(_ title: String, symbol: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: symbol).font(.subheadline.weight(.semibold))
                .padding(.vertical, 11).padding(.horizontal, 14)
                .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color(.tertiarySystemGroupedBackground)))
        }.buttonStyle(.plain)
    }
    private func notice(_ text: String, symbol: String, color: Color) -> some View {
        Label(text, systemImage: symbol).font(.footnote).foregroundColor(color)
            .padding(12).frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(color.opacity(0.12)))
    }

    private func help(_ mode: LRMode) -> String {
        switch mode.id {
        case "agent":
            return "Installs Node.js, git, ripgrep and Python, then Claude Code, Codex CLI and OpenCode from npm. Open Agent Terminal shows a menu of the installed agents and starts them in ~/Projects as your Linux user. Each agent asks you to sign in with your own account the first time; LinuxRunner does not see or store your keys. Agents run on LinuxRunner's interpreter, so they start slower than on a computer."
        case "rdp":
            return "Shares the desktop that is open in LinuxRunner with Remote Desktop clients: Windows Remote Desktop Connection, Microsoft's Windows App on Mac, iPad and Android, or Remmina on Linux. On a device on the same Wi-Fi, connect to the Computer address; when the LinuxRunner login screen appears, type the Password there (not in the app's own sign-in box). It changes each time the desktop starts. The iPhone and the computer see and control the same desktop."
        case "liftoff":
            return "Installs Python, Node.js and git. Start serves the ~/Sites folder of your Linux user on port 8080 (a starter page is created the first time). Open in Safari on this iPhone, or scan the QR code from another phone or computer on the same Wi-Fi. Any dev server you start in a terminal on 0.0.0.0 (npm run dev -- --host, Flask, Django) is reachable the same way on its own port."
        case "pocket":
            return "Shares the ~/Pocket Cloud folder of your Linux user over WebDAV on port 8081, with a new password every start. Mac: Finder › Go › Connect to Server and enter the Address. iPhone/iPad: Files › … › Connect to Server. Windows and Android: any WebDAV app (or open the Address in a browser to browse and download). Put files there from Linux, or from Files › LinuxRunner."
        case "arcadia":
            return "Installs RetroArch with the classic-console cores your distribution offers: NES, Game Boy/Color and Game Boy Advance on Debian; other distributions may offer more (SNES, Mega Drive). Play opens RetroArch in the LinuxRunner desktop; connect a Bluetooth controller or use the keyboard. No games are included: load your own legally obtained game files from the Files app. Older consoles run best; LinuxRunner's interpreter limits speed."
        default:
            return "Starts an OpenSSH server on port 2222. From a computer on the same Wi-Fi run the Command, or connect VS Code (Remote - SSH), scp, rsync or an SFTP app. Sign in as a Linux user with its password (set one in Settings › Linux Users); root login is refused."
        }
    }

    // MARK: Actions
    private func refresh(runStatus: Bool) {
        running = LRRuntimeBridge.isRunning()
        if runStatus && running && installing == nil { _ = LRRuntimeBridge.modesCommand(["status"]) }
        status = LRRuntimeBridge.modesStatus()
    }

    private func install(_ mode: LRMode) {
        failure[mode.id] = nil; installing = mode.id; progress = "Starting"
        LRRuntimeBridge.installDesktop(mode.installMode, progress: { progress = $0 }) { error in
            installing = nil; progress = nil
            if let error { failure[mode.id] = error.localizedDescription }
            refresh(runStatus: true)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { refresh(runStatus: false) }
        }
    }

    private func command(_ arguments: [String], _ mode: LRMode) {
        failure[mode.id] = nil; busy = mode.id
        var arguments = arguments
        if arguments.first == "start" && ["liftoff", "pocket"].contains(mode.id) {
            let user = LRRuntimeBridge.defaultLinuxUser()
            arguments.append(user.isEmpty ? "root" : user)
        }
        guard LRRuntimeBridge.modesCommand(arguments) != nil else {
            busy = nil; failure[mode.id] = "LinuxRunner could not start the command. Restart Linux and try again."; return
        }
        // modes.sh rewrites modes-status when it finishes (a few seconds).
        var checks = 0
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { timer in
            checks += 1
            status = LRRuntimeBridge.modesStatus()
            let wanted = arguments[0] == "start" ? "running" : "stopped"
            if status[mode.id] == wanted || checks >= 15 {
                timer.invalidate(); busy = nil
                if status[mode.id] != wanted {
                    failure[mode.id] = arguments[0] == "start"
                        ? "\(mode.title) did not start. Export the Desktop Report (Settings › Storage & Maintenance) and send it."
                        : "\(mode.title) did not stop yet. Try again."
                }
            }
        }
    }

    /// RetroArch opens inside the LinuxRunner desktop (ManagerView presents it).
    private func launchArcadia() {
        NotificationCenter.default.post(name: Notification.Name("LinuxRunnerLaunchGraphicalApplication"), object: nil,
                                        userInfo: ["executable": "retroarch", "arguments": [String](), "directory": "/root"])
    }

    static func qrCode(_ text: String) -> UIImage? {
        guard let filter = CIFilter(name: "CIQRCodeGenerator") else { return nil }
        filter.setValue(Data(text.utf8), forKey: "inputMessage"); filter.setValue("M", forKey: "inputCorrectionLevel")
        guard let image = filter.outputImage?.transformed(by: CGAffineTransform(scaleX: 6, y: 6)),
              let cg = CIContext().createCGImage(image, from: image.extent) else { return nil }
        return UIImage(cgImage: cg)
    }

    private func openAgent() {
        let user = LRRuntimeBridge.defaultLinuxUser()
        guard let controller = LRRuntimeBridge.modesCommand(["agent", user.isEmpty ? "root" : user]) else { return }
        terminal = ModeTerminal(controller: controller)
    }

    /// The iPhone's Wi-Fi IPv4 address (en0), for connection instructions.
    static func wifiAddress() -> String? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0 else { return nil }
        defer { freeifaddrs(list) }
        var cursor = list
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            guard let address = entry.pointee.ifa_addr, address.pointee.sa_family == UInt8(AF_INET),
                  String(cString: entry.pointee.ifa_name) == "en0" else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(address, socklen_t(address.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 { return String(cString: host) }
        }
        return nil
    }
}
