import Foundation
import SwiftUI

// Genuine Fedora AArch64 userspace, not a Fedora kernel or a WSL runtime.
// The digest was authenticated against Fedora's signed Fedora 44 CHECKSUM
// (key 36F612DCF27F7D1A48A835E4DBFCF71C6D9F90A6). Updating the image requires
// verifying the new signed checksum before changing this pin. RPM/DNF retain
// their normal signature verification inside the installed environment.
enum FedoraImage {
    static let version = "44"
    static let filename = "Fedora-WSL-Base-44-1.7.aarch64.wsl"
    static let url = URL(string: "https://dl.fedoraproject.org/pub/fedora/linux/releases/44/Container/aarch64/images/" + filename)!
    static let bytes: Int64 = 148_781_265
    static let sha256 = "1a3220262dc918b07d08410278205fb35e92c28a53455382028eea1a980476ee"

    static func verify(_ file: URL) throws {
        // The downloader may have cached the size on this URL before appending ranges.
        var file = file
        file.removeAllCachedResourceValues()
        let info = try file.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard info.isRegularFile == true, info.isSymbolicLink != true,
              Int64(info.fileSize ?? -1) == bytes,
              try DebianInstaller.sha256(file) == sha256 else {
            throw FedoraInstallError.failure("Fedora image verification failed. No filesystem was imported. Retry to download the authenticated image again.")
        }
    }
}

enum FedoraInstallError: LocalizedError {
    case failure(String)
    var errorDescription: String? { switch self { case .failure(let text): return text } }
}

private final class FedoraCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var stopped = false
    func cancel() { lock.lock(); stopped = true; lock.unlock() }
    func check() -> Bool { lock.lock(); defer { lock.unlock() }; return stopped }
}

// Keep only complete ranges across attempts. Fedora's release endpoint supports
// byte ranges, and the final signed size and SHA-256 still govern publication.
enum FedoraImageDownload {
    private static let chunkSize: Int64 = 4 * 1024 * 1024

    static func fetch(from source: URL, bytes: Int64, to partial: URL,
                      progress: @Sendable (Int64, Bool) -> Void) async throws {
        let fm = FileManager.default
        let configuration = URLSessionConfiguration.ephemeral
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = 30
        configuration.timeoutIntervalForResource = 120
        let session = URLSession(configuration: configuration)
        defer { session.finishTasksAndInvalidate() }

        if !fm.fileExists(atPath: partial.path) {
            try Data().write(to: partial, options: .withoutOverwriting)
        }
        let info = try partial.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard info.isRegularFile == true, info.isSymbolicLink != true,
              let size = info.fileSize, size >= 0, Int64(size) <= bytes else {
            try fm.removeItem(at: partial)
            throw FedoraInstallError.failure("Fedora's saved download was invalid. Tap Try Again to start a fresh download.")
        }

        var offset = Int64(size)
        progress(offset, false)
        while offset < bytes {
            try Task.checkCancellation()
            let end = min(offset + chunkSize, bytes) - 1
            var request = URLRequest(url: source)
            request.setValue("bytes=\(offset)-\(end)", forHTTPHeaderField: "Range")
            request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
            let (file, response) = try await fetchRange(request, using: session) {
                progress(offset, true)
            }
            defer { try? fm.removeItem(at: file) }
            try Task.checkCancellation()
            let length = Int64(try file.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? -1)
            if response.statusCode == 200, length == bytes {
                // A mirror may ignore Range and serve the entire pinned image.
                _ = try fm.replaceItemAt(partial, withItemAt: file)
                progress(bytes, false)
                return
            }
            guard response.statusCode == 206,
                  response.value(forHTTPHeaderField: "Content-Range") == "bytes \(offset)-\(end)/\(bytes)",
                  length == end - offset + 1 else {
                throw FedoraInstallError.failure("The Fedora server returned an invalid download segment (HTTP \(response.statusCode)). Tap Try Again to resume.")
            }
            let output = try FileHandle(forWritingTo: partial)
            defer { try? output.close() }
            try output.seek(toOffset: UInt64(offset))
            try output.write(contentsOf: Data(contentsOf: file))
            try output.close()
            offset = end + 1
            progress(offset, false)
        }
    }

    private static func fetchRange(_ request: URLRequest, using session: URLSession,
                                   retrying: () -> Void) async throws -> (URL, HTTPURLResponse) {
        for attempt in 0..<4 {
            try Task.checkCancellation()
            do {
                let (file, response) = try await session.download(for: request)
                guard let http = response as? HTTPURLResponse else {
                    try? FileManager.default.removeItem(at: file)
                    throw FedoraInstallError.failure("The Fedora server did not return an HTTP response.")
                }
                if ![408, 429, 500, 502, 503, 504].contains(http.statusCode) || attempt == 3 {
                    return (file, http)
                }
                try? FileManager.default.removeItem(at: file)
            } catch let error as URLError where
                [.timedOut, .cannotFindHost, .cannotConnectToHost, .networkConnectionLost,
                 .dnsLookupFailed, .notConnectedToInternet].contains(error.code) {
                if attempt == 3 { break }
            }
            retrying()
            try await Task.sleep(nanoseconds: UInt64(1 << attempt) * 1_000_000_000)
        }
        throw FedoraInstallError.failure("The Fedora connection kept dropping. Tap Try Again to resume from the last saved segment.")
    }
}

@MainActor final class FedoraInstaller: ObservableObject {
    @Published var message = "Fedora Linux 44 ARM64 · official image · experimental userspace"
    @Published var fraction = 0.0 { didSet { if let longTask { LongTaskCenter.shared.update(longTask, detail: message, fraction: fraction) } } }
    private var longTask: String?
    private func trackLongTask(_ title: String) {
        if running { longTask = LongTaskCenter.shared.begin(title: title, detail: message, userStarted: true) }
        else if let token = longTask { longTask = nil; LongTaskCenter.shared.end(token, success: failure == nil, message: failure ?? message) }
    }
    @Published var running = false {
        didSet { if running != oldValue { running ? LRRuntimeBridge.beginAwake("fedora-install") : LRRuntimeBridge.endAwake("fedora-install"); trackLongTask("Installing Fedora") } }
    }
    @Published var failure: String?
    private static var installing = false
    private var operation: Task<Void, Never>?
    private var cancellation = FedoraCancellation()
    func cancel() { cancellation.cancel(); operation?.cancel() }

    func install(completion: @escaping (LinuxEnvironment) -> Void) {
        guard !running else { return }
        guard !Self.installing else { failure = "Fedora Setup is already running."; return }
        Self.installing = true
        running = true; failure = nil; fraction = 0; cancellation = FedoraCancellation()
        let token = cancellation
        UserDefaults.standard.set(true, forKey: "InstallerActive")
        operation = Task {
            defer { Self.installing = false; running = false; UserDefaults.standard.set(false, forKey: "InstallerActive") }
            do {
                let fm = FileManager.default
                let cache = fm.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("FedoraInstaller-v1", isDirectory: true)
                try fm.createDirectory(at: cache, withIntermediateDirectories: true)
                let image = cache.appendingPathComponent(FedoraImage.filename)
                let storage = try LRRuntimeBridge.managedStorageURL().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeSupportsCaseSensitiveNamesKey])
                let available = storage.volumeAvailableCapacityForImportantUsage ?? 0
                guard available >= 2_000_000_000 else { throw FedoraInstallError.failure("Fedora needs at least 2 GB free for download and isolated base filesystem import. Desktop packages need additional space.") }
                // Fedora has distinct _Exit.2.gz and _exit.2.gz, among other
                // case-sensitive paths. Never merge or drop files to fit a
                // case-insensitive host (the default Mac Simulator location).
                if storage.volumeSupportsCaseSensitiveNames == false {
                    #if targetEnvironment(simulator)
                    throw FedoraInstallError.failure("Fedora needs case-sensitive root storage. Run this Simulator from a case-sensitive device set (simctl --set). The image and existing environments are preserved.")
                    #else
                    throw FedoraInstallError.failure("This storage volume cannot preserve separate Linux filenames that differ only in case. Fedora was not installed; existing environments are preserved.")
                    #endif
                }
                message = "Checking cached Fedora image"
                let valid = await Task.detached(priority: .utility) { (try? FedoraImage.verify(image)) != nil }.value
                if !valid {
                    try Task.checkCancellation()
                    message = "Downloading Fedora Linux 44 ARM64"
                    let partial = cache.appendingPathComponent(FedoraImage.filename + ".partial")
                    try await FedoraImageDownload.fetch(from: FedoraImage.url, bytes: FedoraImage.bytes, to: partial) { offset, retrying in
                        Task { @MainActor in
                            guard self.running, !token.check(), self.fraction < 0.7 else { return }
                            self.fraction = max(self.fraction, Double(offset) / Double(FedoraImage.bytes) * 0.7)
                            self.message = retrying ? "Reconnecting to Fedora. Downloaded segments are saved." : "Downloading Fedora Linux 44 ARM64"
                        }
                    }
                    fraction = 0.7
                    message = "Verifying Fedora's authenticated image"
                    do {
                        try await Task.detached(priority: .utility) { try FedoraImage.verify(partial) }.value
                    } catch {
                        try? fm.removeItem(at: partial)
                        throw error
                    }
                    try Task.checkCancellation()
                    if fm.fileExists(atPath: image.path) { try fm.removeItem(at: image) }
                    try fm.moveItem(at: partial, to: image)
                }
                if token.check() { throw CancellationError() }
                let root = "fedora-" + UUID().uuidString.lowercased()
                let metadata = ["distributionID": "fedora", "distribution": "Fedora Linux", "version": FedoraImage.version,
                                "packageManager": "dnf", "defaultShell": "/bin/bash", "image": FedoraImage.filename,
                                "imageSHA256": FedoraImage.sha256, "source": FedoraImage.url.absoluteString]
                message = "Extracting isolated Fedora filesystem"; fraction = 0.7
                try await Task.detached(priority: .utility) {
                    // The shared importer snapshots and verifies these exact bytes again,
                    // preserves guest metadata, and publishes the root atomically.
                    try LRRuntimeBridge.importVerifiedEnvironment(image, root: root, sha256: FedoraImage.sha256, metadata: metadata,
                        progress: { value, text in Task { @MainActor in
                            guard self.running, !token.check(), self.fraction < 1 else { return }
                            self.fraction = max(self.fraction, 0.7 + value * 0.3); self.message = text
                        } },
                        cancel: { token.check() })
                }.value
                var environment = LinuxEnvironment(name: "Fedora Linux 44 ARM64", rootfsName: root)
                environment.distribution = "Fedora Linux"; environment.distributionID = "fedora"
                environment.rootfsVersion = FedoraImage.version; environment.packageManager = "dnf"; environment.defaultShell = "/bin/bash"
                environment.installationState = "Imported; first-start validation pending"
                environment.guiAvailability = "Not installed"; environment.desktopAvailability = "Not installed"
                completion(environment)
                fraction = 1; message = "Fedora imported. Relaunch to validate the base filesystem and configure your account."
                LRRuntimeBridge.logCategory("INSTALL", event: "Fedora authenticated image import completed")
            } catch {
                failure = error is CancellationError || token.check() ? "Installation cancelled. Saved download segments are retained for retry; existing environments are preserved." : error.localizedDescription
                LRRuntimeBridge.logCategory("INSTALL", event: failure ?? "Fedora installation failed")
            }
        }
    }
}
