import Foundation

// Finder navigation through Apple Events.
//
// The AX command helper and the Jump palette both retarget the same front
// Finder window, so the scripts and the way they are executed live here instead
// of being spelled out once per executable. Both honour FINER_OSASCRIPT_PATH so
// a test can point them at a stub.
enum FinderScript {
    private static let navigateScript = """
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

    private static let revealScript = """
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

    static func navigate(toFolder path: String) -> Bool {
        run(navigateScript, arguments: [path])
    }

    static func reveal(file path: String) -> Bool {
        let parentPath = URL(fileURLWithPath: path)
            .deletingLastPathComponent().path
        return run(revealScript, arguments: [parentPath, path])
    }

    private static func executableURL() -> URL? {
        let path = ProcessInfo.processInfo.environment["FINER_OSASCRIPT_PATH"]
            ?? "/usr/bin/osascript"
        guard FileManager.default.isExecutableFile(atPath: path) else {
            return nil
        }
        return URL(fileURLWithPath: path)
    }

    private static func run(_ script: String, arguments: [String]) -> Bool {
        guard let executableURL = executableURL() else { return false }
        let process = Process()
        process.executableURL = executableURL
        process.arguments = ["-e", script] + arguments
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
}
