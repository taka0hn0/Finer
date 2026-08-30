import AppKit
import ApplicationServices
import Foundation

// Typed reads and writes for the Accessibility API. Nothing here knows about
// Finder's window structure; the callers decide which attributes mean what.

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

func integerAttribute(_ element: AXUIElement, _ name: String) -> Int? {
    guard let value = try? attribute(element, name),
          CFGetTypeID(value) == CFNumberGetTypeID() else {
        return nil
    }
    return (value as? NSNumber)?.intValue
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

func setAttribute(_ element: AXUIElement, _ name: String, _ value: CFTypeRef) throws {
    let error = AXUIElementSetAttributeValue(element, name as CFString, value)
    guard error == .success else {
        throw MoveError.setAttribute(name, error)
    }
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
