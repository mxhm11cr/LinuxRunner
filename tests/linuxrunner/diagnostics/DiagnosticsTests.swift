import Foundation
@main struct Tests {
 static func main() throws {
    let qpa = "qt.qpa.xcb: could not connect to display :4004"
    precondition(DiagnosticPrivacy.crashOutput(qpa) == qpa)
    precondition(DiagnosticPrivacy.applicationExport(qpa) == qpa)
    let platformFatal = "This application failed to start because no Qt platform plugin could be initialized."
    precondition(DiagnosticPrivacy.crashOutput(platformFatal) == platformFatal)
    precondition(DiagnosticPrivacy.applicationExport(platformFatal) == platformFatal)
    precondition(!DiagnosticPrivacy.crashOutput("qt.qpa.plugin: token=private-value https://example.com/private").contains("private"))
    precondition(DiagnosticPrivacy.crashOutput(String(repeating: "x", count: 900) + "fatal").count <= 640)
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: root) }
    let store = try StructuredLogStore(directory: root, maximumBytes: 4096, retainedFiles: 2)
    for n in 0..<150 { try store.append(DiagnosticEvent(level: .info, category: "TEST", message: "record=\(n) token=abc password='hello there' https://example.com/path?secret=yes")) }
    let events = store.read(); precondition(!events.isEmpty)
    precondition(events.last!.message.contains("record=149"))
    for e in events { precondition(!e.message.contains("abc")); precondition(!e.message.contains("hello there")); precondition(!e.message.contains("secret=yes")) }
    let files = try FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)
    precondition(files.count <= 3); precondition(StorageUsage.bytes(root) <= 96 * 1024)
    precondition(DiagnosticPrivacy.applicationExport("BROWSER: JavaScript result={cookie:'secret'}\nprivate app output").contains("omitted"))
    precondition(!DiagnosticPrivacy.redact("BROWSER: URL=https://example.com/private-history").contains("private-history"))
    let desktop = DiagnosticPrivacy.applicationExport("DESKTOP_STARTUP: wm=PASS\nQPainter::begin: Paint device returned engine == 0\nQWidget::render: token=private-session\nprivate conversation text")
    precondition(desktop.contains("wm=PASS") && desktop.contains("Paint device returned engine"))
    precondition(!desktop.contains("private-session") && !desktop.contains("private conversation text"))
    store.setRetention(1)
    let reducedFiles = try FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil); precondition(reducedFiles.count <= 2)
    precondition(!DiagnosticPrivacy.redact("Bearer sensitive-token").contains("sensitive-token"))
    let other = root.appendingPathComponent("outside")
    try FileManager.default.createDirectory(at: other, withIntermediateDirectories: true)
    try Data(repeating: 1, count: 8192).write(to: other.appendingPathComponent("data"))
    let before = StorageUsage.bytes(root)
    try FileManager.default.createSymbolicLink(at: root.appendingPathComponent("alias"), withDestinationURL: other)
    let after = StorageUsage.bytes(root); print("storage before=\(before) after=\(after)"); precondition(after == before)
    precondition(StorageUsage.measuredBytes(root, isCancelled: { true }) == nil)
    var visited = 0
    let interrupted = StorageUsage.measuredBytes(root, isCancelled: { visited += 1; return visited >= 5 })
    precondition(interrupted == nil && visited == 5)
    precondition(StorageUsage.measuredBytes(root, isCancelled: { false }) == after)
    let crashes = root.appendingPathComponent("crashes")
    let crashStore = try GuestCrashStore(directory: crashes)
    for n in 0..<25 {
        crashStore.append(GuestCrashReport(metadata: ["application":"firefox", "signal":"6", "pid":"\(n)"], context: ["FAULT_CALL tid=1 nr=73 result=-24"]))
    }
    precondition(crashStore.entries().count == 20)
    let restored = try GuestCrashStore(directory: crashes)
    precondition(restored.entries().count == 20 && restored.entries().first?.metadata["pid"] == "24")
    precondition(restored.entries().first!.summary.contains("cause suspected"))
    precondition(!GuestCrashReport(metadata: ["signal":"5"], context: []).summary.contains("unsupported instruction"))
    precondition(GuestCrashReport(metadata: ["signal":"0", "exit_code":"7"], context: []).summary.contains("status 7"))
    for file in try FileManager.default.contentsOfDirectory(at: crashes, includingPropertiesForKeys: nil) {
        let mode = (try FileManager.default.attributesOfItem(atPath: file.path)[.posixPermissions] as! NSNumber).intValue
        precondition(mode == 0o600)
    }
    precondition(GuestCrashReport.safeContext("private page text") == nil)
    precondition(!GuestCrashReport.safeContext("FAULT_MAP path=https://example.com/private?token=abc")!.contains("private?"))
    let crashLink = root.appendingPathComponent("crash-link")
    try FileManager.default.createSymbolicLink(at: crashLink, withDestinationURL: crashes)
    do { _ = try GuestCrashStore(directory: crashLink); preconditionFailure("accepted crash-store symlink") } catch {}
    precondition(!DiagnosticPrivacy.crashOutput("FATAL password=private token=secret https://example.com/personal").contains("private"))
    precondition(!DiagnosticPrivacy.crashOutput("ordinary webpage body").contains("webpage"))
    precondition(GuestCrashReport.safeContext("MMAP_FAILURE stage=guest_address_space guest_fd=45 linux_result=-12") != nil)
    precondition(GuestCrashReport.safeContext("PROCESS_ROLE type=utility utility_subtype=network.mojom.NetworkService") != nil)
    let rootFailure=GuestCrashReport(metadata:["signal":"5"],context:["FAULT_STDERR FATAL Running as root without --no-sandbox is not supported"])
    precondition(rootFailure.probableCause.hasPrefix("High confidence:"))
    let unknownExit=GuestCrashReport(metadata:["signal":"0", "exit_code":"191"],context:["FAULT_CALL result=-4 name=futex errno=EINTR", "FAULT_CODE instruction=0xd65f03c0"])
    precondition(unknownExit.probableCause.hasPrefix("Unknown:") && unknownExit.fullText.contains("RET instruction"))
    let unrelatedThreadExit=GuestCrashReport(metadata:["signal":"5"],context:["PROCESS_EVENT kind=termination_request name=exit status_linux=0 origin=runtime_call"])
    precondition(unrelatedThreadExit.probableCause.hasPrefix("Unknown:"))
    let runtimeExit=GuestCrashReport(metadata:["signal":"0", "exit_code":"191"],context:["PROCESS_EVENT kind=termination_request name=exit status_linux=191 origin=runtime_call pc=0"])
    precondition(runtimeExit.probableCause.hasPrefix("High confidence:"))
    print("PASS: fatal output filtering, probable-cause confidence and no RET/EINTR causal inference")
    print("PASS: crash retention/reload, permissions, exit classification, suspected errno, payload and symlink rejection")
    print("PASS: bounded rotation, newest-record retention, credential/URL/payload redaction, symlink-safe storage accounting")
 }
}
