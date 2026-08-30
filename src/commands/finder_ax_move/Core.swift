import AppKit
import ApplicationServices
import Foundation

let axMenuItemModifierShift = 1 << 0
let axMenuItemModifierOption = 1 << 1
let axMenuItemModifierControl = 1 << 2
let axMenuItemModifierNoCommand = 1 << 3

enum MoveError: Error, CustomStringConvertible {
    case invalidArguments
    case finderIsNotFrontmost
    case accessibilityUnavailable
    case missingAttribute(String)
    case unsupportedRole(String)
    case emptyContainer
    case missingSelectionURL
    case menuAction
    case renameAction
    case jumpMenuAction
    case jumpSheet
    case jumpConfirmation
    case folderCreation(String)
    case createdFolderUnavailable
    case stateFile(String)
    case setAttribute(String, AXError)

    var description: String {
        switch self {
        case .invalidArguments:
            return "Usage: finder_ax_move <visual-down|visual-up> <1...99> | jump-to <directory> | reveal-file <file> | <visual-start|visual-first|visual-last|toggle-mark|copy-absolute|copy-directory|copy-filename|copy-stem|new-folder-current-level>"
        case .finderIsNotFrontmost:
            return "Finder is not frontmost"
        case .accessibilityUnavailable:
            return "Accessibility access is unavailable"
        case let .missingAttribute(name):
            return "Missing accessibility attribute: \(name)"
        case let .unsupportedRole(role):
            return "Unsupported focused role: \(role)"
        case .emptyContainer:
            return "The focused Finder container is empty"
        case .missingSelectionURL:
            return "Could not read the selected Finder item URL"
        case .menuAction:
            return "Could not invoke Finder's New Folder menu action"
        case .renameAction:
            return "Could not invoke Finder's Rename menu action"
        case .jumpMenuAction:
            return "Could not invoke Finder's Go to Folder menu action"
        case .jumpSheet:
            return "Finder did not publish the Go to Folder sheet"
        case .jumpConfirmation:
            return "Finder did not navigate to the requested item"
        case let .folderCreation(message):
            return "Could not create a new folder: \(message)"
        case .createdFolderUnavailable:
            return "Finder did not publish the newly created folder"
        case let .stateFile(message):
            return "Could not update Finder mark state: \(message)"
        case let .setAttribute(name, error):
            return "Could not set \(name): \(error.rawValue)"
        }
    }
}

// Visual Mode directions. The string spellings this enum used to carry existed
// for the Normal Mode movement arguments, which the C worker now owns.
enum Direction {
    case down
    case up
    case first
    case last
}

enum CopyMode: String {
    case absolute = "copy-absolute"
    case directory = "copy-directory"
    case filename = "copy-filename"
    case stem = "copy-stem"
}

// Normal Mode movement is not here: single presses, holds, counts, and gg/G are
// handled by the transient C worker (see docs/FINDER_VIM_SPEC.md, 2026-07-30).
// This helper owns Visual Mode, marks, clipboard, and folder creation.
enum Command {
    case visualStart
    case visualMove(Direction, Int)
    case visualEdge(Direction)
    case toggleMark
    case copy(CopyMode)
    case newFolderAtSelectionLevel
    case jumpTo(String)
    case revealFile(String)
}
