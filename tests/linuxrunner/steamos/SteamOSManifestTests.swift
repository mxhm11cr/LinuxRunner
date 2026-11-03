import Foundation

@main struct SteamOSManifestTests {
    static func main() throws {
        let valid: [String: Any] = ["schema": 1, "architecture": "arm64", "version": "0.3.0",
                                   "sourceImageSHA256": SteamOSManifest.sourcePin,
                                   "archiveSHA256": String(repeating: "a", count: 64),
                                   "archiveBytes": 2_000_000_000, "installedBytes": 9_000_000_000, "entries": 226000]
        let manifest = try SteamOSManifest.decode(JSONSerialization.data(withJSONObject: valid))
        precondition(manifest.requiredFreeBytes == 11_536_870_912)
        let invalid: [(String, Any)] = [("schema", 2), ("architecture", "x86_64"), ("version", "3.7"),
            ("sourceImageSHA256", String(repeating: "0", count: 64)), ("archiveSHA256", "../../archive"),
            ("archiveBytes", -1), ("archiveBytes", Int64.max), ("installedBytes", Int64.max),
            ("entries", 0), ("entries", 500001)]
        for (key, value) in invalid {
            var input = valid; input[key] = value
            do { _ = try SteamOSManifest.decode(JSONSerialization.data(withJSONObject: input)); fatalError("Accepted invalid \(key)") }
            catch { }
        }
        do { _ = try SteamOSManifest.decode(Data(repeating: 0, count: 65537)); fatalError("Accepted oversized manifest") }
        catch { }
        print("STEAMOS_MANIFEST: PASS (identity, digest syntax, size bounds, overflow and schema)")
    }
}
