import AppKit
import ApplicationServices
import Darwin
import Foundation

let axMenuItemModifierShift = 1 << 0
let axMenuItemModifierOption = 1 << 1
let axMenuItemModifierControl = 1 << 2
let axMenuItemModifierNoCommand = 1 << 3

func notifyKeystroke(
    keyCode: UInt16,
    modifiers: UInt64 = 0,
    suppressKeyCode: Int = -1,
    suppressModifiers: UInt64 = 0
) {
    let path = "/tmp/keystroke-finer-\(getuid()).sock"
    guard path.utf8.count < MemoryLayout.size(ofValue: sockaddr_un().sun_path) else { return }
    guard access(path, F_OK) == 0 else { return }

    let socketDescriptor = socket(AF_UNIX, SOCK_DGRAM, 0)
    guard socketDescriptor >= 0 else { return }
    defer { close(socketDescriptor) }

    var address = sockaddr_un()
    address.sun_family = sa_family_t(AF_UNIX)
    withUnsafeMutableBytes(of: &address.sun_path) { buffer in
        buffer.initializeMemory(as: UInt8.self, repeating: 0)
        _ = path.utf8CString.withUnsafeBytes { source in
            source.copyBytes(to: buffer)
        }
    }
    let addressLength = socklen_t(
        MemoryLayout.offset(of: \sockaddr_un.sun_path)! + path.utf8.count + 1
    )
    let message = "\(keyCode) \(modifiers) \(suppressKeyCode) \(suppressModifiers)\n"
    message.withCString { bytes in
        withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                _ = sendto(
                    socketDescriptor,
                    bytes,
                    strlen(bytes),
                    MSG_DONTWAIT,
                    socketAddress,
                    addressLength
                )
            }
        }
    }
}

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
            return "Usage: finder_ax_move <down|up|visual-down|visual-up> <1...99> | jump-to <directory> | reveal-file <file> | <down-wrap|up-wrap|first|last|visual-start|visual-first|visual-last|toggle-mark|copy-absolute|copy-directory|copy-filename|copy-stem|new-folder-current-level> | <hold-start|hold-repeat> <down|up>"
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

enum Direction: String {
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

enum Command {
    case move(Direction, Int, wrapping: Bool)
    case visualStart
    case visualMove(Direction, Int)
    case visualEdge(Direction)
    case holdStart(Direction)
    case holdRepeat(Direction)
    case toggleMark
    case copy(CopyMode)
    case newFolderAtSelectionLevel
    case jumpTo(String)
    case revealFile(String)
}
