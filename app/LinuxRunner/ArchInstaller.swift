import Foundation
import CryptoKit
import Security
import SwiftUI

// Arch Linux ARM (aarch64) userspace installer — experimental.
//
// Trust model: Arch Linux ARM does not sign its repository databases, so the
// database (fetched over HTTPS) only names packages. Every package must carry
// a valid OpenPGP signature by the Arch Linux ARM Build System key, embedded
// below and checked against its published fingerprint at run time, and must
// match the SHA-256 in the database. Packages failing either check abort the
// installation. Nothing is extracted onto the host: an authenticated guest tar
// is assembled (DebianBootstrap.c) and imported like Debian.

enum ArchInstallError: LocalizedError {
    case failure(String)
    var errorDescription: String? { switch self { case .failure(let text): return text } }
}

/// RFC 4880 version-4 detached signatures, RSA, as used by Arch Linux ARM packages.
enum AlarmSignature {
    /// Arch Linux ARM Build System <builder@archlinuxarm.org> — archlinuxarm.org/about/package-signing
    static let fingerprint = "68B3537F39A313B3E574D06777193F152BDBE6A6"
    /// Public-key packet body (tag 6), from the archlinuxarm-keyring package.
    static let keyPacket = "BFLbBPMBEADNB2XChJQplQwbAcl8wkhsPZOozGhxUYO+BVEF5vKjxcNzeR57cjj1veSw4aMmEv03MkBHi9Kyyk2wKUkFHuTx4DA5ZxnTt+2ScEezEFcmEoLsRYideQ35tYWaFjpjZDLbR4bp0EumCi8zvxwQhXl1y4mRZtBCX8z4otdgXk8dBUSJJsHgJsmRobzNrBDGEr55nNbT88BxVcG39idEb/VOOqS24rogNJvkQUdRwmu8BGbSWI/IaLB0Wc3cMEMYCf5TjEwS9HsSvUOmdWE6RWibpnaQp9/CC2PHjP1QnXby3b1tA4nNm3IkP8HqwpeIrSL3hXSGKMn3r8D/Sil1kfU1U5nll0yk1hFoAfVK4AWEvcXHqy3BwDYa1iyiiqGxmNRyiK68EZlc20uGjG/GsNJx+/tAqWTOW8sJJrp+YGT/07uH2iRPivjWAetgih6xlmbbkskKm4hQI+sozWFObQzPe9R1lNLfncGXxxOWkkvGlbdZVXYnPSKf6dF+H+lZLjSvhRznTlz+jM/Ou/9bmzf0kvJt6fOI4aR8ZeseXEJfxpA0BdbxarjPUrPj9XVLu75bFXMzeRIEW3zLd9OiHAnxfLlm/WDwc0zYjJIyU0V/KVhYx7KqmNlD5zpg4gIh+n53OpSZPsLCWxwa/5y8w8HLOEpZRB/N6ocmzISSiqWUlwARAQAB"

    struct Key { let secKey: SecKey; let modulusBytes: Int }

    static func key() throws -> Key {
        guard let body = Data(base64Encoded: keyPacket), body.count > 10, body[0] == 4, body[5] == 1 else { throw ArchInstallError.failure("Embedded Arch Linux ARM key is malformed.") }
        var header = Data([0x99, UInt8(body.count >> 8), UInt8(body.count & 0xff)]); header.append(body)
        let fpr = Insecure.SHA1.hash(data: header).map { String(format: "%02X", $0) }.joined()
        guard fpr == fingerprint else { throw ArchInstallError.failure("Embedded Arch Linux ARM key does not match its fingerprint.") }
        var cursor = 6
        guard let n = mpi(body, &cursor), let e = mpi(body, &cursor) else { throw ArchInstallError.failure("Embedded Arch Linux ARM key is malformed.") }
        let der = sequence(integer(n) + integer(e))   // PKCS#1 RSAPublicKey
        let attributes: [CFString: Any] = [kSecAttrKeyType: kSecAttrKeyTypeRSA, kSecAttrKeyClass: kSecAttrKeyClassPublic, kSecAttrKeySizeInBits: n.count * 8]
        var error: Unmanaged<CFError>?
        guard let secKey = SecKeyCreateWithData(der as CFData, attributes as CFDictionary, &error) else { throw ArchInstallError.failure("Arch Linux ARM key could not be loaded.") }
        return Key(secKey: secKey, modulusBytes: n.count)
    }

    /// Verifies a detached binary signature over the file. Throws on any failure.
    static func verify(file: URL, signature: Data, key: Key) throws {
        var c = 0
        guard let packet = packetBody(signature, &c, expectedTag: 2) else { throw ArchInstallError.failure("Invalid package signature packet.") }
        guard packet.count > 10, packet[0] == 4, packet[1] == 0x00, packet[2] == 1, [8, 10].contains(packet[3]) else { throw ArchInstallError.failure("Unsupported package signature (need v4 RSA binary signature, SHA-256/512).") }
        let hashedLength = Int(packet[4]) << 8 | Int(packet[5])
        guard 6 + hashedLength + 2 <= packet.count else { throw ArchInstallError.failure("Truncated package signature.") }
        let hashed = packet.subdata(in: 6..<(6 + hashedLength))
        let unhashedLength = Int(packet[6 + hashedLength]) << 8 | Int(packet[7 + hashedLength])
        var p = 8 + hashedLength + unhashedLength
        guard p + 2 <= packet.count else { throw ArchInstallError.failure("Truncated package signature.") }
        let unhashed = packet.subdata(in: (8 + hashedLength)..<p)
        let left16 = packet.subdata(in: p..<(p + 2)); p += 2
        guard let sig = mpi(packet, &p) else { throw ArchInstallError.failure("Truncated package signature.") }
        // Issuer must be the Arch Linux ARM key; unknown critical subpackets reject.
        let issuer = try subpackets(hashed, critical: true) + (try subpackets(unhashed, critical: false))
        let keyID = String(fingerprint.suffix(16))
        guard issuer.contains(fingerprint) || issuer.contains(keyID) else { throw ArchInstallError.failure("Package is not signed by the Arch Linux ARM key.") }
        var trailer = Data(packet.prefix(6 + hashedLength))
        trailer.append(contentsOf: [0x04, 0xff]); let l = UInt32(6 + hashedLength)
        trailer.append(contentsOf: [UInt8(l >> 24), UInt8((l >> 16) & 0xff), UInt8((l >> 8) & 0xff), UInt8(l & 0xff)])
        let digest: Data; let algorithm: SecKeyAlgorithm
        let handle = try FileHandle(forReadingFrom: file); defer { try? handle.close() }
        if packet[3] == 10 {
            var h = SHA512(); while let d = try handle.read(upToCount: 1 << 20), !d.isEmpty { h.update(data: d) }; h.update(data: trailer)
            digest = Data(h.finalize()); algorithm = .rsaSignatureDigestPKCS1v15SHA512
        } else {
            var h = SHA256(); while let d = try handle.read(upToCount: 1 << 20), !d.isEmpty { h.update(data: d) }; h.update(data: trailer)
            digest = Data(h.finalize()); algorithm = .rsaSignatureDigestPKCS1v15SHA256
        }
        guard digest.prefix(2) == left16 else { throw ArchInstallError.failure("Package signature does not match the file.") }
        let padded = Data(repeating: 0, count: max(0, key.modulusBytes - sig.count)) + sig
        var error: Unmanaged<CFError>?
        guard SecKeyVerifySignature(key.secKey, algorithm, digest as CFData, padded as CFData, &error) else { throw ArchInstallError.failure("Package signature is invalid.") }
    }

    // MARK: OpenPGP encoding helpers
    private static func mpi(_ d: Data, _ c: inout Int) -> Data? {
        guard c + 2 <= d.count else { return nil }
        let bits = Int(d[d.startIndex + c]) << 8 | Int(d[d.startIndex + c + 1]); let len = (bits + 7) / 8; c += 2
        guard c + len <= d.count else { return nil }
        defer { c += len }; return d.subdata(in: (d.startIndex + c)..<(d.startIndex + c + len))
    }
    private static func packetBody(_ d: Data, _ c: inout Int, expectedTag: Int) -> Data? {
        guard c < d.count, d[c] & 0x80 != 0 else { return nil }
        let b = d[c]; var tag = 0, len = 0; c += 1
        if b & 0x40 != 0 {
            tag = Int(b & 0x3f); guard c < d.count else { return nil }
            let l0 = Int(d[c]); c += 1
            if l0 < 192 { len = l0 } else if l0 < 224 { guard c < d.count else { return nil }; len = ((l0 - 192) << 8) + Int(d[c]) + 192; c += 1 }
            else if l0 == 255 { guard c + 4 <= d.count else { return nil }; len = d[c..<(c + 4)].reduce(0) { $0 << 8 | Int($1) }; c += 4 } else { return nil }
        } else {
            tag = Int((b >> 2) & 0xf); let n = [1, 2, 4][Int(b & 3) < 3 ? Int(b & 3) : 0]
            guard b & 3 != 3, c + n <= d.count else { return nil }
            len = d[c..<(c + n)].reduce(0) { $0 << 8 | Int($1) }; c += n
        }
        guard tag == expectedTag, c + len <= d.count else { return nil }
        defer { c += len }; return d.subdata(in: c..<(c + len))
    }
    /// Issuer identities (fingerprint / key ID, uppercase hex) from a subpacket area.
    private static func subpackets(_ d: Data, critical: Bool) throws -> [String] {
        var out: [String] = []; var c = 0
        while c < d.count {
            let l0 = Int(d[c]); c += 1; var len = 0
            if l0 < 192 { len = l0 } else if l0 < 255 { guard c < d.count else { break }; len = ((l0 - 192) << 8) + Int(d[c]) + 192; c += 1 }
            else { guard c + 4 <= d.count else { break }; len = d[c..<(c + 4)].reduce(0) { $0 << 8 | Int($1) }; c += 4 }
            guard len >= 1, c + len <= d.count else { throw ArchInstallError.failure("Malformed signature subpacket.") }
            let type = Int(d[c] & 0x7f), isCritical = d[c] & 0x80 != 0
            let value = d.subdata(in: (c + 1)..<(c + len)); c += len
            switch type {
            case 33 where value.count == 21 && value[0] == 4: out.append(value.dropFirst().map { String(format: "%02X", $0) }.joined())
            case 16 where value.count == 8: out.append(value.map { String(format: "%02X", $0) }.joined())
            case 2, 3, 4, 9, 11, 16, 20, 21, 22, 23, 26, 27, 28, 30, 31, 32, 33: break
            default: if isCritical && critical { throw ArchInstallError.failure("Unsupported critical signature subpacket \(type).") }
            }
        }
        return out
    }
    private static func derLength(_ n: Int) -> Data {
        if n < 128 { return Data([UInt8(n)]) }
        var bytes: [UInt8] = []; var v = n; while v > 0 { bytes.insert(UInt8(v & 0xff), at: 0); v >>= 8 }
        return Data([0x80 | UInt8(bytes.count)] + bytes)
    }
    private static func integer(_ v: Data) -> Data {
        var body = v; if let first = body.first, first & 0x80 != 0 { body.insert(0, at: 0) }
        return Data([0x02]) + derLength(body.count) + body
    }
    private static func sequence(_ body: Data) -> Data { Data([0x30]) + derLength(body.count) + body }
}

/// One package from the repository database (desc + depends).
struct ArchPackage {
    var fields: [String: [String]]
    var name: String { fields["NAME"]?.first ?? "" }
    var version: String { fields["VERSION"]?.first ?? "" }
    var filename: String { fields["FILENAME"]?.first ?? "" }
    var sha256: String { fields["SHA256SUM"]?.first ?? "" }
    var compressedSize: Int64 { Int64(fields["CSIZE"]?.first ?? "") ?? 0 }
    var repository = ""

    static func parseDatabase(_ text: String, repository: String) -> [String: ArchPackage] {
        var result: [String: ArchPackage] = [:]
        for entry in text.split(separator: "\u{1e}") {
            var fields: [String: [String]] = [:]; var key: String?
            for line in entry.split(separator: "\n", omittingEmptySubsequences: false).dropFirst() {
                if line.count > 2, line.hasPrefix("%"), line.hasSuffix("%"), line.dropFirst().dropLast().allSatisfy({ $0.isUppercase || $0.isNumber }) {
                    key = String(line.dropFirst().dropLast()); fields[key!] = []
                } else if let k = key, !line.isEmpty { fields[k, default: []].append(String(line)) }
            }
            var p = ArchPackage(fields: fields); p.repository = repository
            if !p.name.isEmpty { result[p.name] = p }
        }
        return result
    }

    /// pacman local-database desc for an installed package.
    func localDescription(explicit: Bool) -> String {
        var out = ""
        for k in ["NAME", "VERSION", "BASE", "DESC", "URL", "ARCH", "BUILDDATE"] where fields[k] != nil { out += "%\(k)%\n" + fields[k]!.joined(separator: "\n") + "\n\n" }
        out += "%INSTALLDATE%\n\(Int(Date().timeIntervalSince1970))\n\n"
        for k in ["PACKAGER"] where fields[k] != nil { out += "%\(k)%\n" + fields[k]!.joined(separator: "\n") + "\n\n" }
        out += "%SIZE%\n\(fields["ISIZE"]?.first ?? "0")\n\n%REASON%\n\(explicit ? 0 : 1)\n\n%VALIDATION%\npgp\n\n"
        for k in ["LICENSE", "GROUPS", "REPLACES", "DEPENDS", "OPTDEPENDS", "CONFLICTS", "PROVIDES"] where fields[k] != nil { out += "%\(k)%\n" + fields[k]!.joined(separator: "\n") + "\n\n" }
        return out
    }
}

enum ArchResolver {
    static let basePackages = ["filesystem", "bash", "coreutils", "glibc", "pacman", "archlinuxarm-keyring", "sed", "grep", "gawk",
                               "findutils", "tar", "gzip", "xz", "zstd", "which", "less", "procps-ng", "shadow", "util-linux",
                               "ca-certificates", "iana-etc", "sudo", "nano", "file", "diffutils"]
    static func stripVersion(_ dep: String) -> String { String(dep.prefix { !"<>=".contains($0) }) }
    /// Dependency closure, resolving virtual names through PROVIDES.
    static func resolve(_ packages: [String: ArchPackage], wanted: [String]) throws -> [ArchPackage] {
        var provides: [String: String] = [:]
        for (name, p) in packages.sorted(by: { $0.key < $1.key }) { for v in p.fields["PROVIDES"] ?? [] where provides[stripVersion(v)] == nil { provides[stripVersion(v)] = name } }
        var order: [ArchPackage] = []; var seen = Set<String>(); var stack = wanted.reversed().map { $0 }
        while let dep = stack.popLast() {
            let bare = stripVersion(dep)
            guard let name = packages[bare] != nil ? bare : provides[bare] else { throw ArchInstallError.failure("Arch Linux ARM dependency cannot be resolved: \(bare)") }
            if !seen.insert(name).inserted { continue }
            let p = packages[name]!; order.append(p)
            stack.append(contentsOf: (p.fields["DEPENDS"] ?? []).reversed())
        }
        return order
    }
}

private final class ArchCancellation: @unchecked Sendable {
    private let lock = NSLock(); private var stopped = false
    func cancel() { lock.lock(); stopped = true; lock.unlock() }
    func check() -> Bool { lock.lock(); defer { lock.unlock() }; return stopped }
}

@MainActor final class ArchInstaller: ObservableObject {
    static let mirrors = ["https://fl.us.mirror.archlinuxarm.org/aarch64", "https://de.mirror.archlinuxarm.org/aarch64", "https://eu.mirror.archlinuxarm.org/aarch64"]
    @Published var message = "Arch Linux ARM (aarch64) · official signed packages · experimental"
    @Published var fraction = 0.0 { didSet { if let longTask { LongTaskCenter.shared.update(longTask, detail: message, fraction: fraction) } } }
    private var longTask: String?
    private func trackLongTask(_ title: String) {
        if running { longTask = LongTaskCenter.shared.begin(title: title, detail: message, userStarted: true) }
        else if let token = longTask { longTask = nil; LongTaskCenter.shared.end(token, success: failure == nil, message: failure ?? message) }
    }
    @Published var running = false { didSet { if running != oldValue { running ? LRRuntimeBridge.beginAwake("arch-install") : LRRuntimeBridge.endAwake("arch-install"); trackLongTask("Installing Arch Linux ARM") } } }
    @Published var failure: String?
    private var operation: Task<Void, Never>?
    private var cancellation = ArchCancellation()
    func cancel() { cancellation.cancel(); operation?.cancel() }

    private static func fetch(_ path: String, to destination: URL) async throws {
        var lastError = "no mirror reachable"
        for mirror in mirrors {
            guard let url = URL(string: mirror + "/" + path) else { continue }
            do {
                let (download, response) = try await URLSession.shared.download(from: url)
                defer { try? FileManager.default.removeItem(at: download) }
                let status = (response as? HTTPURLResponse)?.statusCode ?? -1
                guard status == 200 else { lastError = "HTTP \(status) from \(url.host ?? mirror)"; continue }
                try? FileManager.default.removeItem(at: destination)
                try FileManager.default.moveItem(at: download, to: destination)
                return
            } catch { lastError = error.localizedDescription }
        }
        throw ArchInstallError.failure("Download failed for \(path): \(lastError)")
    }

    func install(completion: @escaping (LinuxEnvironment) -> Void) {
        guard !running else { return }; running = true; failure = nil; cancellation = ArchCancellation()
        let token = cancellation
        UserDefaults.standard.set(true, forKey: "InstallerActive")
        operation = Task {
            defer { running = false; UserDefaults.standard.set(false, forKey: "InstallerActive") }
            do {
                let key = try AlarmSignature.key()
                let base = LRRuntimeBridge.managedStorageURL()
                let available = try base.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage ?? 0
                guard available >= Int64(1536) * 1024 * 1024 else { throw ArchInstallError.failure("At least 1.5 GB of available storage is required.") }
                let cache = base.appendingPathComponent("downloads/archlinuxarm-aarch64", isDirectory: true)
                let temporary = base.appendingPathComponent("installer-temporary", isDirectory: true)
                try FileManager.default.createDirectory(at: cache, withIntermediateDirectories: true)
                try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
                message = "Reading the Arch Linux ARM package index"
                var packages: [String: ArchPackage] = [:]
                for repo in ["core", "alarm", "extra"] {
                    try Task.checkCancellation()
                    let db = cache.appendingPathComponent("\(repo).db")
                    try await Self.fetch("\(repo)/\(repo).db", to: db)
                    let text = try LRRuntimeBridge.readArchDatabase(db)
                    for (name, p) in ArchPackage.parseDatabase(text, repository: repo) where packages[name] == nil { packages[name] = p }
                }
                let order = try ArchResolver.resolve(packages, wanted: ArchResolver.basePackages)
                let total = order.reduce(Int64(0)) { $0 + $1.compressedSize }
                DiagnosticCenter.installer(2, message: "stage=arch_resolve packages=\(order.count) download_bytes=\(total)")
                var paths: [String] = [], names: [String] = [], descriptions: [String] = []
                var done: Int64 = 0
                for (index, package) in order.enumerated() {
                    try Task.checkCancellation(); if token.check() { throw CancellationError() }
                    guard package.filename.hasSuffix(".pkg.tar.xz"), !package.filename.contains("/"), package.sha256.count == 64,
                          let sig = Data(base64Encoded: (package.fields["PGPSIG"] ?? []).joined()) else {
                        throw ArchInstallError.failure("Unsupported or unsigned package entry: \(package.name)")
                    }
                    message = "Downloading/verifying \(package.name) (\(index + 1)/\(order.count))"
                    fraction = total > 0 ? Double(done) / Double(total) * 0.6 : 0
                    let destination = cache.appendingPathComponent(package.filename)
                    if (try? DebianInstaller.sha256(destination)) != package.sha256 {
                        try await Self.fetch("\(package.repository)/\(package.filename)", to: destination)
                    }
                    let sha = try DebianInstaller.sha256(destination)
                    guard sha == package.sha256 else { throw ArchInstallError.failure("Integrity verification failed for \(package.name): SHA-256 mismatch.") }
                    try await Task.detached(priority: .utility) { try AlarmSignature.verify(file: destination, signature: sig, key: key) }.value
                    DiagnosticCenter.installer(2, message: "stage=arch_verify package=\(package.name) version=\(package.version) sha256=\(sha) signature=PASS")
                    paths.append(destination.path); names.append("\(package.name)-\(package.version)")
                    descriptions.append(package.localDescription(explicit: ArchResolver.basePackages.contains(package.name)))
                    done += package.compressedSize
                }
                let root = UUID().uuidString.lowercased()
                let archive = temporary.appendingPathComponent("\(root).tar.gz")
                defer { try? FileManager.default.removeItem(at: archive) }
                message = "Assembling verified Arch Linux ARM packages"
                try await Task.detached(priority: .utility) {
                    try LRRuntimeBridge.assembleArchPackages(paths, localNames: names, localDescriptions: descriptions, toArchive: archive,
                                                             progress: { value in Task { @MainActor in self.fraction = 0.6 + value * 0.2 } }, cancel: { token.check() })
                }.value
                message = "Importing isolated Arch Linux ARM filesystem"
                let sha = try DebianInstaller.sha256(archive)
                let metadata = ["distributionID": "arch", "distribution": "Arch Linux ARM", "version": "rolling", "packageManager": "pacman", "defaultShell": "/bin/bash"]
                try await Task.detached(priority: .utility) {
                    try LRRuntimeBridge.importVerifiedEnvironment(archive, root: root, sha256: sha, metadata: metadata,
                                                                  progress: { value, _ in Task { @MainActor in self.fraction = 0.8 + value * 0.2 } }, cancel: { token.check() })
                }.value
                var environment = LinuxEnvironment(name: "Arch Linux ARM", rootfsName: root)
                environment.distribution = "Arch Linux ARM"; environment.distributionID = "arch"; environment.packageManager = "pacman"
                environment.rootfsVersion = "rolling"; environment.defaultShell = "/bin/bash"
                environment.installationState = "Imported; first-boot initialization runs automatically"
                environment.guiAvailability = "Not checked"; environment.desktopAvailability = "Desktop components not installed"
                completion(environment)
                message = "Imported. Select Arch Linux ARM for the next launch; initialization runs automatically."
                fraction = 1
                LRRuntimeBridge.logCategory("INSTALL", event: "Arch Linux ARM verified package import completed")
            } catch {
                failure = error is CancellationError || token.check() ? "Installation cancelled. Verified downloads are kept for retry." : error.localizedDescription
                DiagnosticCenter.installer(0, message: "stage=arch_installer_failure reason=\(failure ?? error.localizedDescription)")
                DiagnosticCenter.flush()
                LRRuntimeBridge.logCategory("INSTALL", event: failure ?? "Arch installation failed")
            }
        }
    }
}

struct ArchInstallerView: View {
    @StateObject private var installer = ArchInstaller()
    let completion: (LinuxEnvironment) -> Void
    var body: some View {
        List {
            Text("Arch Linux ARM · aarch64 · rolling release")
            Text("Experimental. Downloads official Arch Linux ARM packages; each must match its SHA-256 and carry a valid signature by the Arch Linux ARM Build System key. Use Set Up Linux to configure an account and install X11 desktop components. Wayland is not supported. Requires 1.5 GB free space. Existing environments are kept.").font(.footnote)
            Text(installer.message)
            if installer.running { ProgressView(value: installer.fraction); Button("Cancel Installation") { installer.cancel() } }
            else { Button("Install Arch Linux ARM") { installer.install(completion: completion) } }
            if let failure = installer.failure { Text(failure).foregroundColor(.red) }
        }.navigationTitle("Install Arch Linux ARM")
    }
}
