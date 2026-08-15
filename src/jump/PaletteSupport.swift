import AppKit
import Foundation

final class JumpPanel: NSPanel {
    var handleNavigationKey: ((NSEvent) -> Bool)?
    var recordLifecycleEvent: ((String) -> Void)?

    override func sendEvent(_ event: NSEvent) {
        if event.type == .keyDown, handleNavigationKey?(event) == true {
            return
        }
        super.sendEvent(event)
    }

    override func cancelOperation(_ sender: Any?) {
        recordLifecycleEvent?("panel-cancel-operation")
        super.cancelOperation(sender)
    }

    override func close() {
        recordLifecycleEvent?("panel-close")
        super.close()
    }

    override func orderOut(_ sender: Any?) {
        recordLifecycleEvent?("panel-order-out")
        super.orderOut(sender)
    }
}

struct PaletteEscapeGate {
    static let markerKeyCode: UInt16 = 90 // F20 on macOS.
    private static let markerLifetime: TimeInterval = 0.1

    private var markerDeadline = -TimeInterval.infinity

    mutating func markPhysicalEscape(at time: TimeInterval) {
        markerDeadline = time + Self.markerLifetime
    }

    mutating func consumePhysicalEscape(at time: TimeInterval) -> Bool {
        let isMarked = time <= markerDeadline
        markerDeadline = -TimeInterval.infinity
        return isMarked
    }

    mutating func clear() {
        markerDeadline = -TimeInterval.infinity
    }
}

enum PaletteKeyCode {
    static let selectAll: UInt16 = 80 // F19 on macOS.
}

final class JumpDiagnostics {
    private let handle: FileHandle?

    init() {
        let temporaryDirectory = URL(fileURLWithPath: NSTemporaryDirectory())
        let enabledURL = temporaryDirectory
            .appendingPathComponent("finer-jump-diagnostics-enabled")
        guard FileManager.default.fileExists(atPath: enabledURL.path) else {
            handle = nil
            return
        }

        let logURL = temporaryDirectory
            .appendingPathComponent("finer-jump-events.log")
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        handle = try? FileHandle(forWritingTo: logURL)
        record("launch pid=\(ProcessInfo.processInfo.processIdentifier)")
    }

    deinit {
        try? handle?.close()
    }

    func record(_ message: String) {
        guard let handle else { return }
        let timestamp = String(
            format: "%.6f",
            ProcessInfo.processInfo.systemUptime
        )
        handle.write(Data("\(timestamp) \(message)\n".utf8))
        handle.synchronizeFile()
    }
}
