import AppKit
import ApplicationServices
import Foundation

// Finding the pane the user is navigating and the items inside it. Container
// attributes are fetched whole; nothing here issues one AX request per item
// outside a compatibility fallback (NFR-PERF-007).

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

func localFilePath(_ url: URL) -> String {
    guard let pathURL = CFURLCreateFilePathURL(
              kCFAllocatorDefault,
              url as CFURL,
              nil
          ) else {
        return url.standardizedFileURL.path
    }
    return (pathURL.takeRetainedValue() as URL)
        .standardizedFileURL.path
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
