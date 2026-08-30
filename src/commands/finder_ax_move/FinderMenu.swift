import AppKit
import ApplicationServices
import Foundation

// Finder menu items, located by their keyboard shortcut rather than their title
// wherever possible, so the helper keeps working under any UI language
// (NFR-COMPAT-003). Only Rename has to be matched by a localized title, and
// that title is read from Finder's own bundle instead of being hardcoded.

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
