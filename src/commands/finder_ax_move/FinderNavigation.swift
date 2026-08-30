import AppKit
import ApplicationServices
import Foundation

// Retargeting a Finder window through Accessibility, used when Apple Events are
// unavailable. Drives Finder's own "Go to Folder" sheet, so jumping to a folder
// never depends on Automation permission.

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
