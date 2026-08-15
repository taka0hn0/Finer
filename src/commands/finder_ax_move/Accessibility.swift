import AppKit
import ApplicationServices
import Foundation

func attribute(_ element: AXUIElement, _ name: String) throws -> CFTypeRef {
    var value: CFTypeRef?
    let error = AXUIElementCopyAttributeValue(element, name as CFString, &value)
    guard error == .success, let value else {
        throw MoveError.missingAttribute(name)
    }
    return value
}

func elements(_ element: AXUIElement, _ name: String) -> [AXUIElement] {
    guard let value = try? attribute(element, name) else { return [] }
    return value as? [AXUIElement] ?? []
}

func stringAttribute(_ element: AXUIElement, _ name: String) -> String? {
    try? attribute(element, name) as? String
}

func boolAttribute(_ element: AXUIElement, _ name: String) -> Bool {
    (try? attribute(element, name) as? Bool) ?? false
}

func urlAttribute(_ element: AXUIElement, depth: Int = 0) -> URL? {
    if let value = try? attribute(element, kAXURLAttribute) {
        if CFGetTypeID(value) == CFURLGetTypeID() {
            return value as? URL
        }
        if let urlString = value as? String {
            return URL(string: urlString) ?? URL(fileURLWithPath: urlString)
        }
    }

    guard depth < 3 else { return nil }
    for child in elements(element, kAXChildrenAttribute) {
        if let url = urlAttribute(child, depth: depth + 1) {
            return url
        }
    }
    return nil
}

func setAttribute(_ element: AXUIElement, _ name: String, _ value: CFTypeRef) throws {
    let error = AXUIElementSetAttributeValue(element, name as CFString, value)
    guard error == .success else {
        throw MoveError.setAttribute(name, error)
    }
}

func integerAttribute(_ element: AXUIElement, _ name: String) -> Int? {
    guard let value = try? attribute(element, name),
          CFGetTypeID(value) == CFNumberGetTypeID() else {
        return nil
    }
    return (value as? NSNumber)?.intValue
}

func isNewFolderMenuItem(_ element: AXUIElement) -> Bool {
    guard stringAttribute(element, kAXRoleAttribute) == kAXMenuItemRole else {
        return false
    }

    let commandCharacter = stringAttribute(
        element,
        kAXMenuItemCmdCharAttribute
    )?.uppercased()
    let commandVirtualKey = integerAttribute(
        element,
        kAXMenuItemCmdVirtualKeyAttribute
    )
    guard commandCharacter == "N" || commandVirtualKey == 45 else {
        return false
    }

    guard let modifiers = integerAttribute(
        element,
        kAXMenuItemCmdModifiersAttribute
    ) else {
        return false
    }
    return modifiers & axMenuItemModifierShift != 0
        && modifiers & (
            axMenuItemModifierOption
                | axMenuItemModifierControl
                | axMenuItemModifierNoCommand
        ) == 0
}

func newFolderMenuItem(
    in element: AXUIElement,
    depth: Int = 0
) -> AXUIElement? {
    if isNewFolderMenuItem(element) {
        return element
    }
    guard depth < 5 else { return nil }
    for child in elements(element, kAXChildrenAttribute) {
        if let result = newFolderMenuItem(in: child, depth: depth + 1) {
            return result
        }
    }
    return nil
}

func finderNewFolderMenuItem(
    finderElement: AXUIElement
) -> AXUIElement? {
    guard let menuBarValue = try? attribute(
              finderElement,
              kAXMenuBarAttribute
          ),
          CFGetTypeID(menuBarValue) == AXUIElementGetTypeID() else {
        return nil
    }
    return newFolderMenuItem(in: menuBarValue as! AXUIElement)
}

func performNewFolderMenuAction(
    finderElement: AXUIElement
) throws {
    guard let menuItem = finderNewFolderMenuItem(
              finderElement: finderElement
          ),
          AXUIElementPerformAction(
              menuItem,
              kAXPressAction as CFString
          ) == .success else {
        throw MoveError.menuAction
    }
}

func renameMenuItem(
    in element: AXUIElement,
    title: String,
    depth: Int = 0
) -> AXUIElement? {
    if stringAttribute(element, kAXRoleAttribute) == kAXMenuItemRole,
       stringAttribute(element, kAXTitleAttribute) == title {
        return element
    }
    guard depth < 5 else { return nil }
    for child in elements(element, kAXChildrenAttribute) {
        if let result = renameMenuItem(
            in: child,
            title: title,
            depth: depth + 1
        ) {
            return result
        }
    }
    return nil
}

func performRenameMenuAction(
    finderElement: AXUIElement
) throws {
    let finderBundle = Bundle(
        path: "/System/Library/CoreServices/Finder.app"
    )
    let title = finderBundle?.localizedString(
        forKey: "OPI-Bm-bCw.title",
        value: "Rename",
        table: "MenuBar"
    ) ?? "Rename"

    guard let menuBarValue = try? attribute(
              finderElement,
              kAXMenuBarAttribute
          ),
          CFGetTypeID(menuBarValue) == AXUIElementGetTypeID(),
          let menuItem = renameMenuItem(
              in: menuBarValue as! AXUIElement,
              title: title
          ),
          AXUIElementPerformAction(
              menuItem,
              kAXPressAction as CFString
          ) == .success else {
        throw MoveError.renameAction
    }
}

func isGoToFolderMenuItem(_ element: AXUIElement) -> Bool {
    guard stringAttribute(element, kAXRoleAttribute) == kAXMenuItemRole else {
        return false
    }

    let commandCharacter = stringAttribute(
        element,
        kAXMenuItemCmdCharAttribute
    )?.uppercased()
    let commandVirtualKey = integerAttribute(
        element,
        kAXMenuItemCmdVirtualKeyAttribute
    )
    guard commandCharacter == "G" || commandVirtualKey == 5 else {
        return false
    }

    guard let modifiers = integerAttribute(
        element,
        kAXMenuItemCmdModifiersAttribute
    ) else {
        return false
    }
    return modifiers & axMenuItemModifierShift != 0
        && modifiers & (
            axMenuItemModifierOption
                | axMenuItemModifierControl
                | axMenuItemModifierNoCommand
        ) == 0
}

func goToFolderMenuItem(
    in element: AXUIElement,
    depth: Int = 0
) -> AXUIElement? {
    if isGoToFolderMenuItem(element) {
        return element
    }
    guard depth < 5 else { return nil }
    for child in elements(element, kAXChildrenAttribute) {
        if let result = goToFolderMenuItem(in: child, depth: depth + 1) {
            return result
        }
    }
    return nil
}

func descendantTextField(
    in element: AXUIElement,
    depth: Int = 0
) -> AXUIElement? {
    if stringAttribute(element, kAXRoleAttribute) == kAXTextFieldRole {
        return element
    }
    guard depth < 6 else { return nil }
    for child in elements(element, kAXChildrenAttribute) {
        if let result = descendantTextField(in: child, depth: depth + 1) {
            return result
        }
    }
    return nil
}

func ancestor(
    from element: AXUIElement,
    role expectedRole: String
) -> AXUIElement? {
    var current = element
    for _ in 0..<8 {
        if stringAttribute(current, kAXRoleAttribute) == expectedRole {
            return current
        }
        guard let parentValue = try? attribute(
                  current,
                  kAXParentAttribute
              ),
              CFGetTypeID(parentValue) == AXUIElementGetTypeID() else {
            return nil
        }
        current = parentValue as! AXUIElement
    }
    return nil
}

func focusedSheet(
    finderElement: AXUIElement,
    frontWindow: AXUIElement
) -> AXUIElement? {
    if let focusedWindowValue = try? attribute(
        finderElement,
        kAXFocusedWindowAttribute
    ), CFGetTypeID(focusedWindowValue) == AXUIElementGetTypeID() {
        let focusedWindow = focusedWindowValue as! AXUIElement
        if stringAttribute(focusedWindow, kAXRoleAttribute) == "AXSheet" {
            return focusedWindow
        }
    }

    if let focusedValue = try? attribute(
        finderElement,
        kAXFocusedUIElementAttribute
    ), CFGetTypeID(focusedValue) == AXUIElementGetTypeID(),
       let sheet = ancestor(
           from: focusedValue as! AXUIElement,
           role: "AXSheet"
       ) {
        return sheet
    }
    return elements(frontWindow, "AXSheets").first
}

func waitUntil(
    timeout: TimeInterval,
    interval: TimeInterval = 0.01,
    condition: () -> Bool
) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    repeat {
        if condition() {
            return true
        }
        Thread.sleep(forTimeInterval: interval)
    } while Date() < deadline
    return condition()
}

func postReturn(to processIdentifier: pid_t) -> Bool {
    guard let source = CGEventSource(stateID: .hidSystemState),
          let keyDown = CGEvent(
              keyboardEventSource: source,
              virtualKey: 36,
              keyDown: true
          ),
          let keyUp = CGEvent(
              keyboardEventSource: source,
              virtualKey: 36,
              keyDown: false
          ) else {
        return false
    }
    keyDown.postToPid(processIdentifier)
    keyUp.postToPid(processIdentifier)
    return true
}

func jumpToFolderDirectly(_ path: String) -> Bool {
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
    process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
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

func revealFileDirectly(_ path: String) -> Bool {
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
    process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
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

func jumpToFolder(
    _ path: String,
    finder: NSRunningApplication,
    finderElement: AXUIElement,
    expectsDirectory: Bool = true
) throws -> Int {
    var isDirectory: ObjCBool = false
    guard FileManager.default.fileExists(
        atPath: path,
        isDirectory: &isDirectory
    ), isDirectory.boolValue == expectsDirectory else {
        throw MoveError.invalidArguments
    }

    guard let windowValue = try? attribute(
              finderElement,
              kAXFocusedWindowAttribute
          ),
          CFGetTypeID(windowValue) == AXUIElementGetTypeID() else {
        throw MoveError.jumpSheet
    }
    let frontWindow = windowValue as! AXUIElement

    _ = finder.activate()
    guard waitUntil(timeout: 0.75, interval: 0.005, condition: {
        finder.isActive
    }) else {
        throw MoveError.finderIsNotFrontmost
    }

    guard let menuBarValue = try? attribute(
              finderElement,
              kAXMenuBarAttribute
          ),
          CFGetTypeID(menuBarValue) == AXUIElementGetTypeID(),
          let menuItem = goToFolderMenuItem(
              in: menuBarValue as! AXUIElement
          ),
          AXUIElementPerformAction(
              menuItem,
              kAXPressAction as CFString
          ) == .success else {
        throw MoveError.jumpMenuAction
    }

    var sheet: AXUIElement?
    var pathField: AXUIElement?
    guard waitUntil(timeout: 1.0, condition: {
        guard let candidate = focusedSheet(
            finderElement: finderElement,
            frontWindow: frontWindow
        ),
        let field = descendantTextField(in: candidate) else {
            return false
        }
        sheet = candidate
        pathField = field
        return true
    }), let sheet, let pathField else {
        throw MoveError.jumpSheet
    }

    try setAttribute(
        pathField,
        kAXValueAttribute,
        path as CFString
    )
    guard finder.isActive, postReturn(to: finder.processIdentifier) else {
        throw MoveError.jumpConfirmation
    }

    guard waitUntil(timeout: 1.5, condition: {
        guard let currentSheet = focusedSheet(
            finderElement: finderElement,
            frontWindow: frontWindow
        ) else {
            return true
        }
        return !CFEqual(currentSheet, sheet)
    }) else {
        throw MoveError.jumpConfirmation
    }
    return 0
}
