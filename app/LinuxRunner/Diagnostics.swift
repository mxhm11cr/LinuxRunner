import Foundation

enum DiagnosticLevel: Int, Codable, CaseIterable { case error, warning, info, debug, trace
    var label: String { ["ERROR", "WARNING", "INFO", "DEBUG", "TRACE"][rawValue] }
}
struct DiagnosticEvent: Codable {
    var timestamp = Date()
    var level: DiagnosticLevel
    var category: String
    var message: String
    var pid: Int?
    var tid: Int?
}
enum DiagnosticPrivacy {
    static func redact(_ input: String) -> String {
        // Fail closed for payloads whose meaning cannot be safely inferred.
        if ["JavaScript result=", "bytes_hex=", "BROWSER_TEST:", "key_text=", "Authorization:", "Set-Cookie:", "Cookie:"].contains(where: input.localizedCaseInsensitiveContains) { return "[private payload omitted]" }
        if input.contains("-----BEGIN") { return "[key material omitted]" }
        var result = input
        let patterns = [
            (#"(?i)(password|passwd|token|secret|api[_-]?key|cookie|authorization)"?\s*[:=]\s*(\"[^\"]*\"|'[^']*'|[^\s,;]+)"#, "$1=[REDACTED]"),
            (#"(?i)Bearer\s+\S+"#, "Bearer [REDACTED]"),
            (#"(https?://)[^/\s]*@"#, "$1[REDACTED]@"),
            (#"(https?://[^/\s?#]+)[/][^\s]*"#, "$1/[path omitted]"),
            (#"(https?://[^\s?#]+)[?#][^\s]*"#, "$1?[REDACTED]"),
            (#"title=.*$"#, "title=[omitted]")]
        for (pattern, replacement) in patterns {
            if let regex = try? NSRegularExpression(pattern: pattern) {
                result = regex.stringByReplacingMatches(in: result, range: NSRange(result.startIndex..., in: result), withTemplate: replacement)
            }
        }
        return String(result.prefix(4096))
    }
    static func crashOutput(_ input: String) -> String {
        let log = input.lowercased()
        let diagnostic = ["fatal", "assert", "check failed", "error", "warning", "sandbox", "libc++", "terminat", "out_of_range", "string_view::substr"]
        let qtPlatform = ["qt.qpa.", "Could not load the Qt platform plugin", "This application failed to start because no Qt platform plugin", "Available platform plugins are:"]
        guard diagnostic.contains(where: log.contains) || qtPlatform.contains(where: input.hasPrefix) else { return "[non-diagnostic application output omitted]" }
        return String(redact(input).prefix(640))
    }
    static func applicationExport(_ input: String) -> String {
        // Preserve raw stdout/stderr in private session logs. Default exports
        // include diagnostic records, not arbitrary user program output.
        input.split(separator: "\n", omittingEmptySubsequences: false).map { line in
            let s = String(line)
            let allowed = ["GUI_", "DESKTOP_", "X11_", "FOCUS:", "QT_TEST:", "BROWSER:", "BROWSER_", "FIREFOX_", "[Parent", "Gtk-WARNING", "WARNING", "ERROR", "FATAL", "Crash Annotation", "Xlib:", "xauth:", "QPainter::", "QWidget::", "QEGLPlatformContext:", "qt.qpa.", "Could not load the Qt platform plugin", "This application failed to start because no Qt platform plugin", "Available platform plugins are:"]
            return allowed.contains(where: s.hasPrefix) ? redact(s) : "[unstructured application output omitted]"
        }.joined(separator: "\n")
    }
}

// Crash reports contain structured runtime evidence, never arbitrary application
// buffers or browser output. A preceding errno is evidence, not proven causality.
struct GuestCrashReport: Codable, Identifiable {
    var id = UUID()
    var timestamp = Date()
    var metadata: [String: String]
    var context: [String]
    var signal: Int { Int(metadata["signal"] ?? "0") ?? 0 }
    var application: String { metadata["application"] ?? "Linux application" }
    var category: String {
        if signal == 4 { return "Illegal or unsupported instruction" }
        if signal == 11 || signal == 7 { return "Memory access violation" }
        if signal == 5 { return "Guest trap (BRK/SIGTRAP)" }
        if signal == 6 { return "Guest abort (SIGABRT)" }
        return "Nonzero guest exit"
    }
    var recentResourceFailure: Bool { context.contains { $0.contains("result=-24 ") || $0.hasSuffix("result=-24") || $0.contains("result=-23 ") || $0.hasSuffix("result=-23") } }
    var summary: String {
        let action = signal == 0 ? "exited with status \(metadata["exit_code"] ?? "unknown")" : "crashed — \(category)"
        return "\(application) \(action)" + (recentResourceFailure ? "; recent descriptor exhaustion (cause suspected)" : "")
    }
    static func safeContext(_ line: String) -> String? {
        guard ["FAULT_ID ", "FAULT_MAP ", "FAULT_SYMBOL ", "FAULT_CODE ", "FAULT_REG ", "FAULT_CALL ", "FAULT_PATH ", "FD_SNAPSHOT", "FD_PRESSURE", "FD_LIVE ", "PROCESS_EVENT ", "PROCESS_ROLE ", "MMAP_FAILURE ", "FAULT_STDOUT ", "FAULT_STDERR ", "FD_OWNER ", "FD_ORIGIN ", "FD_TREND ", "FD_CENSUS_LIMIT ", "ENGINE_INTERRUPT ", "FUTEX_ERROR:", "POLL_BACKEND_FAILURE:"].contains(where: line.hasPrefix) else { return nil }
        return DiagnosticPrivacy.redact(line)
    }
    var probableCause: String {
        let output = context.filter { $0.hasPrefix("FAULT_STDERR ") || $0.hasPrefix("FAULT_STDOUT ") }.joined(separator: "\n")
        let runtimeExit = metadata["signal"] == "0" && metadata["exit_code"].map { code in
            context.contains { $0.contains("kind=termination_request") && $0.contains("status_linux=\(code) ") && $0.contains("origin=runtime_call") }
        } == true
        if context.contains(where: { $0.hasPrefix("ENGINE_INTERRUPT ") }) || runtimeExit { return "High confidence: the recorded termination originated in the runtime. Inspect the matching exit request or engine interrupt; RET/EINTR does not explain its underlying cause." }
        if output.contains("Running as root without --no-sandbox") { return "High confidence: application explicitly refused a root session. This is an application safety check; changing the last RET or futex cannot repair it." }
        if output.contains("SUID sandbox helper") || output.contains("setuid_sandbox_host.cc") { return "High confidence: application reported a sandbox-helper configuration failure. Verify guest identity, imported owner/mode and supported isolation; sandbox support is not established by changing permissions alone." }
        if output.contains("string_view::substr") { return "Medium confidence: application reported a C++ string-view bounds failure. Its triggering caller or input still needs investigation." }
        if output.lowercased().contains("fatal") || output.lowercased().contains("check failed") { return "Medium confidence: an application fatal/assertion message was captured below. The violated check is evidence; its underlying runtime cause is not yet established." }
        if recentResourceFailure { return "Low confidence: recent descriptor exhaustion was observed, but causality is unproven. Check global FD trends and the actual exit requester." }
        return "Unknown: signal/exit status is established, but no causal fatal message was captured. A RET instruction or futex EINTR is not itself the cause of termination."
    }
    var fullText: String {
        let format = ISO8601DateFormatter()
        return "LinuxRunner Application Crash Diagnostics\n\(summary)\nTimestamp: \(format.string(from: timestamp))\nClassification: \(category)\nProbable cause: \(probableCause)\nSignal/exit status is confirmed. A preceding syscall error does not prove the crash cause.\n" + metadata.keys.sorted().map { "\($0): \(metadata[$0] ?? "")" }.joined(separator: "\n") + "\n\nRuntime context (frame-pointer walk is best effort; symbols appear only for exact exported/function ranges):\n" + context.joined(separator: "\n") + "\n\nPrivacy: bounded stdout/stderr tails are filtered to diagnostic lines and redacted before persistence. Arbitrary output, input, cookies, browser contents and environment variables are omitted. Error evidence is bounded; missing context is not a successful test.\n"
    }
}
final class GuestCrashStore {
    private let lock = NSLock()
    private let directory: URL
    private var saved = Set<UUID>()
    private var reports: [GuestCrashReport] = []
    private(set) var persistenceError: String?
    init(directory: URL) throws {
        self.directory = directory
        guard (try? directory.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true else { throw EnvironmentError.invalidMetadata }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let decoder = JSONDecoder(); decoder.dateDecodingStrategy = .millisecondsSince1970
        let legacy = JSONDecoder(); legacy.dateDecodingStrategy = .iso8601
        for url in try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: [.isSymbolicLinkKey, .fileSizeKey]) {
            guard url.lastPathComponent.hasPrefix("crash-"), url.pathExtension == "json",
                  let values = try? url.resourceValues(forKeys: [.isSymbolicLinkKey, .fileSizeKey]), values.isSymbolicLink != true, (values.fileSize ?? Int.max) <= 131072,
                  let data = try? Data(contentsOf: url), let report = (try? decoder.decode(GuestCrashReport.self, from: data)) ?? (try? legacy.decode(GuestCrashReport.self, from: data)), url.lastPathComponent == "crash-\(report.id.uuidString).json" else { continue }
            reports.append(report); saved.insert(report.id)
        }
        reports.sort { $0.timestamp > $1.timestamp }; prune()
    }
    private func path(_ id: UUID) -> URL { directory.appendingPathComponent("crash-\(id.uuidString).json") }
    private func prune() {
        for report in reports.dropFirst(20) { try? FileManager.default.removeItem(at: path(report.id)); saved.remove(report.id) }
        reports = Array(reports.prefix(20))
    }
    private func persistPending() {
        let encoder = JSONEncoder(); encoder.dateEncodingStrategy = .millisecondsSince1970
        for report in reports where !saved.contains(report.id) {
            do {
                let url = path(report.id)
                guard (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true else { throw EnvironmentError.invalidMetadata }
                let data = try encoder.encode(report); guard data.count <= 131072 else { throw EnvironmentError.invalidMetadata }
                try data.write(to: url, options: .atomic)
                try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
                saved.insert(report.id); persistenceError = nil
            } catch { persistenceError = "Crash report retained in memory; persistence failed: \(error.localizedDescription)"; break }
        }
    }
    func append(_ report: GuestCrashReport) {
        lock.lock(); defer { lock.unlock() }
        reports.insert(report, at: 0); prune(); persistPending()
    }
    func entries() -> [GuestCrashReport] {
        lock.lock(); defer { lock.unlock() }; persistPending(); return reports
    }
}

final class StructuredLogStore {
    let directory: URL
    var maximumBytes: Int
    var retainedFiles: Int
    private let lock = NSLock()
    private var handle: FileHandle?
    private var size = 0
    init(directory: URL, maximumBytes: Int = 1024 * 1024, retainedFiles: Int = 4) throws {
        self.directory = directory; self.maximumBytes = max(4096, maximumBytes); self.retainedFiles = max(1, min(16, retainedFiles))
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try reopen()
    }
    private func reopen() throws {
        let url = directory.appendingPathComponent("events.jsonl")
        guard (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true else { throw EnvironmentError.invalidMetadata }
        if !FileManager.default.fileExists(atPath: url.path) { FileManager.default.createFile(atPath: url.path, contents: nil, attributes: [.posixPermissions: 0o600]) }
        handle = try FileHandle(forWritingTo: url); size = Int(try handle!.seekToEnd())
    }
    func setRetention(_ count: Int) {
        lock.lock(); defer { lock.unlock() }
        retainedFiles = max(1, min(16, count))
        if retainedFiles < 16 { for n in (retainedFiles + 1)...16 { try? FileManager.default.removeItem(at: directory.appendingPathComponent("events.\(n).jsonl")) } }
    }
    func append(_ event: DiagnosticEvent) throws {
        lock.lock(); defer { lock.unlock() }
        var safe = event; safe.message = DiagnosticPrivacy.redact(event.message)
        let encoder = JSONEncoder(); encoder.dateEncodingStrategy = .iso8601
        var data = try encoder.encode(safe); data.append(10)
        if size + data.count > maximumBytes {
            try handle?.close(); handle = nil
            let fm = FileManager.default
            for n in stride(from: retainedFiles, through: 1, by: -1) {
                let target = directory.appendingPathComponent("events.\(n).jsonl")
                if fm.fileExists(atPath: target.path) { try fm.removeItem(at: target) }
                let source = directory.appendingPathComponent(n == 1 ? "events.jsonl" : "events.\(n-1).jsonl")
                if fm.fileExists(atPath: source.path) { try fm.moveItem(at: source, to: target) }
            }
            try reopen()
        }
        try handle?.write(contentsOf: data); size += data.count
    }
    func read() -> [DiagnosticEvent] {
        lock.lock(); defer { lock.unlock() }
        let decoder = JSONDecoder(); decoder.dateDecodingStrategy = .iso8601
        let paths = (1...retainedFiles).reversed().map { directory.appendingPathComponent("events.\($0).jsonl") } + [directory.appendingPathComponent("events.jsonl")]
        return paths.flatMap { url -> [DiagnosticEvent] in
            guard (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true,
                  let data = try? Data(contentsOf: url), data.count <= maximumBytes + 8192 else { return [] }
            return data.split(separator: 10).compactMap { try? decoder.decode(DiagnosticEvent.self, from: Data($0)) }
        }.suffix(1000).map { $0 }
    }
}
struct StorageUsage: Codable {
    var rootfs: Int64 = 0
    var exchange: Int64 = 0
    var packageCache: Int64 = 0
    var diagnostics: Int64 = 0
    var downloads: Int64 = 0
    var temporary: Int64 = 0
    var total: Int64 { rootfs + exchange + diagnostics + downloads + temporary }
    static func bytes(_ root: URL) -> Int64 {
        measuredBytes(root, isCancelled: { false }) ?? 0
    }
    // A cancelled scan has no result: callers must retain their last complete
    // measurement instead of publishing a partial/zero storage total.
    static func measuredBytes(_ root: URL, isCancelled: () -> Bool) -> Int64? {
        guard !isCancelled() else { return nil }
        var sum: Int64 = 0
        var visited = Set<String>()
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .isSymbolicLinkKey, .fileAllocatedSizeKey, .fileResourceIdentifierKey]
        guard (try? root.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true,
              let files = FileManager.default.enumerator(at: root, includingPropertiesForKeys: Array(keys)) else { return 0 }
        for case let url as URL in files {
            guard !isCancelled() else { return nil }
            guard let values = try? url.resourceValues(forKeys: keys), values.isSymbolicLink != true else { continue }
            if values.isRegularFile == true {
                let attributes = try? FileManager.default.attributesOfItem(atPath: url.path)
                let id = "\(attributes?[.systemNumber] ?? ""):\(attributes?[.systemFileNumber] ?? url.path)"
                if visited.insert(id).inserted { sum += Int64(values.fileAllocatedSize ?? 0) }
            }
        }
        return isCancelled() ? nil : sum
    }
}
#if canImport(UIKit)
import UIKit
import SwiftUI
@objc(LRGuestCrashCenter) final class GuestCrashCenter: NSObject {
    private static let queue = DispatchQueue(label: "dev.linuxrunner.guest-crashes", qos: .utility)
    private static var store: GuestCrashStore?
    private static var pending: [Int: GuestCrashReport] = [:]
    private static var memoryOnly: [GuestCrashReport] = []
    private static var recent: [String] = []
    private static var lastBanner = Date.distantPast
    @objc static func configure(_ directory: URL) {
        queue.sync { store = try? GuestCrashStore(directory: directory) }
    }
    @objc static func begin(_ metadata: [String: String]) {
        queue.async {
            guard let tid = Int(metadata["tid"] ?? "") else { return }
            if pending.count >= 32 { pending.removeAll() }
            // Fatal delivery can report the same PC again. Keep the original
            // fault address and instruction rather than replacing them with 0.
            if var previous = pending[tid], previous.metadata["pc"] == metadata["pc"], previous.metadata["signal"] == metadata["signal"] {
                for (key, value) in metadata where key != "address" && key != "instruction" { previous.metadata[key] = DiagnosticPrivacy.redact(value) }
                pending[tid] = previous; return
            }
            pending[tid] = GuestCrashReport(metadata: metadata.mapValues { String(DiagnosticPrivacy.redact($0).prefix(512)) }, context: recent.filter { $0.contains("tid=\(tid) ") || $0.contains("tgid=\(metadata["pid"] ?? "?") ") })
        }
    }
    @objc static func append(_ line: String, tid: Int) {
        // Each line is bounded before queuing. Only structured records and
        // already-filtered diagnostic output enter the persistent report.
        let bounded = String(line.prefix(640))
        queue.async {
            if bounded.hasPrefix("FD_PRESSURE") || bounded.hasPrefix("FUTEX_ERROR:") || bounded.hasPrefix("POLL_BACKEND_FAILURE:") || bounded.hasPrefix("PROCESS_EVENT ") || bounded.hasPrefix("FD_TREND ") {
                recent.append(DiagnosticPrivacy.redact(bounded)); recent = Array(recent.suffix(8))
            }
            guard let safe = GuestCrashReport.safeContext(bounded), var report = pending[tid], report.context.count < 160 else { return }
            // The second fatal snapshot repeats the initial register/map data.
            if !report.context.contains(safe) { report.context.append(safe) }
            pending[tid] = report
        }
    }
    @objc static func finish(_ tid: Int, signal: Int, exitCode: Int) {
        queue.async {
            guard var report = pending.removeValue(forKey: tid) else { return }
            report.metadata["signal"] = String(signal)
            if signal == 0 { report.metadata["exit_code"] = String(exitCode) }
            // A nonzero exit status alone is ordinary Unix behaviour (grep found
            // nothing, a readiness probe polled "not yet", test failed). Keep it
            // only with evidence of a failure; crashes (signals) are always kept.
            // Recording every such exit evicted real crash reports and showed
            // "desktop-probe exited with status 1" banners during desktop start.
            if signal == 0 && report.probableCause.hasPrefix("Unknown") { return }
            if let store = store { store.append(report) } else { memoryOnly.insert(report,at:0); memoryOnly=Array(memoryOnly.prefix(20)) }
            DispatchQueue.main.async {
                NotificationCenter.default.post(name: Notification.Name("LRGuestCrashRecorded"), object: nil)
                showBanner(report)
            }
        }
    }
    @objc static func latestSummary() -> String { queue.sync { (store?.entries() ?? memoryOnly).first?.summary ?? "No guest application failures recorded" } }
    static func reports() -> [GuestCrashReport] { queue.sync { store?.entries() ?? memoryOnly } }
    static func storageStatus() -> String { queue.sync { guard let store = store else { return "Memory only: persistent crash storage could not be initialized." }; return store.persistenceError ?? "Latest 20 reports retained privately across restarts." } }
    static func export(_ report: GuestCrashReport) throws -> URL {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("LinuxRunnerCrashExports", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let url = directory.appendingPathComponent("LinuxRunner-crash-\(report.id.uuidString).txt")
        guard (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true else { throw EnvironmentError.invalidMetadata }
        try Data(report.fullText.utf8).write(to: url, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
        let allowed = Set(reports().map { "LinuxRunner-crash-\($0.id.uuidString).txt" })
        for file in (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? [] where file.lastPathComponent.hasPrefix("LinuxRunner-crash-") && !allowed.contains(file.lastPathComponent) { try? FileManager.default.removeItem(at: file) }
        return url
    }
    private static func showBanner(_ report: GuestCrashReport) {
        guard Date().timeIntervalSince(lastBanner) > 3,
              let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first(where: { $0.activationState == .foregroundActive }),
              let window = scene.windows.first(where: { $0.isKeyWindow }) else { return }
        lastBanner = Date()
        let button = UIButton(type: .system)
        button.setTitle(report.summary + "  ·  Details", for: .normal)
        button.titleLabel?.numberOfLines = 3; button.titleLabel?.font = .preferredFont(forTextStyle: .footnote)
        button.backgroundColor = .secondarySystemBackground; button.layer.cornerRadius = 12
        button.contentEdgeInsets = UIEdgeInsets(top: 12, left: 14, bottom: 12, right: 14)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.accessibilityIdentifier = "Guest Application Crash"
        button.addAction(UIAction { _ in
            button.removeFromSuperview()
            guard var presenter = window.rootViewController else { return }
            while let presented = presenter.presentedViewController { presenter = presented }
            let controller = UIHostingController(rootView: NavigationView { GuestCrashDetailView(report: report, dismissible: true) })
            presenter.present(controller, animated: true)
        }, for: .touchUpInside)
        window.addSubview(button)
        NSLayoutConstraint.activate([button.topAnchor.constraint(equalTo: window.safeAreaLayoutGuide.topAnchor, constant: 8), button.leadingAnchor.constraint(equalTo: window.safeAreaLayoutGuide.leadingAnchor, constant: 12), button.trailingAnchor.constraint(equalTo: window.safeAreaLayoutGuide.trailingAnchor, constant: -12)])
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak button] in button?.removeFromSuperview() }
    }
}
@objc(LRDiagnosticCenter) final class DiagnosticCenter: NSObject {
    private static let queue = DispatchQueue(label: "dev.linuxrunner.diagnostics", qos: .utility)
    private static var store: StructuredLogStore?
    private static var installerStore: StructuredLogStore?
    private static var threshold = DiagnosticLevel.info
    private static var repeated: [String: (Date, Int)] = [:]
    private static let overflowLock = NSLock()
    private static var dropped = 0
    private static let slots = DispatchSemaphore(value: 256)
    @objc static func configure(_ directory: URL) {
        queue.sync {
            store = try? StructuredLogStore(directory: directory, retainedFiles: UserDefaults.standard.object(forKey: "DiagnosticRetention") == nil ? 4 : UserDefaults.standard.integer(forKey: "DiagnosticRetention").clamped(to: 1...16))
            installerStore = try? StructuredLogStore(directory: directory.appendingPathComponent("debian-installer"), retainedFiles: 2)
            threshold = DiagnosticLevel(rawValue: UserDefaults.standard.integer(forKey: "DiagnosticLevel")) ?? .info
            if UserDefaults.standard.object(forKey: "DiagnosticLevel") == nil { threshold = .info }
        }
    }
    // Independent bounded log persists installation diagnostics even if the
    // live ring is busy with the already-running Alpine guest.
    @objc static func installer(_ severity: Int, message: String) {
        record("DEBIAN_INSTALL", severity: severity, message: message, pid: 0, tid: 0)
        let event = DiagnosticEvent(level: DiagnosticLevel(rawValue: severity) ?? .info, category: "DEBIAN_INSTALL", message: message)
        if severity == 0 { queue.sync { try? installerStore?.append(event) }; return }
        guard slots.wait(timeout: .now()) == .success else { return }
        queue.async { defer { slots.signal() }; if event.level.rawValue <= threshold.rawValue { try? installerStore?.append(event) } }
    }
    @objc static func setLevel(_ value: Int) { queue.sync { threshold = DiagnosticLevel(rawValue: value) ?? .info }; UserDefaults.standard.set(value, forKey: "DiagnosticLevel") }
    @objc static func setRetention(_ count: Int) { queue.sync { store?.setRetention(count) }; UserDefaults.standard.set(count.clamped(to: 1...16), forKey: "DiagnosticRetention") }
    @objc static func record(_ category: String, severity: Int, message: String, pid: Int, tid: Int) {
        guard slots.wait(timeout: .now()) == .success else { overflowLock.lock(); dropped += 1; overflowLock.unlock(); return }
        let event = DiagnosticEvent(level: DiagnosticLevel(rawValue: severity) ?? .info, category: category, message: DiagnosticPrivacy.redact(message), pid: pid > 0 ? pid : nil, tid: tid > 0 ? tid : nil)
        queue.async {
            defer { slots.signal() }
            overflowLock.lock(); let lost = dropped; dropped = 0; overflowLock.unlock()
            if lost > 0 { try? store?.append(DiagnosticEvent(level: .warning, category: "DIAGNOSTICS", message: "pending_queue_overflow dropped_records=\(lost)")) }
            guard event.level.rawValue <= threshold.rawValue else { return }
            let stableMessage = event.message.components(separatedBy: " duration_ns=").first ?? event.message
            let key = "\(event.category):\(event.pid ?? 0):\(stableMessage)"
            if threshold != .trace {
                if let previous = repeated[key], Date().timeIntervalSince(previous.0) < 5 { repeated[key] = (previous.0, previous.1 + 1); return }
                if let previous = repeated[key], previous.1 > 0 { try? store?.append(DiagnosticEvent(level: event.level, category: event.category, message: "repeated_records=\(previous.1) original=\(stableMessage)", pid: event.pid, tid: event.tid)) }
                if repeated.count > 256 { repeated.removeAll() }; repeated[key] = (Date(), 0)
            }
            try? store?.append(event)
        }
    }
    @objc static func entries() -> [[String: Any]] {
        queue.sync { (store?.read() ?? []).map { ["timestamp": $0.timestamp.timeIntervalSince1970, "level": $0.level.label, "category": $0.category, "message": $0.message, "pid": $0.pid ?? 0, "tid": $0.tid ?? 0] } }
    }
    @objc static func crashOutput(_ text: String) -> String { DiagnosticPrivacy.crashOutput(text) }
    @objc static func redact(_ text: String, application: Bool) -> String { application ? DiagnosticPrivacy.applicationExport(text) : (text.contains("-----BEGIN") ? "[key material omitted]" : text.components(separatedBy: "\n").map(DiagnosticPrivacy.redact).joined(separator: "\n")) }
    @objc static func flush() { queue.sync {} }
}
private extension Int { func clamped(to range: ClosedRange<Int>) -> Int { Swift.min(range.upperBound, Swift.max(range.lowerBound, self)) } }
#endif
