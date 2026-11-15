import Foundation

// Canonical Ubuntu Base ARM64. SHA-256 is pinned from Ubuntu's signed
// 24.04.5 SHA256SUMS (CD image signing key 843938DF228D22F7B3742BC0D94AA3F0EFE21092).
// APT retains the image's official signed Ubuntu repositories.
enum UbuntuImage {
    static let version = "24.04.5"
    static let filename = "ubuntu-base-24.04.5-base-arm64.tar.gz"
    static let url = URL(string: "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/" + filename)!
    static let bytes: Int64 = 29_936_675
    static let sha256 = "a91d5a93010193712d346d761372b7c9db6dfcf093893161c64ca107f05914f2"

    static func verify(_ file: URL) throws {
        var file = file
        file.removeAllCachedResourceValues()
        let info = try file.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard info.isRegularFile == true, info.isSymbolicLink != true,
              Int64(info.fileSize ?? -1) == bytes,
              try DebianInstaller.sha256(file) == sha256 else {
            throw UbuntuInstallError.failure("Ubuntu image verification failed. No filesystem was imported. Retry to download the image again.")
        }
    }
}

/// Ubuntu-based distributions installed from the same verified Ubuntu Base
/// image; their own signed repositories are layered at first desktop setup
/// (GUISupport.bundle/initialize-<id>.sh). distributionID stays "ubuntu" so
/// every Ubuntu code path applies; `flavor` records which one it is.
enum UbuntuFlavor: String, CaseIterable {
    case pop, zorin
    var name: String { switch self { case .pop: return "Pop!_OS"; case .zorin: return "Zorin OS" } }
    var version: String { switch self { case .pop: return "24.04"; case .zorin: return "18" } }
    var vendor: String { switch self { case .pop: return "System76's signed Pop!_OS repository"; case .zorin: return "Zorin OS's signed Launchpad repositories" } }
}

enum UbuntuInstallError: LocalizedError {
    case failure(String)
    var errorDescription: String? { switch self { case .failure(let text): return text } }
}

private final class UbuntuCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var stopped = false
    func cancel() { lock.lock(); stopped = true; lock.unlock() }
    func check() -> Bool { lock.lock(); defer { lock.unlock() }; return stopped }
}

@MainActor final class UbuntuInstaller: ObservableObject {
    @Published var message = "Ubuntu Linux \(UbuntuImage.version) ARM64 · official image · experimental"
    @Published var fraction = 0.0 { didSet { if let longTask { LongTaskCenter.shared.update(longTask, detail: message, fraction: fraction) } } }
    private var longTask: String?
    private func trackLongTask(_ title: String) {
        if running { longTask = LongTaskCenter.shared.begin(title: title, detail: message, userStarted: true) }
        else if let token = longTask { longTask = nil; LongTaskCenter.shared.end(token, success: failure == nil, message: failure ?? message) }
    }
    @Published var running = false {
        didSet { if running != oldValue { running ? LRRuntimeBridge.beginAwake("ubuntu-install") : LRRuntimeBridge.endAwake("ubuntu-install"); trackLongTask("Installing Ubuntu Linux") } }
    }
    @Published var failure: String?
    private static var installing = false
    private var operation: Task<Void, Never>?
    private var cancellation = UbuntuCancellation()
    func cancel() { cancellation.cancel(); operation?.cancel() }

    /// nil installs Ubuntu itself.
    var flavor: UbuntuFlavor?
    private var displayName: String { flavor.map { "\($0.name) \($0.version)" } ?? "Ubuntu Linux \(UbuntuImage.version)" }

    func install(completion: @escaping (LinuxEnvironment) -> Void) {
        guard !running else { return }
        guard !Self.installing else { failure = "Ubuntu Setup is already running."; return }
        Self.installing = true
        running = true; failure = nil; fraction = 0; cancellation = UbuntuCancellation()
        let token = cancellation
        UserDefaults.standard.set(true, forKey: "InstallerActive")
        operation = Task {
            defer { Self.installing = false; running = false; UserDefaults.standard.set(false, forKey: "InstallerActive") }
            do {
                let fm = FileManager.default
                let cache = fm.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("UbuntuInstaller-v1", isDirectory: true)
                try fm.createDirectory(at: cache, withIntermediateDirectories: true)
                let image = cache.appendingPathComponent(UbuntuImage.filename)
                let storage = try LRRuntimeBridge.managedStorageURL().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeSupportsCaseSensitiveNamesKey])
                let available = storage.volumeAvailableCapacityForImportantUsage ?? 0
                guard available >= 2_000_000_000 else { throw UbuntuInstallError.failure("Ubuntu needs at least 2 GB free for download and import. Desktop packages need additional space.") }
                if storage.volumeSupportsCaseSensitiveNames == false {
                    throw UbuntuInstallError.failure("This storage volume cannot preserve separate Linux filenames that differ only in case. Ubuntu was not installed; existing environments are preserved.")
                }
                message = "Checking cached Ubuntu image"
                let valid = await Task.detached(priority: .utility) { (try? UbuntuImage.verify(image)) != nil }.value
                if !valid {
                    message = "Downloading Ubuntu Linux \(UbuntuImage.version) ARM64"
                    let partial = cache.appendingPathComponent(UbuntuImage.filename + ".partial")
                    try await FedoraImageDownload.fetch(from: UbuntuImage.url, bytes: UbuntuImage.bytes, to: partial, distributionName: "Ubuntu") { offset, retrying in
                        Task { @MainActor in
                            guard self.running, !token.check(), self.fraction < 0.7 else { return }
                            self.fraction = max(self.fraction, Double(offset) / Double(UbuntuImage.bytes) * 0.7)
                            self.message = retrying ? "Reconnecting to Ubuntu. Downloaded segments are saved." : "Downloading Ubuntu Linux \(UbuntuImage.version) ARM64"
                        }
                    }
                    fraction = 0.7
                    message = "Verifying Ubuntu's image"
                    do {
                        try await Task.detached(priority: .utility) { try UbuntuImage.verify(partial) }.value
                    } catch {
                        try? fm.removeItem(at: partial)
                        throw error
                    }
                    try Task.checkCancellation()
                    if fm.fileExists(atPath: image.path) { try fm.removeItem(at: image) }
                    try fm.moveItem(at: partial, to: image)
                }
                if token.check() { throw CancellationError() }
                let root = (flavor?.rawValue ?? "ubuntu") + "-" + UUID().uuidString.lowercased()
                let metadata = ["distributionID": "ubuntu", "distribution": "Ubuntu Linux", "version": UbuntuImage.version,
                                "packageManager": "apt", "defaultShell": "/bin/bash", "image": UbuntuImage.filename,
                                "imageSHA256": UbuntuImage.sha256, "source": UbuntuImage.url.absoluteString,
                                "flavor": flavor?.rawValue ?? ""]
                message = "Extracting isolated Ubuntu filesystem"; fraction = 0.7
                try await Task.detached(priority: .utility) {
                    try LRRuntimeBridge.importVerifiedEnvironment(image, root: root, sha256: UbuntuImage.sha256, metadata: metadata,
                        progress: { value, text in Task { @MainActor in
                            guard self.running, !token.check(), self.fraction < 1 else { return }
                            self.fraction = max(self.fraction, 0.7 + value * 0.3); self.message = text
                        } },
                        cancel: { token.check() })
                }.value
                var environment = LinuxEnvironment(name: "\(displayName) ARM64", rootfsName: root)
                environment.distribution = flavor?.name ?? "Ubuntu Linux"; environment.distributionID = "ubuntu"
                environment.flavor = flavor?.rawValue
                environment.rootfsVersion = UbuntuImage.version; environment.packageManager = "apt"; environment.defaultShell = "/bin/bash"
                environment.installationState = "Imported; first-start validation pending"
                environment.guiAvailability = "Not installed"; environment.desktopAvailability = "Not installed"
                completion(environment)
                fraction = 1; message = flavor == nil ? "Ubuntu imported. Relaunch to validate the base filesystem and create your account." : "\(displayName) base imported. Relaunch to create your account; \(flavor!.vendor) is added during desktop setup."
                LRRuntimeBridge.logCategory("INSTALL", event: "Ubuntu image import completed")
            } catch {
                failure = error is CancellationError || token.check() ? "Installation cancelled. Saved download segments are retained for retry; existing environments are preserved." : error.localizedDescription
                LRRuntimeBridge.logCategory("INSTALL", event: failure ?? "Ubuntu installation failed")
            }
        }
    }
}

// Void Linux ARM64 (glibc) ROOTFS. SHA-256 pinned from Void's sha256sum.txt for
// the 20250202 release, whose minisign signature was verified against Void's
// release key void-release-20250202.pub (key id 4D56E70F102AF9F9). xbps keeps
// the image's signed repository configuration.
enum VoidImage {
    static let version = "20250202"
    static let filename = "void-aarch64-ROOTFS-20250202.tar.xz"
    static let url = URL(string: "https://repo-default.voidlinux.org/live/20250202/" + filename)!
    static let bytes: Int64 = 45_789_416
    static let sha256 = "01a30f17ae06d4d5b322cd579ca971bc479e02cc284ec1e5a4255bea6bac3ce6"

    static func verify(_ file: URL) throws {
        var file = file
        file.removeAllCachedResourceValues()
        let info = try file.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard info.isRegularFile == true, info.isSymbolicLink != true,
              Int64(info.fileSize ?? -1) == bytes,
              try DebianInstaller.sha256(file) == sha256 else {
            throw UbuntuInstallError.failure("Void Linux image verification failed. No filesystem was imported. Retry to download the image again.")
        }
    }
}

@MainActor final class VoidInstaller: ObservableObject {
    @Published var message = "Void Linux ARM64 · official ROOTFS · experimental"
    @Published var fraction = 0.0 { didSet { if let longTask { LongTaskCenter.shared.update(longTask, detail: message, fraction: fraction) } } }
    private var longTask: String?
    @Published var running = false {
        didSet {
            guard running != oldValue else { return }
            running ? LRRuntimeBridge.beginAwake("void-install") : LRRuntimeBridge.endAwake("void-install")
            if running { longTask = LongTaskCenter.shared.begin(title: "Installing Void Linux", detail: message, userStarted: true) }
            else if let token = longTask { longTask = nil; LongTaskCenter.shared.end(token, success: failure == nil, message: failure ?? message) }
        }
    }
    @Published var failure: String?
    private var operation: Task<Void, Never>?
    private var cancellation = UbuntuCancellation()
    func cancel() { cancellation.cancel(); operation?.cancel() }

    func install(completion: @escaping (LinuxEnvironment) -> Void) {
        guard !running else { return }
        running = true; failure = nil; fraction = 0; cancellation = UbuntuCancellation()
        let token = cancellation
        UserDefaults.standard.set(true, forKey: "InstallerActive")
        operation = Task {
            defer { running = false; UserDefaults.standard.set(false, forKey: "InstallerActive") }
            do {
                let fm = FileManager.default
                let cache = fm.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("VoidInstaller-v1", isDirectory: true)
                try fm.createDirectory(at: cache, withIntermediateDirectories: true)
                let image = cache.appendingPathComponent(VoidImage.filename)
                let storage = try LRRuntimeBridge.managedStorageURL().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeSupportsCaseSensitiveNamesKey])
                guard (storage.volumeAvailableCapacityForImportantUsage ?? 0) >= 1_500_000_000 else { throw UbuntuInstallError.failure("Void Linux needs at least 1.5 GB free for download and import. Desktop packages need additional space.") }
                if storage.volumeSupportsCaseSensitiveNames == false {
                    throw UbuntuInstallError.failure("This storage volume cannot preserve separate Linux filenames that differ only in case. Void Linux was not installed; existing environments are preserved.")
                }
                message = "Checking cached Void Linux image"
                let valid = await Task.detached(priority: .utility) { (try? VoidImage.verify(image)) != nil }.value
                if !valid {
                    message = "Downloading Void Linux ARM64"
                    let partial = cache.appendingPathComponent(VoidImage.filename + ".partial")
                    try await FedoraImageDownload.fetch(from: VoidImage.url, bytes: VoidImage.bytes, to: partial, distributionName: "Void Linux") { offset, retrying in
                        Task { @MainActor in
                            guard self.running, !token.check(), self.fraction < 0.7 else { return }
                            self.fraction = max(self.fraction, Double(offset) / Double(VoidImage.bytes) * 0.7)
                            self.message = retrying ? "Reconnecting to Void Linux. Downloaded segments are saved." : "Downloading Void Linux ARM64"
                        }
                    }
                    fraction = 0.7; message = "Verifying Void Linux's image"
                    do { try await Task.detached(priority: .utility) { try VoidImage.verify(partial) }.value }
                    catch { try? fm.removeItem(at: partial); throw error }
                    try Task.checkCancellation()
                    if fm.fileExists(atPath: image.path) { try fm.removeItem(at: image) }
                    try fm.moveItem(at: partial, to: image)
                }
                if token.check() { throw CancellationError() }
                let root = "void-" + UUID().uuidString.lowercased()
                let metadata = ["distributionID": "void", "distribution": "Void Linux", "version": VoidImage.version,
                                "packageManager": "xbps", "defaultShell": "/bin/bash", "image": VoidImage.filename,
                                "imageSHA256": VoidImage.sha256, "source": VoidImage.url.absoluteString]
                message = "Extracting isolated Void Linux filesystem"; fraction = 0.7
                try await Task.detached(priority: .utility) {
                    try LRRuntimeBridge.importVerifiedEnvironment(image, root: root, sha256: VoidImage.sha256, metadata: metadata,
                        progress: { value, text in Task { @MainActor in
                            guard self.running, !token.check(), self.fraction < 1 else { return }
                            self.fraction = max(self.fraction, 0.7 + value * 0.3); self.message = text
                        } },
                        cancel: { token.check() })
                }.value
                var environment = LinuxEnvironment(name: "Void Linux ARM64", rootfsName: root)
                environment.distribution = "Void Linux"; environment.distributionID = "void"
                environment.rootfsVersion = VoidImage.version; environment.packageManager = "xbps"; environment.defaultShell = "/bin/bash"
                environment.installationState = "Imported; first-start validation pending"
                environment.guiAvailability = "Not installed"; environment.desktopAvailability = "Not installed"
                completion(environment)
                fraction = 1; message = "Void Linux imported. Relaunch to validate the base filesystem and create your account."
                LRRuntimeBridge.logCategory("INSTALL", event: "Void Linux image import completed")
            } catch {
                failure = error is CancellationError || token.check() ? "Installation cancelled. Saved download segments are retained for retry; existing environments are preserved." : error.localizedDescription
                LRRuntimeBridge.logCategory("INSTALL", event: failure ?? "Void Linux installation failed")
            }
        }
    }
}
