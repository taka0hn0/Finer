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

        if navigateDirectly(to: path) {
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

        if revealDirectly(file: path) {
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

    private static func navigateDirectly(to path: String) -> Bool {
        let environment = ProcessInfo.processInfo.environment
        let executablePath = environment["FINER_OSASCRIPT_PATH"]
            ?? "/usr/bin/osascript"
        guard FileManager.default.isExecutableFile(atPath: executablePath) else {
            return false
        }

        let script = """
        on run argv
            set destinationPath to item 1 of argv
            tell application "/System/Library/CoreServices/Finder.app"
                if (count of Finder windows) is 0 then error "No Finder window"
                set destinationFolder to POSIX file destinationPath as alias
                set target of front Finder window to destinationFolder
                activate
            end tell
        end run
        """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executablePath)
        process.arguments = ["-e", script, path]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
        }
    }

    private static func revealDirectly(file path: String) -> Bool {
        let environment = ProcessInfo.processInfo.environment
        let executablePath = environment["FINER_OSASCRIPT_PATH"]
            ?? "/usr/bin/osascript"
        guard FileManager.default.isExecutableFile(atPath: executablePath) else {
            return false
        }

        let parentPath = URL(fileURLWithPath: path)
            .deletingLastPathComponent().path
        let script = """
        on run argv
            set destinationPath to item 1 of argv
            set selectedPath to item 2 of argv
            tell application "/System/Library/CoreServices/Finder.app"
                if (count of Finder windows) is 0 then error "No Finder window"
                set destinationFolder to POSIX file destinationPath as alias
                set selectedFile to POSIX file selectedPath as alias
                set target of front Finder window to destinationFolder
                set selection to {selectedFile}
                activate
            end tell
        end run
        """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executablePath)
        process.arguments = ["-e", script, parentPath, path]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
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
