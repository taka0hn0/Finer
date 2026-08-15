import AppKit
import Darwin
import Foundation

@main
enum FinerJumpCommand {
    static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        if let exitCode = runHeadless(arguments: arguments) {
            exit(exitCode)
        }

        notifyKeystrokeJumpKey()

        let app = NSApplication.shared
        let controller = JumpController()
        app.delegate = controller
        app.setActivationPolicy(.accessory)
        app.run()
    }
}
