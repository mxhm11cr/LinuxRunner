import Foundation

enum SteamOSInstallError: LocalizedError {
    case failure(String)
    var errorDescription: String? { switch self { case .failure(let message): return message } }
}

struct SteamOSManifest: Decodable {
    let schema: Int
    let architecture: String
    let version: String
    let sourceImageSHA256: String
    let archiveSHA256: String
    let archiveBytes: Int64
    let installedBytes: Int64
    let entries: Int
    // Valve's recovery image and the pinned display packages, for installing
    // without a prepared archive.
    struct Source: Decodable { let url: URL; let bytes: Int64; let sha256: String; let partition: String; let partitionBytes: Int64 }
    struct Package: Decodable { let name: String; let url: URL; let sha256: String; let bytes: Int64 }
    let source: Source?
    let packages: [Package]?
    let streamInstalledBytes: Int64?

    static let sourcePin = "3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676"
    var requiredFreeBytes: Int64 { archiveBytes + installedBytes + 512 * 1024 * 1024 }
    // Download (unless already present) + unpacked partition + installed root + headroom.
    func directRequiredFreeBytes(imagePresent: Bool) -> Int64 {
        guard let source else { return .max }
        return (imagePresent ? 0 : source.bytes) + source.partitionBytes + (streamInstalledBytes ?? installedBytes) + 1024 * 1024 * 1024
    }
    var supportsDirectInstall: Bool { source != nil && packages != nil }

    static func decode(_ data: Data) throws -> Self {
        guard data.count <= 65536 else { throw SteamOSInstallError.failure("SteamOS image manifest is too large.") }
        let manifest = try JSONDecoder().decode(Self.self, from: data)
        guard manifest.schema == 1, manifest.architecture == "arm64", manifest.version == "0.3.0",
              manifest.sourceImageSHA256 == sourcePin,
              manifest.archiveSHA256.count == 64,
              manifest.archiveSHA256.allSatisfy({ "0123456789abcdef".contains($0) }),
              manifest.archiveBytes > 0, manifest.archiveBytes <= 6 * 1024 * 1024 * 1024,
              manifest.installedBytes > 0, manifest.installedBytes <= 16 * 1024 * 1024 * 1024,
              manifest.entries > 0, manifest.entries <= 500000 else {
            throw SteamOSInstallError.failure("This app does not contain a supported prepared SteamOS image manifest.")
        }
        let hex = { (value: String) in value.count == 64 && value.allSatisfy({ "0123456789abcdef".contains($0) }) }
        if let source = manifest.source {
            guard source.url.scheme == "https", source.url.host == "steamdeck-images.steamos.cloud", source.sha256 == sourcePin,
                  source.bytes > 0, source.bytes <= 8 * 1024 * 1024 * 1024, source.partition == "rootfs-A",
                  source.partitionBytes > 0, source.partitionBytes <= 16 * 1024 * 1024 * 1024 else {
                throw SteamOSInstallError.failure("This app's SteamOS source entry is not supported.")
            }
        }
        if let packages = manifest.packages {
            guard packages.count <= 8, packages.allSatisfy({ $0.url.scheme == "https" && $0.url.host == "fl.us.mirror.archlinuxarm.org" &&
                    hex($0.sha256) && $0.bytes > 0 && $0.bytes <= 64 * 1024 * 1024 && !$0.name.contains("/") && $0.url.lastPathComponent == $0.name }) else {
                throw SteamOSInstallError.failure("This app's SteamOS package list is not supported.")
            }
        }
        return manifest
    }

    static func load() throws -> Self {
        guard let bundle = Bundle.main.url(forResource: "SteamOSSupport", withExtension: "bundle") else {
            throw SteamOSInstallError.failure("The prepared SteamOS image manifest is missing from this app.")
        }
        return try decode(Data(contentsOf: bundle.appendingPathComponent("manifest.json")))
    }
}
