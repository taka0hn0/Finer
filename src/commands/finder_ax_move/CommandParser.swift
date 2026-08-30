import Foundation

// The accepted command line for finder_ax_move, in one place. Karabiner rules
// are generated against this surface, so the accepted spellings and their
// bounds are easier to review separately from what each command then does.
func parseCommand(_ arguments: [String]) throws -> Command {
    if arguments.count == 2,
       arguments[0] == "visual-down" || arguments[0] == "visual-up",
       let requestedCount = Int(arguments[1]),
       (1...99).contains(requestedCount) {
        return .visualMove(
            arguments[0] == "visual-down" ? .down : .up,
            requestedCount
        )
    }
    if arguments.count == 1, arguments[0] == "visual-start" {
        return .visualStart
    }
    if arguments.count == 1, arguments[0] == "visual-first" {
        return .visualEdge(.first)
    }
    if arguments.count == 1, arguments[0] == "visual-last" {
        return .visualEdge(.last)
    }
    if arguments.count == 1, arguments[0] == "toggle-mark" {
        return .toggleMark
    }
    if arguments.count == 1, arguments[0] == "new-folder-current-level" {
        return .newFolderAtSelectionLevel
    }
    if arguments.count == 2, arguments[0] == "jump-to" {
        return .jumpTo(arguments[1])
    }
    if arguments.count == 2, arguments[0] == "reveal-file" {
        return .revealFile(arguments[1])
    }
    if arguments.count == 1, let mode = CopyMode(rawValue: arguments[0]) {
        return .copy(mode)
    }
    throw MoveError.invalidArguments
}

// FR-DIAG-004: report the key the user actually pressed while the demo overlay
// is listening. Commands that reach Finder through a converted key report from
// the rule instead, so only the ones that finish inside this helper appear here.
func notifyConsumedKey(for command: Command) {
    switch command {
    case .visualStart:
        KeystrokeSocket.notify(keyCode: 9)
    case .copy(.absolute):
        KeystrokeSocket.notify(keyCode: 8)
    case .copy(.directory):
        KeystrokeSocket.notify(keyCode: 2)
    case .copy(.filename):
        KeystrokeSocket.notify(keyCode: 3)
    case .copy(.stem):
        KeystrokeSocket.notify(keyCode: 45)
    default:
        break
    }
}
