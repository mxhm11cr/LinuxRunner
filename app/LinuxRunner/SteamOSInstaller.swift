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

// Large download with progress and resume. The finished file is moved into
// place inside the delegate callback, before URLSession deletes it.
private final class SteamOSDownloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private let destination: URL, resumeFile: URL, expectedBytes: Int64
    private let progress: @Sendable (Int64) -> Void
    private var continuation: CheckedContinuation<Void, Error>?
    private var session: URLSession?
    private var task: URLSessionDownloadTask?
    private var moveError: Error?

    init(destination: URL, expectedBytes: Int64, progress: @escaping @Sendable (Int64) -> Void) {
        self.destination = destination; self.expectedBytes = expectedBytes; self.progress = progress
        resumeFile = destination.appendingPathExtension("resume")
    }

    func run(_ url: URL) async throws {
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                self.continuation = continuation
                let configuration = URLSessionConfiguration.default
                configuration.timeoutIntervalForRequest = 120
                configuration.timeoutIntervalForResource = 24 * 3600
                configuration.waitsForConnectivity = true
                let session = URLSession(configuration: configuration, delegate: self, delegateQueue: nil)
                self.session = session
                if let data = try? Data(contentsOf: resumeFile) { task = session.downloadTask(withResumeData: data) }
                else { task = session.downloadTask(with: url) }
                task?.resume()
            }
        } onCancel: {
            // Keep resume data so a later attempt continues instead of restarting.
            self.task?.cancel(byProducingResumeData: { data in if let data { try? data.write(to: self.resumeFile, options: .atomic) } })
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        progress(totalBytesWritten)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        do {
            if let response = downloadTask.response as? HTTPURLResponse, !(200...299).contains(response.statusCode) {
                throw SteamOSInstallError.failure("Valve's server answered with HTTP \(response.statusCode).")
            }
            let size = (try location.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? 0
            guard size == expectedBytes else { throw SteamOSInstallError.failure("The SteamOS image download is incomplete. Try again.") }
            try? FileManager.default.removeItem(at: destination)
            try FileManager.default.moveItem(at: location, to: destination)
            try? FileManager.default.removeItem(at: resumeFile)
        } catch { moveError = error }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        session.finishTasksAndInvalidate()
        if let data = (error as NSError?)?.userInfo[NSURLSessionDownloadTaskResumeData] as? Data { try? data.write(to: resumeFile, options: .atomic) }
        let failure = error ?? moveError
        if let failure { continuation?.resume(throwing: failure) } else { continuation?.resume() }
        continuation = nil
    }
}

@MainActor final class SteamOSInstaller: ObservableObject {
    @Published var running = false {
        didSet { if running != oldValue { running ? LRRuntimeBridge.beginAwake("steamos-install") : LRRuntimeBridge.endAwake("steamos-install") } }
    }
    // Same task list, Live Activity and screensaver status as other installers.
    private var longTask: String?
    private func taskProgress() {
        if longTask == nil, running { longTask = LongTaskCenter.shared.begin(title: "Installing SteamOS ARM64", detail: message, userStarted: true) }
        if let longTask { LongTaskCenter.shared.update(longTask, detail: message, fraction: fraction) }
    }
    private func taskEnd(_ success: Bool, _ text: String) {
        if let longTask { LongTaskCenter.shared.end(longTask, success: success, message: text) }
        longTask = nil
    }
    @Published var message = "Installs SteamOS ARM64 from Valve's SteamFrame recovery image."
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

    // Where Valve's image and the pinned packages are kept until installation
    // succeeds (not in Documents, and excluded from backups).
    static var imageDirectory: URL {
        var url = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("SteamOSImage", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        var values = URLResourceValues(); values.isExcludedFromBackup = true
        try? url.setResourceValues(values)
        return url
    }
    static func downloadedImage(_ manifest: SteamOSManifest) -> URL? {
        guard let source = manifest.source else { return nil }
        let url = imageDirectory.appendingPathComponent(source.url.lastPathComponent)
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init)
        return size == source.bytes ? url : nil
    }

    // Downloads Valve's SteamFrame recovery image, checks its published
    // SHA-256 and installs its ARM64 system without a prepared archive.
    func installFromValve(completion: @escaping (LinuxEnvironment) -> Void) {
        guard !running else { return }
        running = true; failure = nil; fraction = 0
        control = SteamOSInstallControl()
        let token = control, identifier = UUID()
        operationID = identifier
        UserDefaults.standard.set(true, forKey: "InstallerActive")
        operation = Task {
            defer { operationID = nil; running = false; UserDefaults.standard.set(false, forKey: "InstallerActive") }
            do {
                let manifest = try SteamOSManifest.load()
                guard let source = manifest.source, let packages = manifest.packages else {
                    throw SteamOSInstallError.failure("This build cannot download SteamOS directly.")
                }
                let directory = Self.imageDirectory
                let image = directory.appendingPathComponent(source.url.lastPathComponent)
                let available = try LRRuntimeBridge.managedStorageURL().resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage ?? 0
                let required = manifest.directRequiredFreeBytes(imagePresent: Self.downloadedImage(manifest) != nil)
                guard available >= required else {
                    throw SteamOSInstallError.failure("Installing SteamOS needs " + ByteCountFormatter.string(fromByteCount: required, countStyle: .file) + " of free space (download, unpacking and the installed system). Free some space and try again.")
                }
                let download = 0.35
                if Self.downloadedImage(manifest) == nil {
                    message = "Downloading SteamOS from Valve"
                    taskProgress()
                    let total = Double(source.bytes)
                    try await SteamOSDownloader(destination: image, expectedBytes: source.bytes) { written in
                        guard token.reportProgress() else { return }
                        Task { @MainActor in
                            guard self.operationID == identifier, self.running else { return }
                            self.fraction = download * min(1, Double(written) / total)
                            self.message = "Downloading SteamOS from Valve (" + ByteCountFormatter.string(fromByteCount: written, countStyle: .file) + " of " + ByteCountFormatter.string(fromByteCount: source.bytes, countStyle: .file) + ")"
                            self.taskProgress()
                        }
                    }.run(source.url)
                }
                try Task.checkCancellation()
                message = "Downloading display packages"
                taskProgress()
                var packageFiles: [URL] = []
                for package in packages {
                    let file = directory.appendingPathComponent(package.name)
                    if ((try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init)) != package.bytes {
                        let (temporary, response) = try await URLSession.shared.download(from: package.url)
                        guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw SteamOSInstallError.failure("Could not download " + package.name + ".") }
                        try? FileManager.default.removeItem(at: file)
                        try FileManager.default.moveItem(at: temporary, to: file)
                    }
                    packageFiles.append(file)
                }
                let root = UUID().uuidString.lowercased()
                let metadata = ["distributionID": "steamos", "distribution": "SteamOS ARM64", "version": manifest.version,
                                "packageManager": "pacman", "defaultShell": "/bin/bash", "sourceImageSHA256": source.sha256]
                let work = directory.appendingPathComponent("work", isDirectory: true)
                message = "Verifying Valve's SteamOS image"
                fraction = download
                taskProgress()
                try await Task.detached(priority: .utility) {
                    try LRRuntimeBridge.installSteamOS(fromImage: image, imageSHA256: source.sha256, packages: packageFiles,
                        packageSHA256: packages.map(\.sha256), work: work, root: root, metadata: metadata,
                        progress: { value, text in
                            guard token.reportProgress() || value >= 1 else { return }
                            Task { @MainActor in
                                guard self.operationID == identifier, self.running else { return }
                                self.fraction = download + (1 - download) * value
                                self.message = text
                                self.taskProgress()
                            }
                        }, cancel: { token.isCancelled() })
                }.value
                // Installed: the image is no longer needed (about 3.8 GB).
                try? FileManager.default.removeItem(at: directory)
                var environment = LinuxEnvironment(name: "SteamOS ARM64 · Experimental", rootfsName: root)
                environment.distribution = "SteamOS ARM64"; environment.distributionID = "steamos"
                environment.packageManager = "pacman"; environment.rootfsVersion = manifest.version
                environment.defaultShell = "/bin/bash"; environment.installationState = "Installed from Valve's SteamFrame recovery image"
                environment.desktopAvailability = "Plasma X11 included; session checked at startup"
                UserDefaults.standard.set("plasma", forKey: "LinuxRunnerDesktop.environment." + root)
                completion(environment)
                fraction = 1; message = "SteamOS installed. Restart LinuxRunner to configure your account."
                taskEnd(true, message)
                LRRuntimeBridge.logCategory("INSTALL", event: "SteamOS ARM64 installed from Valve's recovery image; SHA-256 verified")
            } catch {
                let cancelled = token.isCancelled() || error is CancellationError || (error as NSError).code == NSUserCancelledError
                failure = cancelled ? "Installation cancelled. Downloaded data was kept, so trying again continues where it stopped." : error.localizedDescription
                LRRuntimeBridge.logCategory("INSTALL", event: failure ?? "SteamOS installation failed")
                taskEnd(false, failure ?? "SteamOS installation failed")
            }
        }
    }

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
                taskProgress()
                try await Task.detached(priority: .utility) {
                    try LRRuntimeBridge.importVerifiedEnvironment(source, root: root, sha256: manifest.archiveSHA256, metadata: metadata,
                        progress: { value, _ in
                            guard token.reportProgress() else { return }
                            Task { @MainActor in
                                guard self.operationID == identifier, self.running else { return }
                                self.fraction = value
                                self.message = value < 0.2 ? "Verifying the prepared SteamOS filesystem" : "Installing SteamOS"
                                self.taskProgress()
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
                taskEnd(true, message)
                LRRuntimeBridge.logCategory("INSTALL", event: "Prepared SteamOS ARM64 image imported; SHA-256 verified")
            } catch {
                failure = token.isCancelled() ? "Installation cancelled. Your selected archive and existing environments were kept." : error.localizedDescription
                LRRuntimeBridge.logCategory("INSTALL", event: failure ?? "SteamOS installation failed")
                taskEnd(false, failure ?? "SteamOS installation failed")
            }
        }
    }
}
