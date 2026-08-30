import AppKit
import ApplicationServices
import Foundation

// Retargets the front Finder window at `path`.
//
// Apple Events do the work whenever Finder answers them, which keeps the jump
// to one process hop. The Accessibility route through Finder's own "Go to
// Folder" sheet is the fallback, so the feature does not depend on Automation
// permission.
private func revealInFinder(
    _ path: String,
    expectsDirectory: Bool,
    finder: NSRunningApplication
) throws -> Int {
    var isDirectory: ObjCBool = false
    guard FileManager.default.fileExists(
        atPath: path,
        isDirectory: &isDirectory
    ), isDirectory.boolValue == expectsDirectory else {
        throw MoveError.invalidArguments
    }

    let handledByAppleEvent = expectsDirectory
        ? FinderScript.navigate(toFolder: path)
        : FinderScript.reveal(file: path)
    if handledByAppleEvent { return 0 }

    guard AXIsProcessTrusted() else {
        throw MoveError.accessibilityUnavailable
    }
    return try jumpToFolder(
        path,
        finder: finder,
        finderElement: AXUIElementCreateApplication(finder.processIdentifier),
        expectsDirectory: expectsDirectory
    )
}

func run() throws -> Int {
    let command = try parseCommand(Array(CommandLine.arguments.dropFirst()))
    notifyConsumedKey(for: command)

    guard let finder = NSRunningApplication.runningApplications(
        withBundleIdentifier: "com.apple.finder"
    ).first else {
        throw MoveError.finderIsNotFrontmost
    }

    // Jump and reveal retarget a window that does not have to be frontmost yet,
    // so they run before the frontmost check the remaining commands need.
    switch command {
    case let .jumpTo(path):
        return try revealInFinder(path, expectsDirectory: true, finder: finder)
    case let .revealFile(path):
        return try revealInFinder(path, expectsDirectory: false, finder: finder)
    default:
        break
    }

    guard AXIsProcessTrusted() else { throw MoveError.accessibilityUnavailable }
    guard finder.isActive else {
        throw MoveError.finderIsNotFrontmost
    }

    let finderElement = AXUIElementCreateApplication(finder.processIdentifier)
    let focusedElement = try attribute(finderElement, kAXFocusedUIElementAttribute) as! AXUIElement
    let (container, role) = try navigationContainer(from: focusedElement, in: finderElement)

    switch command {
    case .visualStart:
        return try startVisualSelection(
            container: container,
            role: role,
            items: try navigationItems(container, role: role)
        )
    case let .visualMove(direction, count):
        return try extendVisualSelectionAfterPendingStart(
            container: container,
            role: role,
            items: try navigationItems(container, role: role),
            direction: direction,
            count: count
        )
    case let .visualEdge(direction):
        return try extendVisualSelectionAfterPendingStart(
            container: container,
            role: role,
            items: try navigationItems(container, role: role),
            direction: direction,
            count: 0
        )
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
