import AppKit
import ApplicationServices
import Foundation

func readMarkedPaths(from fileURL: URL) -> [String] {
    guard let contents = try? String(contentsOf: fileURL, encoding: .utf8) else { return [] }
    return contents.split(separator: "\n").map(String.init)
}

func writeMarkedPaths(_ paths: [String], to fileURL: URL) throws {
    do {
        try FileManager.default.createDirectory(
            at: fileURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        let contents = paths.isEmpty ? "" : paths.joined(separator: "\n") + "\n"
        try contents.write(to: fileURL, atomically: false, encoding: .utf8)
    } catch {
        throw MoveError.stateFile(error.localizedDescription)
    }
}

func setSelection(
    _ selection: [AXUIElement],
    in container: AXUIElement,
    role: String,
    allItems: [AXUIElement]
) throws {
    if role == kAXListRole || role == kAXGridRole {
        try setAttribute(container, kAXSelectedChildrenAttribute, selection as CFArray)
        _ = AXUIElementSetAttributeValue(container, kAXFocusedAttribute as CFString, kCFBooleanTrue)
        return
    }

    let selectedRowsError = AXUIElementSetAttributeValue(
        container,
        kAXSelectedRowsAttribute as CFString,
        selection as CFArray
    )
    if selectedRowsError == .success { return }

    let selectedIDs = Set(selection.map { CFHash($0) })
    for item in allItems {
        let value = selectedIDs.contains(CFHash(item)) ? kCFBooleanTrue! : kCFBooleanFalse!
        try setAttribute(item, kAXSelectedAttribute, value)
    }
}

func toggleCurrentMark(_ container: AXUIElement, role: String) throws -> Int {
    let items = try navigationItems(container, role: role)
    let selected = selectedItems(container, role: role, items: items)
    let previousAnchorIndex = takeNavigationAnchor(in: items)
    let currentItem = previousAnchorIndex.flatMap { items.indices.contains($0) ? items[$0] : nil }
        ?? selected.last
    guard let currentItem,
          let currentIndex = navigationItemIndex(containing: currentItem, in: items),
          let currentURL = urlAttribute(currentItem) else {
        throw MoveError.missingSelectionURL
    }

    let currentPath = currentURL.standardizedFileURL.path
    let stateURL = marksFileURL()
    var markedPaths = readMarkedPaths(from: stateURL)
    if let index = markedPaths.firstIndex(of: currentPath) {
        markedPaths.remove(at: index)
    } else {
        markedPaths.append(currentPath)
    }
    try writeMarkedPaths(markedPaths, to: stateURL)

    let markedSet = Set(markedPaths)
    let markedItems = items.filter { item in
        guard let url = urlAttribute(item) else { return false }
        return markedSet.contains(url.standardizedFileURL.path)
    }
    var visibleSelection = markedItems
    if !visibleSelection.contains(where: { CFEqual($0, items[currentIndex]) }) {
        visibleSelection.append(items[currentIndex])
    }
    try setSelection(visibleSelection, in: container, role: role, allItems: items)
    try writeNavigationAnchor(indexHint: currentIndex, itemURL: currentURL)
    return markedItems.count
}

func copySelectionInfo(
    _ mode: CopyMode,
    container: AXUIElement,
    role: String
) throws -> Int {
    let items = try navigationItems(container, role: role)
    let urls = selectedItems(container, role: role, items: items).compactMap { urlAttribute($0) }
    guard !urls.isEmpty else { throw MoveError.missingSelectionURL }

    let values = urls.map { url -> String in
        switch mode {
        case .absolute:
            return url.standardizedFileURL.path
        case .directory:
            var isDirectory: ObjCBool = false
            if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
               isDirectory.boolValue {
                return url.standardizedFileURL.path
            }
            return url.deletingLastPathComponent().standardizedFileURL.path
        case .filename:
            return url.lastPathComponent
        case .stem:
            return (url.lastPathComponent as NSString).deletingPathExtension
        }
    }

    let pasteboard = NSPasteboard.general
    pasteboard.clearContents()
    guard pasteboard.setString(values.joined(separator: "\n"), forType: .string) else {
        throw MoveError.stateFile("Could not write to the clipboard")
    }
    return values.count
}

func directlySelectedItems(
    _ container: AXUIElement,
    role: String
) -> [AXUIElement] {
    elements(
        container,
        role == kAXOutlineRole
            ? kAXSelectedRowsAttribute
            : kAXSelectedChildrenAttribute
    )
}

func selectedItemURL(
    in container: AXUIElement,
    role: String
) -> URL? {
    selectedItemWithURL(in: container, role: role)?.url
}

func selectedItemWithURL(
    in container: AXUIElement,
    role: String
) -> (item: AXUIElement, url: URL)? {
    directlySelectedItems(container, role: role)
        .reversed()
        .compactMap { item in
            urlAttribute(item).map { (item, $0) }
        }
        .first
}

func selectedItemContext(
    in finderElement: AXUIElement,
    fallbackContainer: AXUIElement,
    fallbackRole: String
) -> (
    container: AXUIElement,
    role: String,
    subrole: String,
    item: AXUIElement,
    url: URL
)? {
    if let selected = selectedItemWithURL(
        in: fallbackContainer,
        role: fallbackRole
    ) {
        return (
            fallbackContainer,
            fallbackRole,
            stringAttribute(fallbackContainer, kAXSubroleAttribute) ?? "",
            selected.item,
            selected.url
        )
    }

    if let windowValue = try? attribute(
           finderElement,
           kAXFocusedWindowAttribute
       ),
       CFGetTypeID(windowValue) == AXUIElementGetTypeID() {
        let candidates = descendantNavigationContainers(
            from: windowValue as! AXUIElement
        ).compactMap { element, role -> (
            container: AXUIElement,
            role: String,
            subrole: String,
            item: AXUIElement,
            url: URL,
            xPosition: CGFloat
        )? in
            guard let selected = selectedItemWithURL(
                      in: element,
                      role: role
                  ) else {
                return nil
            }
            return (
                element,
                role,
                stringAttribute(element, kAXSubroleAttribute) ?? "",
                selected.item,
                selected.url,
                pointAttribute(element, kAXPositionAttribute)?.x ?? 0
            )
        }
        if let best = candidates.max(by: {
            $0.xPosition < $1.xPosition
        }) {
            return (
                best.container,
                best.role,
                best.subrole,
                best.item,
                best.url
            )
        }
    }
    return nil
}
