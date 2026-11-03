import Foundation
import SwiftUI

// Blocking native import runs off the main actor. Cancellation and progress
// throttling are shared with that callback and protected by this lock.
private final class SteamOSInstallControl: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    private var lastReport = 0.0
    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
    func isCancelled() -> Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
    func reportProgress() -> Bool {
        lock.lock(); defer { lock.unlock() }
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastReport >= 0.2 else { return false }
        lastReport = now
        return true
    }
}

@MainActor final class SteamOSInstaller: ObservableObject {
    @Published var running = false {
        didSet { if running != oldValue { running ? LRRuntimeBridge.beginAwake("steamos-install") : LRRuntimeBridge.endAwake("steamos-install") } }
    }
    @Published var message = "Install the prepared SteamFrame ARM64 userspace."
    @Published var fraction = 0.0
    @Published var failure: String?
    private var operation: Task<Void, Never>?
    private var control = SteamOSInstallControl()
    private var operationID: UUID?

    static var preparedArchiveURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SteamOS/rootfs.tar.gz")
    }

    func cancel() { control.cancel(); operation?.cancel() }

    func install(source: URL, completion: @escaping (LinuxEnvironment) -> Void) {
        guard !running else { return }
        running = true; failure = nil; fraction = 0
        control = SteamOSInstallControl()
        let token = control, identifier = UUID()
        operationID = identifier
        UserDefaults.standard.set(true, forKey: "InstallerActive")
        operation = Task {
            defer { operationID = nil; running = false; UserDefaults.standard.set(false, forKey: "InstallerActive") }
            let scoped = source.startAccessingSecurityScopedResource()
            defer { if scoped { source.stopAccessingSecurityScopedResource() } }
            do {
                let manifest = try SteamOSManifest.load()
                let attributes = try source.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
                guard attributes.isRegularFile == true, attributes.isSymbolicLink != true,
                      Int64(attributes.fileSize ?? 0) == manifest.archiveBytes else {
                    throw SteamOSInstallError.failure("Choose the prepared SteamOS rootfs.tar.gz that matches this app's image manifest.")
                }
                let available = try LRRuntimeBridge.managedStorageURL().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage ?? 0
                guard available >= manifest.requiredFreeBytes else {
                    throw SteamOSInstallError.failure("SteamOS installation needs " + ByteCountFormatter.string(fromByteCount: manifest.requiredFreeBytes, countStyle: .file) + " of available space for verification and installation.")
                }
                let root = UUID().uuidString.lowercased()
                let metadata = ["distributionID": "steamos", "distribution": "SteamOS ARM64", "version": manifest.version,
                                "packageManager": "pacman", "defaultShell": "/bin/bash", "sourceImageSHA256": manifest.sourceImageSHA256]
                message = "Verifying the prepared SteamOS filesystem"
                try await Task.detached(priority: .utility) {
                    try LRRuntimeBridge.importVerifiedEnvironment(source, root: root, sha256: manifest.archiveSHA256, metadata: metadata,
                        progress: { value, _ in
                            guard token.reportProgress() else { return }
                            Task { @MainActor in
                                guard self.operationID == identifier, self.running else { return }
                                self.fraction = value
                                self.message = value < 0.2 ? "Verifying the prepared SteamOS filesystem" : "Installing SteamOS"
                            }
                        }, cancel: { token.isCancelled() })
                }.value
                var environment = LinuxEnvironment(name: "SteamOS ARM64 · Experimental", rootfsName: root)
                environment.distribution = "SteamOS ARM64"; environment.distributionID = "steamos"
                environment.packageManager = "pacman"; environment.rootfsVersion = manifest.version
                environment.defaultShell = "/bin/bash"; environment.installationState = "Prepared image imported"
                environment.desktopAvailability = "Plasma X11 prepared; session checked at startup"
                UserDefaults.standard.set("plasma", forKey: "LinuxRunnerDesktop.environment." + root)
                completion(environment)
                fraction = 1; message = "SteamOS installed. Restart LinuxRunner to configure your account."
                LRRuntimeBridge.logCategory("INSTALL", event: "Prepared SteamOS ARM64 image imported; SHA-256 verified")
            } catch {
                failure = token.isCancelled() ? "Installation cancelled. Your selected archive and existing environments were kept." : error.localizedDescription
                LRRuntimeBridge.logCategory("INSTALL", event: failure ?? "SteamOS installation failed")
            }
        }
    }
}
