import Foundation

// Kali Linux ARM64: the official Kali NetHunter minimal root filesystem
// (Kali GNU/Linux Rolling userspace, arm64), pinned by release, size and
// SHA-256 from Kali's published SHA256SUMS. Kali does not sign that file, so
// integrity rests on HTTPS plus the pinned hash; everything installed later is
// verified by apt against Kali's own archive keyring inside the environment.
// The archive keeps its files under one top-level directory, imported as /.
enum KaliImage {
    static let version = "2026.3"
    static let filename = "kali-nethunter-rootfs-minimal-arm64.tar.xz"
    static let url = URL(string: "https://kali.download/nethunter-images/kali-2026.3/rootfs/" + filename)!
    static let bytes: Int64 = 125_846_088
    static let sha256 = "96aeced81a857a987715a14417af05c2934e3e19052d6151c398a1c215448b60"
    static let topLevel = "kali-arm64"

    static func verify(_ file: URL) throws {
        var file = file
        file.removeAllCachedResourceValues()
        let info = try file.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard info.isRegularFile == true, info.isSymbolicLink != true,
              Int64(info.fileSize ?? -1) == bytes,
              try DebianInstaller.sha256(file) == sha256 else {
            throw KaliInstallError.failure("Kali image verification failed. No filesystem was imported. Retry to download the image again.")
        }
    }
}

enum KaliInstallError: LocalizedError {
    case failure(String)
    var errorDescription: String? { switch self { case .failure(let text): return text } }
}

private final class KaliCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var stopped = false
    func cancel() { lock.lock(); stopped = true; lock.unlock() }
    func check() -> Bool { lock.lock(); defer { lock.unlock() }; return stopped }
}

@MainActor final class KaliInstaller: ObservableObject {
    @Published var message = "Kali Linux \(KaliImage.version) ARM64 · official image · experimental"
    @Published var fraction = 0.0 { didSet { if let longTask { LongTaskCenter.shared.update(longTask, detail: message, fraction: fraction) } } }
    private var longTask: String?
    private func trackLongTask(_ title: String) {
        if running { longTask = LongTaskCenter.shared.begin(title: title, detail: message, userStarted: true) }
        else if let token = longTask { longTask = nil; LongTaskCenter.shared.end(token, success: failure == nil, message: failure ?? message) }
    }
    @Published var running = false {
        didSet { if running != oldValue { running ? LRRuntimeBridge.beginAwake("kali-install") : LRRuntimeBridge.endAwake("kali-install"); trackLongTask("Installing Kali Linux") } }
    }
    @Published var failure: String?
    private static var installing = false
    private var operation: Task<Void, Never>?
    private var cancellation = KaliCancellation()
    func cancel() { cancellation.cancel(); operation?.cancel() }

    func install(completion: @escaping (LinuxEnvironment) -> Void) {
        guard !running else { return }
        guard !Self.installing else { failure = "Kali Setup is already running."; return }
        Self.installing = true
        running = true; failure = nil; fraction = 0; cancellation = KaliCancellation()
        let token = cancellation
        UserDefaults.standard.set(true, forKey: "InstallerActive")
        operation = Task {
            defer { Self.installing = false; running = false; UserDefaults.standard.set(false, forKey: "InstallerActive") }
            do {
                let fm = FileManager.default
                let cache = fm.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("KaliInstaller-v1", isDirectory: true)
                try fm.createDirectory(at: cache, withIntermediateDirectories: true)
                let image = cache.appendingPathComponent(KaliImage.filename)
                let storage = try LRRuntimeBridge.managedStorageURL().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeSupportsCaseSensitiveNamesKey])
                let available = storage.volumeAvailableCapacityForImportantUsage ?? 0
                guard available >= 2_000_000_000 else { throw KaliInstallError.failure("Kali needs at least 2 GB free for download and import. Its desktop needs about 2 GB more, and Kali's standard tools about 6 GB.") }
                if storage.volumeSupportsCaseSensitiveNames == false {
                    throw KaliInstallError.failure("This storage volume cannot preserve separate Linux filenames that differ only in case. Kali was not installed; existing environments are preserved.")
                }
                message = "Checking cached Kali image"
                let valid = await Task.detached(priority: .utility) { (try? KaliImage.verify(image)) != nil }.value
                if !valid {
                    message = "Downloading Kali Linux \(KaliImage.version) ARM64"
                    let partial = cache.appendingPathComponent(KaliImage.filename + ".partial")
                    try await FedoraImageDownload.fetch(from: KaliImage.url, bytes: KaliImage.bytes, to: partial) { offset, retrying in
                        Task { @MainActor in
                            guard self.running, !token.check(), self.fraction < 0.7 else { return }
                            self.fraction = max(self.fraction, Double(offset) / Double(KaliImage.bytes) * 0.7)
                            self.message = retrying ? "Reconnecting to Kali. Downloaded segments are saved." : "Downloading Kali Linux \(KaliImage.version) ARM64"
                        }
                    }
                    fraction = 0.7
                    message = "Verifying Kali's image"
                    do {
                        try await Task.detached(priority: .utility) { try KaliImage.verify(partial) }.value
                    } catch {
                        try? fm.removeItem(at: partial)
                        throw error
                    }
                    try Task.checkCancellation()
                    if fm.fileExists(atPath: image.path) { try fm.removeItem(at: image) }
                    try fm.moveItem(at: partial, to: image)
                }
                if token.check() { throw CancellationError() }
                let root = "kali-" + UUID().uuidString.lowercased()
                let metadata = ["distributionID": "kali", "distribution": "Kali Linux", "version": KaliImage.version,
                                "packageManager": "apt", "defaultShell": "/bin/bash", "image": KaliImage.filename,
                                "imageSHA256": KaliImage.sha256, "source": KaliImage.url.absoluteString]
                message = "Extracting isolated Kali filesystem"; fraction = 0.7
                try await Task.detached(priority: .utility) {
                    try LRRuntimeBridge.importVerifiedEnvironment(image, root: root, sha256: KaliImage.sha256, topLevel: KaliImage.topLevel, metadata: metadata,
                        progress: { value, text in Task { @MainActor in
                            guard self.running, !token.check(), self.fraction < 1 else { return }
                            self.fraction = max(self.fraction, 0.7 + value * 0.3); self.message = text
                        } },
                        cancel: { token.check() })
                }.value
                var environment = LinuxEnvironment(name: "Kali Linux \(KaliImage.version) ARM64", rootfsName: root)
                environment.distribution = "Kali Linux"; environment.distributionID = "kali"
                environment.rootfsVersion = KaliImage.version; environment.packageManager = "apt"; environment.defaultShell = "/bin/bash"
                environment.installationState = "Imported; first-start validation pending"
                environment.guiAvailability = "Not installed"; environment.desktopAvailability = "Not installed"
                completion(environment)
                fraction = 1; message = "Kali imported. Relaunch to validate the base filesystem and create your account."
                LRRuntimeBridge.logCategory("INSTALL", event: "Kali image import completed")
            } catch {
                failure = error is CancellationError || token.check() ? "Installation cancelled. Saved download segments are retained for retry; existing environments are preserved." : error.localizedDescription
                LRRuntimeBridge.logCategory("INSTALL", event: failure ?? "Kali installation failed")
            }
        }
    }
}
