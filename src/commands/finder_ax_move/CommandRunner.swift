import AppKit
import ApplicationServices
import Foundation

func run() throws -> Int {
    let arguments = Array(CommandLine.arguments.dropFirst())
    let command: Command
    if arguments.count == 2,
       arguments[0] == "visual-down" || arguments[0] == "visual-up",
       let requestedCount = Int(arguments[1]),
       (1...99).contains(requestedCount) {
        command = .visualMove(
            arguments[0] == "visual-down" ? .down : .up,
            requestedCount
        )
    } else if arguments.count == 1, arguments[0] == "visual-start" {
        command = .visualStart
    } else if arguments.count == 1, arguments[0] == "visual-first" {
        command = .visualEdge(.first)
    } else if arguments.count == 1, arguments[0] == "visual-last" {
        command = .visualEdge(.last)
    } else if arguments.count == 1, arguments[0] == "toggle-mark" {
        command = .toggleMark
    } else if arguments.count == 1, arguments[0] == "new-folder-current-level" {
        command = .newFolderAtSelectionLevel
    } else if arguments.count == 2, arguments[0] == "jump-to" {
        command = .jumpTo(arguments[1])
    } else if arguments.count == 2, arguments[0] == "reveal-file" {
        command = .revealFile(arguments[1])
    } else if arguments.count == 1, let mode = CopyMode(rawValue: arguments[0]) {
        command = .copy(mode)
    } else if arguments.count == 1, arguments[0] == "down-wrap" {
        command = .move(.down, 1, wrapping: true)
    } else if arguments.count == 1, arguments[0] == "up-wrap" {
        command = .move(.up, 1, wrapping: true)
    } else if arguments.count == 2,
              arguments[0] == "hold-start" || arguments[0] == "hold-repeat",
              let direction = Direction(rawValue: arguments[1]),
              direction == .down || direction == .up {
        command = arguments[0] == "hold-start" ? .holdStart(direction) : .holdRepeat(direction)
    } else if let directionName = arguments.first,
              let direction = Direction(rawValue: directionName) {
        switch direction {
        case .down, .up:
            guard arguments.count == 2,
                  let requestedCount = Int(arguments[1]),
                  (1...99).contains(requestedCount) else {
                throw MoveError.invalidArguments
            }
            command = .move(direction, requestedCount, wrapping: false)
        case .first, .last:
            guard arguments.count == 1 else { throw MoveError.invalidArguments }
            command = .move(direction, 0, wrapping: false)
        }
    } else {
        throw MoveError.invalidArguments
    }

    switch command {
    case .visualStart:
        notifyKeystroke(keyCode: 9)
    case .copy(.absolute):
        notifyKeystroke(keyCode: 8)
    case .copy(.directory):
        notifyKeystroke(keyCode: 2)
    case .copy(.filename):
        notifyKeystroke(keyCode: 3)
    case .copy(.stem):
        notifyKeystroke(keyCode: 45)
    default:
        break
    }

    let repeatToken: String
    switch command {
    case let .holdRepeat(direction):
        repeatToken = readHoldToken(for: direction)
        if repeatToken.isEmpty { return 0 }
    default:
        repeatToken = ""
    }

    guard let finder = NSRunningApplication.runningApplications(
        withBundleIdentifier: "com.apple.finder"
    ).first else {
        throw MoveError.finderIsNotFrontmost
    }
    if case let .jumpTo(path) = command {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(
            atPath: path,
            isDirectory: &isDirectory
        ), isDirectory.boolValue else {
            throw MoveError.invalidArguments
        }
        if jumpToFolderDirectly(path) {
            return 0
        }
        guard AXIsProcessTrusted() else {
            throw MoveError.accessibilityUnavailable
        }
        let finderElement = AXUIElementCreateApplication(
            finder.processIdentifier
        )
        return try jumpToFolder(
            path,
            finder: finder,
            finderElement: finderElement
        )
    }
    if case let .revealFile(path) = command {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(
            atPath: path,
            isDirectory: &isDirectory
        ), !isDirectory.boolValue else {
            throw MoveError.invalidArguments
        }
        if revealFileDirectly(path) {
            return 0
        }
        guard AXIsProcessTrusted() else {
            throw MoveError.accessibilityUnavailable
        }
        let finderElement = AXUIElementCreateApplication(
            finder.processIdentifier
        )
        return try jumpToFolder(
            path,
            finder: finder,
            finderElement: finderElement,
            expectsDirectory: false
        )
    }
    guard AXIsProcessTrusted() else { throw MoveError.accessibilityUnavailable }
    guard finder.isActive else {
        throw MoveError.finderIsNotFrontmost
    }

    let finderElement = AXUIElementCreateApplication(finder.processIdentifier)
    let focusedElement = try attribute(finderElement, kAXFocusedUIElementAttribute) as! AXUIElement
    let (container, role) = try navigationContainer(from: focusedElement, in: finderElement)

    var shouldUseNavigationAnchor = true
    var movementCursorIndex: Int?
    func move(_ direction: Direction, count: Int, wrapping: Bool) throws -> Int {
        let useNavigationAnchor = shouldUseNavigationAnchor
        shouldUseNavigationAnchor = false
        let position: Int
        if role == kAXOutlineRole {
            position = try moveInOutline(
                container,
                direction: direction,
                count: count,
                wrapping: wrapping,
                useNavigationAnchor: useNavigationAnchor,
                currentIndexOverride: movementCursorIndex
            )
        } else {
            position = try moveInList(
                container,
                direction: direction,
                count: count,
                wrapping: wrapping,
                useNavigationAnchor: useNavigationAnchor,
                currentIndexOverride: movementCursorIndex
            )
        }
        movementCursorIndex = position - 1
        return position
    }

    switch command {
    case let .move(direction, count, wrapping):
        return try move(direction, count: count, wrapping: wrapping)
    case .visualStart:
        let items = try navigationItems(container, role: role)
        return try startVisualSelection(
            container: container,
            role: role,
            items: items
        )
    case let .visualMove(direction, count):
        let items = try navigationItems(container, role: role)
        return try extendVisualSelectionAfterPendingStart(
            container: container,
            role: role,
            items: items,
            direction: direction,
            count: count
        )
    case let .visualEdge(direction):
        let items = try navigationItems(container, role: role)
        return try extendVisualSelectionAfterPendingStart(
            container: container,
            role: role,
            items: items,
            direction: direction,
            count: 0
        )
    case let .holdStart(direction):
        let position = try move(direction, count: 1, wrapping: true)
        try startHold(for: direction)
        return position
    case let .holdRepeat(direction):
        var lastPosition = 0
        let deadline = Date().addingTimeInterval(30)
        while Date() < deadline
                && finder.isActive
                && readHoldToken(for: direction) == repeatToken {
            lastPosition = try move(direction, count: 1, wrapping: true)
            Thread.sleep(forTimeInterval: 0.005)
        }
        return lastPosition
    case .toggleMark:
        return try toggleCurrentMark(container, role: role)
    case let .copy(mode):
        return try copySelectionInfo(mode, container: container, role: role)
    case .newFolderAtSelectionLevel:
        return try createNewFolderAtSelectionLevel(
            finder: finder,
            finderElement: finderElement,
            container: container,
            role: role
        )
    case .jumpTo, .revealFile:
        throw MoveError.invalidArguments
    }
}
