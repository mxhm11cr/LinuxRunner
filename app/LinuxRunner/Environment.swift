import Foundation

struct LinuxApplication: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var command: String
    var arguments: [String] = []
    var genericName: String?
    var comment: String?
    var categories: [String]?
    var icon: String?
    // Optional additions preserve decoding of existing schema-1 catalogs.
    var environment: [String: String]?
    var displayMode: LinuxDisplayMode?
    var workingDirectory = "/root"

    // Literal argv identity persists across discovery refreshes without relying
    // on transient UUIDs or shell-formatted commands.
    var launchKey: String {
        (try? JSONEncoder().encode([command, workingDirectory] + arguments).base64EncodedString()) ?? command
    }

    func validate() throws {
        let env = environment ?? [:]
        guard env.reduce(0, { $0 + $1.key.utf8.count + $1.value.utf8.count + 2 }) <= 4096,
              env.allSatisfy({ key, value in
                  key.range(of: "^[A-Za-z_][A-Za-z0-9_]*$", options: .regularExpression) != nil &&
                  !value.contains("\0") && !["DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR"].contains(key) && key.utf8.count + value.utf8.count <= 4096
              }),
              !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              command.hasPrefix("/"), workingDirectory.hasPrefix("/"),
              !command.contains("\0"), !workingDirectory.contains("\0"),
              !arguments.contains(where: { $0.contains("\0") }),
              ([command] + arguments).reduce(1, { $0 + $1.utf8.count + 1 }) <= 4096 else {
            throw EnvironmentError.invalidCommand
        }
    }
}

struct LinuxEnvironment: Codable, Identifiable {
    var id = UUID()
    var name: String
    var distribution = "Alpine Linux"
    var distributionID: String?
    var packageManager: String?
    var environmentVariables: [String: String]?
    var guiConfiguration: LinuxGUIConfiguration?
    var installationState: String?
    var lastBootResult: String?
    var storageBytes: Int64?
    var guiAvailability: String?
    var desktopAvailability: String?
    var diagnosticHistory: [String]?
    var architecture = "arm64"
    // Relative to Roots' container. Never persist an absolute sandbox path.
    var rootfsName: String
    var defaultShell: String?
    var homePath = "/root"
    var rootfsVersion = "3.24.2"
    var createdAt = Date()
    var lastBootedAt: Date?
    var runtimeConfiguration = ["backend": "asbestos", "jit": "false"]
    var applications = [
        LinuxApplication(name: "Shell", command: "/bin/sh"),
        LinuxApplication(name: "System information", command: "/bin/uname", arguments: ["-a"])
    ]
    // State and storage are measured at runtime, never restored as Running.
}

enum LinuxDisplayMode: String, Codable { case terminal, application }
struct LinuxPackageRecord: Identifiable {
    var id: String { name }
    let name, version, architecture, status, summary: String
    static func parse(_ text: String) -> [Self] {
        var seen = Set<String>()
        return text.components(separatedBy: .newlines).prefix(2000).compactMap { line in
            let fields = line.components(separatedBy: "\t")
            guard fields.count >= 4, PackageOperation.validName(fields[0]), seen.insert(fields[0]).inserted else { return nil }
            return Self(name: fields[0], version: fields[1], architecture: fields[2], status: fields[3], summary: fields.count > 4 ? fields[4] : "")
        }
    }
}
enum PackageOperation {
    static func validName(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 128 && value.range(of: "^[A-Za-z0-9][A-Za-z0-9+._:-]*$", options: .regularExpression) != nil
    }
    static func command(_ operation: String, value: String = "") throws -> [String] {
        let named = ["search", "details", "install", "upgrade", "remove", "plan-install", "plan-remove", "plan-upgrade", "apply-install", "apply-remove", "apply-upgrade"]
        guard (["installed", "updates", "update", "diagnose", "configure", "dependencies", "repair", "catalog"] + named).contains(operation),
              !named.contains(operation) || validName(value) else { throw EnvironmentError.invalidCommand }
        return ["/bin/sh", "/home/mobile/Downloads/.linuxrunner/gui/package-manager.sh", operation, value]
    }
}
struct LinuxGUIConfiguration: Codable, Equatable {
    var backend = "xvfb-rfb"
    var width = 640
    var height = 480
    var keyboardMode = "xkb"
}
// Capabilities are recorded from tests; installing a distribution does not
// imply GUI or desktop acceptance. Existing catalogs remain decodable.

enum EnvironmentError: LocalizedError {
    case invalidCommand, invalidMetadata
    var errorDescription: String? {
        switch self {
        case .invalidCommand: return "Use an absolute guest executable and directory. Arguments must fit within 4 KiB and contain no NUL characters."
        case .invalidMetadata: return "Environment metadata is invalid. The existing file has been preserved."
        }
    }
}

struct EnvironmentCatalog: Codable {
    var schemaVersion = 1
    var environments: [LinuxEnvironment]
    static func read(_ url: URL) throws -> Self {
        let value = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        guard value.schemaVersion == 1,
              Set(value.environments.map(\.id)).count == value.environments.count,
              Set(value.environments.map(\.rootfsName)).count == value.environments.count,
              value.environments.allSatisfy({ validRootName($0.rootfsName) }) else {
            throw EnvironmentError.invalidMetadata
        }
        for environment in value.environments {
            for application in environment.applications { try application.validate() }
        }
        return value
    }
    func write(_ url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: url, options: .atomic)
    }
    static func validRootName(_ name: String) -> Bool {
        !name.isEmpty && name != "." && name != ".." &&
        !name.contains("/") && !name.contains("\\") && !name.contains("\0")
    }
}

// Conservative Desktop Entry Exec parser. No shell evaluation, URL/file
// selection or D-Bus activation is implied. Localized labels follow the iOS
// display language; this does not silently change the guest application's LANG.
// Spec: https://specifications.freedesktop.org/desktop-entry/latest/exec-variables.html
@objc(LRDesktopEntry) final class DesktopEntry: NSObject {
    @objc static func configuration(_ contents: String, path: String) -> [String: Any]? {
        guard contents.utf8.count <= 65536, !contents.contains("\0") else { return nil }
        var values: [String: String] = [:], inEntry = false
        for raw in contents.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("[") { inEntry = line == "[Desktop Entry]"; continue }
            guard inEntry, !line.hasPrefix("#"), let equal = line.firstIndex(of: "=") else { continue }
            let key = String(line[..<equal])
            guard values[key] == nil else { return nil }
            values[key] = String(line[line.index(after: equal)...])
        }
        func localized(_ key: String) -> String? {
            for preference in Locale.preferredLanguages {
                let locale = preference.replacingOccurrences(of: "-", with: "_")
                for suffix in [locale, String(locale.split(separator: "_").first ?? "")] {
                    if let label = values[key + "[" + suffix + "]"] { return stringValue(label) }
                }
            }
            return values[key].map(stringValue)
        }
        guard values["Type"] == "Application", values["Hidden"] != "true", values["NoDisplay"] != "true",
              let name = localized("Name"), !name.isEmpty, let exec = values["Exec"], let words = arguments(exec),
              let executable = words.first, !executable.isEmpty, !executable.contains("="), !executable.contains("%") else { return nil }
        guard words.filter({ ["%f", "%F", "%u", "%U"].contains($0) }).count <= 1 else { return nil }
        var args: [String] = []
        for word in words.dropFirst() {
            switch word {
            case "%f", "%F", "%u", "%U", "%d", "%D", "%n", "%N", "%v", "%m": continue
            case "%i": if let icon = values["Icon"], !icon.isEmpty { args += ["--icon", icon] }
            case "%c": args.append(name)
            case "%k": args.append(path)
            default:
                var result = "", i = word.startIndex
                while i < word.endIndex {
                    let char = word[i]; i = word.index(after: i)
                    if char == "%" {
                        guard i < word.endIndex, word[i] == "%" else { return nil }
                        i = word.index(after: i); result.append("%")
                    } else { result.append(char) }
                }
                args.append(result)
            }
        }
        let directory = values["Path"].map(stringValue) ?? "/root"
        guard directory.hasPrefix("/"), ([name, executable, directory] + args).reduce(0, { $0 + $1.utf8.count + 1 }) <= 4096 else { return nil }
        return ["name": name, "executable": executable, "arguments": args, "directory": directory,
                "terminal": values["Terminal"] == "true", "tryExec": values["TryExec"].map(stringValue) ?? "", "desktopFile": path,
                "genericName": localized("GenericName") ?? "", "comment": localized("Comment") ?? "",
                "icon": values["Icon"].map(stringValue) ?? "", "categories": (values["Categories"] ?? "").split(separator: ";").map(String.init),
                "dbusActivatable": values["DBusActivatable"] == "true"]
    }
    private static func stringValue(_ input: String) -> String {
        var output = "", escape = false
        for c in input {
            if escape { output += ["s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"][String(c)] ?? "\\" + String(c); escape = false }
            else if c == "\\" { escape = true }
            else { output.append(c) }
        }
        if escape { output.append("\\") }; return output
    }
    private static func arguments(_ input: String) -> [String]? {
        // Desktop string escaping is applied before Exec quoting rules.
        let source = Array(input); var decoded: [Character] = [], index = 0
        while index < source.count {
            let c = source[index]; index += 1
            if c == "\\", index < source.count {
                let next = source[index]; index += 1
                switch next {
                case "s": decoded.append(" "); case "n": decoded.append("\n")
                case "t": decoded.append("\t"); case "r": decoded.append("\r")
                case "\\": decoded.append("\\")
                default: decoded += ["\\", next]
                }
            } else { decoded.append(c) }
        }
        var words: [String] = [], word = "", quoted = false, started = false; index = 0
        let reserved = Set("'<>~|&;$*?#()`")
        while index < decoded.count {
            let c = decoded[index]; index += 1
            if c == "\"" { quoted.toggle(); started = true; continue }
            if quoted, c == "\\" {
                guard index < decoded.count, "\"`$\\".contains(decoded[index]) else { return nil }
                word.append(decoded[index]); index += 1; started = true; continue
            }
            if !quoted, c.isWhitespace {
                if started { words.append(word); word = ""; started = false }; continue
            }
            if !quoted, reserved.contains(c) || c == "\\" { return nil }
            // Field codes inside quotes have undefined behavior; refuse them.
            if quoted, c == "%" { return nil }
            word.append(c); started = true
        }
        guard !quoted else { return nil }
        if started { words.append(word) }
        return words.isEmpty ? nil : words
    }
}
