import Foundation
import SwiftUI
import UIKit
import UserNotifications
#if canImport(BackgroundTasks)
import BackgroundTasks
#endif

/// User-started long work (package installs, desktop components, Kali tools,
/// App Store changes, an opted-in terminal) and what iOS genuinely allows it
/// while LinuxRunner is not in front:
///
/// * Always: a UIApplication background-task assertion, a short grace period
///   (typically about 30 seconds) after the app leaves the foreground.
/// * iOS 26 and later: a BGContinuedProcessingTask. iOS may keep the app
///   running and shows the task in its own Live Activity with this progress.
///   The system decides; it can expire the task at any time (load, thermal
///   state, user cancellation, or progress that looks stalled).
///
/// Nothing here plays silent audio, uses location or any other background
/// mode to avoid suspension. When iOS suspends the app, Linux processes are
/// frozen with it and continue when it returns, unless iOS terminates it;
/// interrupted package operations are then finished by the existing
/// interrupted-installation recovery on the next run.
@objc(LRLongTaskCenter) @MainActor final class LongTaskCenter: NSObject, ObservableObject {
    @objc static let shared = LongTaskCenter()

    enum State: String { case running = "Running", backgroundGranted = "Running in background", pausedByIOS = "Paused by iOS", completed = "Completed", failed = "Failed" }
    struct Entry: Identifiable {
        let id: String
        var title: String
        var detail: String
        var fraction: Double?
        var state: State
        let started: Date
        var ended: Date?
    }
    @Published private(set) var entries: [Entry] = []

    private var assertions: [String: UIBackgroundTaskIdentifier] = [:]
    private var system: [String: AnyObject] = [:]          // BGContinuedProcessingTask, iOS 26+
    private var installTokens: [String: String] = [:]     // support-install title -> token
    private var observer: NSObjectProtocol?

    /// The system shows its own UI for continued tasks; this switch only
    /// controls LinuxRunner's completion notifications.
    @objc static var notificationsEnabled: Bool {
        get { UserDefaults.standard.object(forKey: "LinuxRunner.notifyTaskCompletion") as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: "LinuxRunner.notifyTaskCompletion") }
    }

    /// Follow the support installer's real progress notifications.
    @objc func start() {
        guard observer == nil else { return }
        observer = NotificationCenter.default.addObserver(forName: Notification.Name("LRInstallProgressNotification"), object: nil, queue: .main) { [weak self] note in
            guard let info = note.userInfo, let title = info["title"] as? String, let phase = info["phase"] as? String else { return }
            Task { @MainActor in
                guard let self else { return }
                let fraction = (info["fraction"] as? Double).flatMap { $0 >= 0 ? $0 : nil }
                let stage = info["stage"] as? String ?? ""
                switch phase {
                case "running":
                    if let token = self.installTokens[title] { self.update(token, detail: stage, fraction: fraction) }
                    else { self.installTokens[title] = self.begin(title: title, detail: stage) }
                case "succeeded", "failed":
                    if let token = self.installTokens.removeValue(forKey: title) {
                        self.end(token, success: phase == "succeeded", message: phase == "succeeded" ? "\(title) finished." : (info["error"] as? String ?? "\(title) failed."))
                    }
                default: break
                }
            }
        }
    }

    /// `userStarted`: the user explicitly started this task (install button);
    /// only then may iOS's notification permission prompt appear, never in
    /// the middle of an implicit step such as desktop component setup.
    @objc @discardableResult func begin(title: String, detail: String) -> String { begin(title: title, detail: detail, userStarted: false) }
    @discardableResult func begin(title: String, detail: String, userStarted: Bool) -> String {
        let token = UUID().uuidString.lowercased()
        entries.insert(Entry(id: token, title: title, detail: detail, fraction: nil, state: .running, started: Date()), at: 0)
        if entries.count > 20 { entries.removeLast(entries.count - 20) }
        let assertion = UIApplication.shared.beginBackgroundTask(withName: "LinuxRunner: " + title) { [weak self] in
            Task { @MainActor in self?.assertionExpired(token) }
        }
        if assertion != .invalid { assertions[token] = assertion }
        requestContinuedProcessing(token, title: title, detail: detail)
        if userStarted { requestNotificationPermissionOnce() }
        LRRuntimeBridge.logCategory("BACKGROUND", event: "task begin title=\(title) assertion=\(assertion != .invalid)")
        return token
    }

    @objc(updateToken:detail:fraction:) func update(_ token: String, detail: String, fraction: NSNumber?) { update(token, detail: detail, fraction: fraction?.doubleValue) }
    func update(_ token: String, detail: String, fraction: Double?) {
        guard let index = entries.firstIndex(where: { $0.id == token }) else { return }
        if !detail.isEmpty { entries[index].detail = detail }
        if let fraction { entries[index].fraction = min(1, max(0, fraction)) }
        #if canImport(BackgroundTasks)
        if #available(iOS 26.0, *), let task = system[token] as? BGContinuedProcessingTask {
            if let fraction { task.progress.totalUnitCount = 1000; task.progress.completedUnitCount = Int64(min(1, max(0, fraction)) * 1000) }
            task.updateTitle(entries[index].title, subtitle: entries[index].detail)
        }
        #endif
    }

    @objc func end(_ token: String, success: Bool, message: String) {
        guard let index = entries.firstIndex(where: { $0.id == token }) else { return }
        entries[index].state = success ? .completed : .failed
        entries[index].ended = Date()
        entries[index].detail = message
        #if canImport(BackgroundTasks)
        if #available(iOS 26.0, *), let task = system.removeValue(forKey: token) as? BGContinuedProcessingTask {
            task.progress.totalUnitCount = 1000; task.progress.completedUnitCount = 1000
            task.setTaskCompleted(success: success)
        }
        #endif
        if let assertion = assertions.removeValue(forKey: token) { UIApplication.shared.endBackgroundTask(assertion) }
        if UIApplication.shared.applicationState != .active { notify(title: entries[index].title, body: message, success: success) }
        LRRuntimeBridge.logCategory("BACKGROUND", event: "task end title=\(entries[index].title) success=\(success)")
    }

    var running: [Entry] { entries.filter { $0.ended == nil } }

    // MARK: - System mechanisms

    private func assertionExpired(_ token: String) {
        // The grace period is over. Without a continued-processing grant, iOS
        // suspends the app now; Linux resumes when LinuxRunner returns.
        if let assertion = assertions.removeValue(forKey: token) { UIApplication.shared.endBackgroundTask(assertion) }
        if let index = entries.firstIndex(where: { $0.id == token }), entries[index].ended == nil, system[token] == nil {
            entries[index].state = .pausedByIOS
            LRRuntimeBridge.logCategory("BACKGROUND", event: "background grace ended; work pauses until LinuxRunner is reopened title=\(entries[index].title)")
        }
    }

    private func requestContinuedProcessing(_ token: String, title: String, detail: String) {
        #if canImport(BackgroundTasks)
        guard #available(iOS 26.0, *), let bundle = Bundle.main.bundleIdentifier else { return }
        let identifier = bundle + ".linuxtask." + token
        // Continued-processing registrations may happen after launch; each
        // identifier is registered exactly once (the token is unique).
        let registered = BGTaskScheduler.shared.register(forTaskWithIdentifier: identifier, using: .main) { [weak self] task in
            guard let task = task as? BGContinuedProcessingTask else { task.setTaskCompleted(success: false); return }
            Task { @MainActor in
                guard let self, let index = self.entries.firstIndex(where: { $0.id == token }), self.entries[index].ended == nil else { task.setTaskCompleted(success: true); return }
                self.system[token] = task
                self.entries[index].state = .backgroundGranted
                task.progress.totalUnitCount = 1000
                task.progress.completedUnitCount = Int64((self.entries[index].fraction ?? 0) * 1000)
                task.expirationHandler = { [weak self] in
                    Task { @MainActor in
                        guard let self else { return }
                        self.system.removeValue(forKey: token)
                        if let i = self.entries.firstIndex(where: { $0.id == token }), self.entries[i].ended == nil {
                            self.entries[i].state = .pausedByIOS
                            LRRuntimeBridge.logCategory("BACKGROUND", event: "iOS expired continued processing title=\(self.entries[i].title)")
                            if UIApplication.shared.applicationState != .active {
                                self.notify(title: self.entries[i].title, body: "iOS paused this task. Open LinuxRunner to continue.", success: false)
                            }
                        }
                    }
                }
            }
        }
        guard registered else { LRRuntimeBridge.logCategory("BACKGROUND", event: "continued processing unavailable: identifier not permitted"); return }
        let request = BGContinuedProcessingTaskRequest(identifier: identifier, title: title, subtitle: detail.isEmpty ? "Linux task in progress" : detail)
        request.strategy = .fail     // now or not at all; the assertion still applies
        do { try BGTaskScheduler.shared.submit(request) }
        catch { LRRuntimeBridge.logCategory("BACKGROUND", event: "continued processing not granted: \(error.localizedDescription)") }
        #endif
    }

    private func requestNotificationPermissionOnce() {
        guard Self.notificationsEnabled, !UserDefaults.standard.bool(forKey: "LinuxRunner.notificationPermissionAsked") else { return }
        UserDefaults.standard.set(true, forKey: "LinuxRunner.notificationPermissionAsked")
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    private func notify(title: String, body: String, success: Bool) {
        guard Self.notificationsEnabled else { return }
        let content = UNMutableNotificationContent()
        content.title = success ? title : title + " needs attention"
        content.body = body
        content.sound = .default
        UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }
}

// MARK: - Installation screensaver (OLED care)

/// Records the time of the user's last touch anywhere in the app without
/// taking part in gesture handling (it always fails and never cancels touches).
private final class ActivityRecognizer: UIGestureRecognizer {
    var touched: () -> Void = {}
    override init(target: Any?, action: Selector?) {
        super.init(target: target, action: action)
        cancelsTouchesInView = false; delaysTouchesBegan = false; delaysTouchesEnded = false
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) { touched(); state = .failed }
    override func canPrevent(_ preventedGestureRecognizer: UIGestureRecognizer) -> Bool { false }
    override func canBePrevented(by preventingGestureRecognizer: UIGestureRecognizer) -> Bool { false }
}

/// While a long operation (installation, package or desktop setup) runs and
/// the user has not touched the screen for the configured time, show a mostly
/// black screen with a small, slowly moving status. It never pauses or changes
/// the operation, does not keep the display awake, and leaves iOS's auto-lock
/// alone. It reduces bright static content; it cannot rule out burn-in.
@MainActor final class InstallScreensaver: NSObject {
    static let shared = InstallScreensaver()
    static let timeoutKey = "LinuxRunner.installScreensaverMinutes"
    /// Minutes of inactivity: 0 = off, else 3–5. Default 4.
    static var minutes: Int {
        // Off or 3–5 minutes (earlier builds offered 2 and 10; those map to the nearest).
        get { let v = UserDefaults.standard.object(forKey: timeoutKey) as? Int ?? 4; return v <= 0 ? 0 : min(5, max(3, v)) }
        set { UserDefaults.standard.set(newValue, forKey: timeoutKey) }
    }
    private var lastActivity = Date()
    private var timer: Timer?
    private var window: UIWindow?
    private var attached = Set<ObjectIdentifier>()

    func start() {
        guard timer == nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in Task { @MainActor in self?.tick() } }
        timer?.tolerance = 2
        NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.noteActivity() }
        }
    }
    func noteActivity() {
        lastActivity = Date()
        if window != nil { dismiss() }
    }
    private func attachRecognizers() {
        for scene in UIApplication.shared.connectedScenes {
            guard let scene = scene as? UIWindowScene else { continue }
            for w in scene.windows where w !== window && !attached.contains(ObjectIdentifier(w)) {
                let recognizer = ActivityRecognizer(target: nil, action: nil)
                recognizer.touched = { [weak self] in Task { @MainActor in self?.noteActivity() } }
                w.addGestureRecognizer(recognizer); attached.insert(ObjectIdentifier(w))
            }
        }
    }
    private func tick() {
        attachRecognizers()
        let running = LongTaskCenter.shared.running
        if window != nil {
            if running.isEmpty { dismiss() }
            return
        }
        guard Self.minutes > 0, !running.isEmpty, UIApplication.shared.applicationState == .active,
              Date().timeIntervalSince(lastActivity) >= Double(Self.minutes) * 60 else { return }
        present()
    }
    private func present() {
        guard let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive }) else { return }
        let w = UIWindow(windowScene: scene)
        w.windowLevel = .alert + 1
        let host = UIHostingController(rootView: InstallScreensaverView(center: LongTaskCenter.shared) { [weak self] in self?.noteActivity() })
        host.view.backgroundColor = .black
        w.rootViewController = host
        w.alpha = 0; w.isHidden = false
        UIView.animate(withDuration: 0.8) { w.alpha = 1 }
        window = w
        LRRuntimeBridge.logCategory("SCREENSAVER", event: "installation screensaver shown after \(Self.minutes) min without interaction")
    }
    private func dismiss() {
        guard let w = window else { return }
        window = nil
        UIView.animate(withDuration: 0.25, animations: { w.alpha = 0 }) { _ in w.isHidden = true }
    }
}

struct InstallScreensaverView: View {
    @ObservedObject var center: LongTaskCenter
    let wake: () -> Void
    @State private var position = CGPoint(x: 0.5, y: 0.45)
    private let move = Timer.publish(every: 20, tolerance: 5, on: .main, in: .common).autoconnect()
    var body: some View {
        GeometryReader { geometry in
            let task = center.running.first
            VStack(spacing: 6) {
                Text(task?.title ?? "Working").font(.footnote.weight(.semibold))
                if let fraction = task?.fraction { Text("\(Int(fraction * 100))%").font(.system(.title3, design: .rounded).monospacedDigit()) }
                if let detail = task?.detail, !detail.isEmpty { Text(detail).font(.caption2).lineLimit(1) }
                Text("Touch to return").font(.caption2).opacity(0.6)
            }
            .foregroundColor(Color(white: 0.42))
            .multilineTextAlignment(.center)
            .frame(maxWidth: min(320, geometry.size.width - 40))
            .position(x: position.x * geometry.size.width, y: position.y * geometry.size.height)
            .onReceive(move) { _ in
                withAnimation(.easeInOut(duration: 4)) {
                    position = CGPoint(x: .random(in: 0.25...0.75), y: .random(in: 0.2...0.8))
                }
            }
        }
        .background(Color.black.ignoresSafeArea())
        .contentShape(Rectangle())
        .onTapGesture { wake() }
        .accessibilityElement(children: .combine)
        .accessibilityHint("Double-tap to return to LinuxRunner")
        .statusBarHidden(true)
    }
}
