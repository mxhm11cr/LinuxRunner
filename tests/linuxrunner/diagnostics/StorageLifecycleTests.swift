// Host-only runtime seam. The runner compiles the production manager/scanner;
// only UIKit images and the guest runtime are replaced by disposable fixtures.
import Foundation
import Combine

struct UIImage {
    let size = CGSize(width: 1, height: 1)
    init?(data: Data) {}
}
enum LRRuntimeBridge {
    static var base = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    static func catalogURL() -> URL { base.appendingPathComponent("catalog.json") }
    static func rootNames() -> [String] { ["fixture"] }
    static func rootURL(_ name: String) -> URL { base.appendingPathComponent("root") }
    static func exchangeURL(_ name: String) -> URL { base.appendingPathComponent("exchange") }
    static func activeRootName() -> String { "fixture" }
    static func isRunning() -> Bool { false }
    static func beginAwake(_ reason: String) {}
    static func endAwake(_ reason: String) {}
    static func discoverApplications() -> [[String: String]] { [] }
    static func discoverGraphicalApplications() -> [[String: Any]] { [] }
    static func logLines() -> [String] { [] }
    static func createEnvironment(_ root: String, completion: (NSError?) -> Void) { completion(nil) }
}

// Same-file extensions can observe private task state without adding test APIs
// to the app or substituting a copy of the storage state machine.
extension EnvironmentManager {
    var testTask: Task<Void, Never>? { storageMeasurement }
    var testGeneration: UUID { storageGeneration }
    var testMeasuring: Bool { measuringStorage }
    func testComplete(_ sizes: [UUID: Int64], generation: UUID) { setStorage(sizes, generation: generation) }
}

@main struct StorageLifecycleTests {
    @MainActor static func main() async throws {
        let base = LRRuntimeBridge.base
        defer { try? FileManager.default.removeItem(at: base) }
        try FileManager.default.createDirectory(at: LRRuntimeBridge.rootURL("fixture"), withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: LRRuntimeBridge.exchangeURL("fixture"), withIntermediateDirectories: true)
        try Data(repeating: 1, count: 32768).write(to: LRRuntimeBridge.rootURL("fixture").appendingPathComponent("data"))
        var environment = LinuxEnvironment(name: "Fixture", rootfsName: "fixture")
        environment.storageBytes = 123
        try EnvironmentCatalog(environments: [environment]).write(LRRuntimeBridge.catalogURL())
        let id = environment.id
        let first = EnvironmentManager()
        let scene = EnvironmentManager()
        let initialTasks = [first.testTask!, scene.testTask!]
        let obsolete = first.testGeneration
        let localViewer = UUID(), externalViewer = UUID()

        // Pause every existing manager synchronously before a completion can
        // hop back to MainActor. Real scanner tasks are awaited, not timed.
        GraphicalActivity.shared.setActive(true, forSource: localViewer)
        precondition(!first.testMeasuring && !scene.testMeasuring)
        precondition(initialTasks.allSatisfy(\.isCancelled))
        for task in initialTasks { await task.value }
        precondition(first.storage[id] == 123 && scene.storage[id] == 123)
        let fileSharing = EnvironmentManager()
        precondition(fileSharing.testTask == nil && fileSharing.storage[id] == 123)
        fileSharing.refresh()
        precondition(fileSharing.testTask == nil)
        // A deliberate Storage settings measurement remains available while
        // automatic manager scans are paused.
        precondition(StorageUsage.bytes(LRRuntimeBridge.rootURL("fixture")) > 123)
        print("PASS: existing scenes cancel; new File Sharing manager stays paused with cached totals")

        // Model local dismissal with a retained external viewer, or closing one
        // of two scenes. Repeated activity updates must not emit false resumes.
        GraphicalActivity.shared.setActive(true, forSource: externalViewer)
        GraphicalActivity.shared.setActive(true, forSource: localViewer)
        GraphicalActivity.shared.setActive(false, forSource: localViewer)
        GraphicalActivity.shared.setActive(false, forSource: localViewer)
        precondition(GraphicalActivity.shared.active)
        precondition([first, scene, fileSharing].allSatisfy { $0.testTask == nil })
        GraphicalActivity.shared.setActive(false, forSource: externalViewer)
        precondition(!GraphicalActivity.shared.active)
        let resumedTasks = [first.testTask!, scene.testTask!, fileSharing.testTask!]
        first.testComplete([id: 999], generation: obsolete)
        precondition(first.storage[id] == 123 && first.testMeasuring)
        for task in resumedTasks { await task.value }
        let measured = StorageUsage.bytes(LRRuntimeBridge.rootURL("fixture"))
        precondition(measured > 123)
        precondition([first, scene, fileSharing].allSatisfy { $0.storage[id] == measured && !$0.testMeasuring && $0.testTask == nil })
        let persisted = try EnvironmentCatalog.read(LRRuntimeBridge.catalogURL())
        precondition(persisted.environments.first!.storageBytes == measured)
        print("PASS: last viewer release resumes all managers; obsolete completion cannot clear a new task; complete totals persist")

        GraphicalActivity.shared.setActive(true, forSource: localViewer)
        GraphicalActivity.shared.setActive(false, forSource: localViewer)
        precondition([first, scene, fileSharing].allSatisfy { $0.testTask == nil })
        print("PASS: recent completed totals retain the refresh interval")

        weak var released: EnvironmentManager?
        do { let temporary = EnvironmentManager(); released = temporary }
        precondition(released == nil)
        print("PASS: shared activity observation does not retain managers")
    }
}
