import AppKit
import ApplicationServices
import Foundation

func pointAttribute(_ element: AXUIElement, _ name: String) -> CGPoint? {
    guard let value = try? attribute(element, name),
          CFGetTypeID(value) == AXValueGetTypeID() else { return nil }
    var point = CGPoint.zero
    guard AXValueGetValue(value as! AXValue, .cgPoint, &point) else { return nil }
    return point
}

func sizeAttribute(_ element: AXUIElement, _ name: String) -> CGSize? {
    guard let value = try? attribute(element, name),
          CFGetTypeID(value) == AXValueGetTypeID() else { return nil }
    var size = CGSize.zero
    guard AXValueGetValue(value as! AXValue, .cgSize, &size) else { return nil }
    return size
}

func descendantNavigationContainers(
    from element: AXUIElement,
    depth: Int = 0
) -> [(AXUIElement, String)] {
    guard depth <= 12 else { return [] }

    let role = stringAttribute(element, kAXRoleAttribute) ?? ""
    if role == kAXOutlineRole || role == kAXListRole || role == kAXGridRole {
        // Navigation containers can expose thousands of rows as descendants.
        // Their items cannot contain another Finder navigation pane, so stop
        // here instead of walking every item during fallback discovery.
        return [(element, role)]
    }

    var containers: [(AXUIElement, String)] = []
    for child in elements(element, kAXChildrenAttribute) {
        containers.append(contentsOf: descendantNavigationContainers(from: child, depth: depth + 1))
    }
    return containers
}

func navigationContainer(
    from focusedElement: AXUIElement,
    in applicationElement: AXUIElement
) throws -> (AXUIElement, String) {
    var current = focusedElement

    for _ in 0..<10 {
        let role = stringAttribute(current, kAXRoleAttribute) ?? ""
        if role == kAXOutlineRole || role == kAXListRole || role == kAXGridRole {
            return (current, role)
        }

        guard let parentValue = try? attribute(current, kAXParentAttribute),
              CFGetTypeID(parentValue) == AXUIElementGetTypeID() else {
            break
        }
        let parent = parentValue as! AXUIElement
        current = parent
    }

    if let windowValue = try? attribute(applicationElement, kAXFocusedWindowAttribute),
       CFGetTypeID(windowValue) == AXUIElementGetTypeID() {
        let window = windowValue as! AXUIElement
        let candidates = descendantNavigationContainers(from: window).compactMap {
            (element, role) -> (AXUIElement, String, Bool, CGFloat)? in
            guard let items = try? navigationItems(element, role: role) else { return nil }
            let hasSelection = !selectedItems(element, role: role, items: items).isEmpty
            let xPosition = pointAttribute(element, kAXPositionAttribute)?.x ?? 0
            return (element, role, hasSelection, xPosition)
        }
        if let best = candidates.max(by: { left, right in
            if left.2 != right.2 { return !left.2 && right.2 }
            return left.3 < right.3
        }) {
            return (best.0, best.1)
        }
    }

    let role = stringAttribute(focusedElement, kAXRoleAttribute) ?? "unknown"
    throw MoveError.unsupportedRole(role)
}

func navigationItems(_ container: AXUIElement, role: String) throws -> [AXUIElement] {
    if role == kAXOutlineRole {
        let rows = elements(container, kAXRowsAttribute).filter { row in
            guard let firstCell = elements(row, kAXChildrenAttribute).first else { return false }
            return !elements(firstCell, kAXChildrenAttribute).isEmpty
        }
        guard !rows.isEmpty else { throw MoveError.emptyContainer }
        return rows
    }

    let directChildren = elements(container, kAXChildrenAttribute)
    let items = stringAttribute(container, kAXSubroleAttribute) == "AXCollectionList"
        ? directChildren.flatMap { elements($0, kAXChildrenAttribute) }
        : directChildren
    guard !items.isEmpty else { throw MoveError.emptyContainer }
    return items
}

func selectedItems(
    _ container: AXUIElement,
    role: String,
    items: [AXUIElement]
) -> [AXUIElement] {
    if role == kAXOutlineRole {
        return items.filter { boolAttribute($0, kAXSelectedAttribute) }
    }
    return elements(container, kAXSelectedChildrenAttribute)
}

func marksFileURL() -> URL {
    if let overridePath = ProcessInfo.processInfo.environment["KARABINER_FINDER_MARKS_FILE"] {
        return URL(fileURLWithPath: overridePath)
    }
    return FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent(".local/state/finder-vim/finder_marks.txt")
}

struct NavigationAnchorRecord {
    let indexHint: Int
    let itemURL: String
}

struct VisualAnchorRecord {
    let indexHint: Int
    let itemPath: String
}

func navigationAnchorFileURL() -> URL {
    if let overridePath = ProcessInfo.processInfo.environment["KARABINER_FINDER_ANCHOR_FILE"] {
        return URL(fileURLWithPath: overridePath)
    }
    return FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent(".local/state/finder-vim/finder_navigation_anchor.txt")
}

func visualAnchorFileURL() -> URL {
    if let overridePath = ProcessInfo.processInfo.environment["KARABINER_FINDER_VISUAL_ANCHOR_FILE"] {
        return URL(fileURLWithPath: overridePath)
    }
    return FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent(".local/state/finder-vim/finder_visual_anchor.txt")
}

func navigationItemMatchesURL(
    _ item: AXUIElement,
    expectedURLString: String
) -> Bool {
    guard let itemURL = urlAttribute(item) else { return false }
    if itemURL.absoluteString == expectedURLString {
        return true
    }
    guard let expectedURL = URL(string: expectedURLString) else {
        return false
    }
    return localFilePath(itemURL) == localFilePath(expectedURL)
}

func navigationItemIndex(
    containing element: AXUIElement,
    in items: [AXUIElement]
) -> Int? {
    var current = element
    for _ in 0..<8 {
        if let index = items.firstIndex(where: { CFEqual($0, current) }) {
            return index
        }
        guard let parentValue = try? attribute(current, kAXParentAttribute),
              CFGetTypeID(parentValue) == AXUIElementGetTypeID() else {
            return nil
        }
        current = parentValue as! AXUIElement
    }
    return nil
}

func parseNavigationAnchor(_ contents: String) -> NavigationAnchorRecord? {
    let fields = contents.split(
        separator: "\t",
        maxSplits: 2,
        omittingEmptySubsequences: false
    )
    guard fields.count == 3,
          fields[0] == "1",
          let indexHint = Int(fields[1]) else { return nil }

    let itemURL = String(fields[2]).trimmingCharacters(in: .newlines)
    guard !itemURL.isEmpty else { return nil }
    return NavigationAnchorRecord(indexHint: indexHint, itemURL: itemURL)
}

func takeNavigationAnchor(in items: [AXUIElement]) -> Int? {
    let stateURL = navigationAnchorFileURL()
    let claimedURL = stateURL.deletingLastPathComponent().appendingPathComponent(
        ".finder_navigation_anchor.consuming.\(ProcessInfo.processInfo.processIdentifier).\(UUID().uuidString)"
    )
    do {
        try FileManager.default.moveItem(at: stateURL, to: claimedURL)
    } catch {
        return nil
    }
    defer { try? FileManager.default.removeItem(at: claimedURL) }

    guard let values = try? claimedURL.resourceValues(
        forKeys: [.isRegularFileKey, .isSymbolicLinkKey]
    ), values.isRegularFile == true, values.isSymbolicLink != true else {
        return nil
    }
    guard let contents = try? String(contentsOf: claimedURL, encoding: .utf8),
          let record = parseNavigationAnchor(contents) else { return nil }

    if items.indices.contains(record.indexHint),
       navigationItemMatchesURL(
           items[record.indexHint],
           expectedURLString: record.itemURL
       ) {
        return record.indexHint
    }
    return items.firstIndex {
        navigationItemMatchesURL(
            $0,
            expectedURLString: record.itemURL
        )
    }
}

func writeNavigationAnchor(indexHint: Int, itemURL: URL) throws {
    let stateURL = navigationAnchorFileURL()
    do {
        try FileManager.default.createDirectory(
            at: stateURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        let contents = "1\t\(indexHint)\t\(itemURL.absoluteString)\n"
        try contents.write(to: stateURL, atomically: true, encoding: .utf8)
    } catch {
        throw MoveError.stateFile(error.localizedDescription)
    }
}

func parseVisualAnchor(_ contents: String) -> VisualAnchorRecord? {
    let fields = contents.split(
        separator: "\t",
        maxSplits: 2,
        omittingEmptySubsequences: false
    )
    guard fields.count == 3,
          fields[0] == "1",
          let indexHint = Int(fields[1]) else { return nil }

    let itemPath = String(fields[2]).trimmingCharacters(in: .newlines)
    guard !itemPath.isEmpty else { return nil }
    return VisualAnchorRecord(indexHint: indexHint, itemPath: itemPath)
}

func visualItemPath(_ item: AXUIElement) -> String? {
    urlAttribute(item)?.standardizedFileURL.path
}

func readVisualAnchor(in items: [AXUIElement]) -> Int? {
    let stateURL = visualAnchorFileURL()
    guard let values = try? stateURL.resourceValues(
        forKeys: [.isRegularFileKey, .isSymbolicLinkKey]
    ), values.isRegularFile == true, values.isSymbolicLink != true else {
        return nil
    }
    guard let contents = try? String(contentsOf: stateURL, encoding: .utf8),
          let record = parseVisualAnchor(contents) else { return nil }

    if items.indices.contains(record.indexHint),
       visualItemPath(items[record.indexHint]) == record.itemPath {
        return record.indexHint
    }
    return items.firstIndex { visualItemPath($0) == record.itemPath }
}

func writeVisualAnchor(indexHint: Int, item: AXUIElement) throws {
    guard let itemPath = visualItemPath(item) else {
        throw MoveError.missingSelectionURL
    }
    let stateURL = visualAnchorFileURL()
    do {
        try FileManager.default.createDirectory(
            at: stateURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        let contents = "1\t\(indexHint)\t\(itemPath)\n"
        try contents.write(to: stateURL, atomically: true, encoding: .utf8)
    } catch {
        throw MoveError.stateFile(error.localizedDescription)
    }
}

func holdTokenURL(for direction: Direction) -> URL {
    let suffix = direction == .down ? "down" : "up"
    return FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent(".local/state/finder-vim/finder_\(suffix)_hold.txt")
}

func readHoldToken(for direction: Direction) -> String {
    let tokenURL = holdTokenURL(for: direction)
    return (try? String(contentsOf: tokenURL, encoding: .utf8))?
        .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
}

func startHold(for direction: Direction) throws {
    let tokenURL = holdTokenURL(for: direction)
    do {
        try FileManager.default.createDirectory(
            at: tokenURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try UUID().uuidString.write(to: tokenURL, atomically: false, encoding: .utf8)
    } catch {
        throw MoveError.stateFile(error.localizedDescription)
    }
}
