import AppKit
import Foundation

enum FinderNavigator {
    static func navigate(to candidate: JumpCandidate) throws {
        switch candidate.kind {
        case .folder:
            try navigate(to: candidate.path)
        case .file:
            try reveal(file: candidate.path)
        }
    }

    static func navigate(to path: String) throws {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory),
              isDirectory.boolValue else {
            throw JumpError.navigationFailed("the folder no longer exists")
        }

        if FinderScript.navigate(toFolder: path) {
            return
        }

        try runHelper(arguments: ["jump-to", path])
    }

    static func reveal(file path: String) throws {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory),
              !isDirectory.boolValue else {
            throw JumpError.navigationFailed("the file no longer exists")
        }

        if FinderScript.reveal(file: path) {
            return
        }

        try runHelper(arguments: ["reveal-file", path])
    }

    private static func runHelper(arguments: [String]) throws {
        let process = Process()
        let stderr = Pipe()
        process.executableURL = try helperURL()
        process.arguments = arguments
        process.standardOutput = FileHandle.nullDevice
        process.standardError = stderr
        do {
            try process.run()
        } catch {
            throw JumpError.navigationFailed(error.localizedDescription)
        }
        process.waitUntilExit()

        guard process.terminationStatus == 0 else {
            let errorData = stderr.fileHandleForReading.readDataToEndOfFile()
            let message = String(data: errorData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw JumpError.navigationFailed(
                message?.isEmpty == false
                    ? message!
                    : "finder_ax_move exited with \(process.terminationStatus)"
            )
        }
    }

    private static func helperURL() throws -> URL {
        let environment = ProcessInfo.processInfo.environment
        if let override = environment["FINER_AX_MOVE_PATH"],
           FileManager.default.isExecutableFile(atPath: override) {
            return URL(fileURLWithPath: override)
        }

        let sibling = URL(fileURLWithPath: CommandLine.arguments[0])
            .deletingLastPathComponent()
            .appendingPathComponent("finder_ax_move")
        guard FileManager.default.isExecutableFile(atPath: sibling.path) else {
            throw JumpError.navigationFailed("finder_ax_move was not found")
        }
        return sibling
    }
}
