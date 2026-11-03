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

    static let sourcePin = "3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676"
    var requiredFreeBytes: Int64 { archiveBytes + installedBytes + 512 * 1024 * 1024 }

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
        return manifest
    }

    static func load() throws -> Self {
        guard let bundle = Bundle.main.url(forResource: "SteamOSSupport", withExtension: "bundle") else {
            throw SteamOSInstallError.failure("The prepared SteamOS image manifest is missing from this app.")
        }
        return try decode(Data(contentsOf: bundle.appendingPathComponent("manifest.json")))
    }
}
