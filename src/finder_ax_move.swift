import AppKit
import ApplicationServices
import Foundation

private enum MoveError: Error, CustomStringConvertible {
    case invalidArguments
    case finderIsNotFrontmost
    case accessibilityUnavailable
    case missingAttribute(String)
    case unsupportedRole(String)
    case emptyContainer
    case missingSelectionURL
    case stateFile(String)
    case setAttribute(String, AXError)

    var description: String {
        switch self {
        case .invalidArguments:
            return "Usage: finder_ax_move <down|up> <1...99> | <down-wrap|up-wrap|first|last|toggle-mark|copy-absolute|copy-directory|copy-filename|copy-stem> | <hold-start|hold-repeat> <down|up>"
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
        case let .stateFile(message):
            return "Could not update Finder mark state: \(message)"
        case let .setAttribute(name, error):
            return "Could not set \(name): \(error.rawValue)"
        }
    }
}

private enum Direction: String {
    case down
    case up
    case first
    case last
}

private enum CopyMode: String {
    case absolute = "copy-absolute"
    case directory = "copy-directory"
    case filename = "copy-filename"
    case stem = "copy-stem"
}

private enum Command {
    case move(Direction, Int, wrapping: Bool)
    case holdStart(Direction)
    case holdRepeat(Direction)
    case toggleMark
    case copy(CopyMode)
}

private func attribute(_ element: AXUIElement, _ name: String) throws -> CFTypeRef {
    var value: CFTypeRef?
    let error = AXUIElementCopyAttributeValue(element, name as CFString, &value)
    guard error == .success, let value else {
        throw MoveError.missingAttribute(name)
    }
    return value
}

private func elements(_ element: AXUIElement, _ name: String) -> [AXUIElement] {
    guard let value = try? attribute(element, name) else { return [] }
    return value as? [AXUIElement] ?? []
}

private func stringAttribute(_ element: AXUIElement, _ name: String) -> String? {
    try? attribute(element, name) as? String
}

private func boolAttribute(_ element: AXUIElement, _ name: String) -> Bool {
    (try? attribute(element, name) as? Bool) ?? false
}

private func urlAttribute(_ element: AXUIElement, depth: Int = 0) -> URL? {
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

private func setAttribute(_ element: AXUIElement, _ name: String, _ value: CFTypeRef) throws {
    let error = AXUIElementSetAttributeValue(element, name as CFString, value)
    guard error == .success else {
        throw MoveError.setAttribute(name, error)
    }
}

private func pointAttribute(_ element: AXUIElement, _ name: String) -> CGPoint? {
    guard let value = try? attribute(element, name),
          CFGetTypeID(value) == AXValueGetTypeID() else { return nil }
    var point = CGPoint.zero
    guard AXValueGetValue(value as! AXValue, .cgPoint, &point) else { return nil }
    return point
}

private func descendantNavigationContainers(
    from element: AXUIElement,
    depth: Int = 0
) -> [(AXUIElement, String)] {
    guard depth <= 12 else { return [] }

    let role = stringAttribute(element, kAXRoleAttribute) ?? ""
    var containers: [(AXUIElement, String)] = []
    if role == kAXOutlineRole || role == kAXListRole {
        containers.append((element, role))
    }
    for child in elements(element, kAXChildrenAttribute) {
        containers.append(contentsOf: descendantNavigationContainers(from: child, depth: depth + 1))
    }
    return containers
}

private func navigationContainer(
    from focusedElement: AXUIElement,
    in applicationElement: AXUIElement
) throws -> (AXUIElement, String) {
    var current = focusedElement

    for _ in 0..<10 {
        let role = stringAttribute(current, kAXRoleAttribute) ?? ""
        if role == kAXOutlineRole || role == kAXListRole {
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

private func navigationItems(_ container: AXUIElement, role: String) throws -> [AXUIElement] {
    if role == kAXOutlineRole {
        let rows = elements(container, kAXRowsAttribute).filter { row in
            guard let firstCell = elements(row, kAXChildrenAttribute).first else { return false }
            return !elements(firstCell, kAXChildrenAttribute).isEmpty
        }
        guard !rows.isEmpty else { throw MoveError.emptyContainer }
        return rows
    }

    let items = elements(container, kAXChildrenAttribute)
    guard !items.isEmpty else { throw MoveError.emptyContainer }
    return items
}

private func selectedItems(
    _ container: AXUIElement,
    role: String,
    items: [AXUIElement]
) -> [AXUIElement] {
    if role == kAXOutlineRole {
        return items.filter { boolAttribute($0, kAXSelectedAttribute) }
    }
    return elements(container, kAXSelectedChildrenAttribute)
}

private func marksFileURL() -> URL {
    if let overridePath = ProcessInfo.processInfo.environment["KARABINER_FINDER_MARKS_FILE"] {
        return URL(fileURLWithPath: overridePath)
    }
    return FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent(".local/state/finder-vim/finder_marks.txt")
}

private func holdTokenURL(for direction: Direction) -> URL {
    let suffix = direction == .down ? "down" : "up"
    return FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent(".local/state/finder-vim/finder_\(suffix)_hold.txt")
}

private func readHoldToken(for direction: Direction) -> String {
    let tokenURL = holdTokenURL(for: direction)
    return (try? String(contentsOf: tokenURL, encoding: .utf8))?
        .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
}

private func startHold(for direction: Direction) throws {
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

private func readMarkedPaths(from fileURL: URL) -> [String] {
    guard let contents = try? String(contentsOf: fileURL, encoding: .utf8) else { return [] }
    return contents.split(separator: "\n").map(String.init)
}

private func writeMarkedPaths(_ paths: [String], to fileURL: URL) throws {
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

private func setSelection(
    _ selection: [AXUIElement],
    in container: AXUIElement,
    role: String,
    allItems: [AXUIElement]
) throws {
    if role == kAXListRole {
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

private func toggleCurrentMark(_ container: AXUIElement, role: String) throws -> Int {
    let items = try navigationItems(container, role: role)
    let selected = selectedItems(container, role: role, items: items)
    guard let currentURL = selected.last.flatMap({ urlAttribute($0) }) else {
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
    try setSelection(markedItems, in: container, role: role, allItems: items)
    return markedItems.count
}

private func copySelectionInfo(
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

private func targetIndex(
    currentIndex: Int?,
    itemCount: Int,
    direction: Direction,
    count: Int,
    wrapping: Bool
) -> Int {
    switch direction {
    case .down:
        let start = currentIndex ?? -1
        if wrapping { return (start + count) % itemCount }
        return min(start + count, itemCount - 1)
    case .up:
        let start = currentIndex ?? itemCount
        if wrapping { return ((start - count) % itemCount + itemCount) % itemCount }
        return max(start - count, 0)
    case .first:
        return 0
    case .last:
        return itemCount - 1
    }
}

private func moveInOutline(
    _ outline: AXUIElement,
    direction: Direction,
    count: Int,
    wrapping: Bool = false
) throws -> Int {
    let rows = try navigationItems(outline, role: kAXOutlineRole)

    let currentIndex = rows.firstIndex { boolAttribute($0, kAXSelectedAttribute) }
    let destinationIndex = targetIndex(
        currentIndex: currentIndex,
        itemCount: rows.count,
        direction: direction,
        count: count,
        wrapping: wrapping
    )

    try setSelection(
        [rows[destinationIndex]],
        in: outline,
        role: kAXOutlineRole,
        allItems: rows
    )
    return destinationIndex + 1
}

private func moveInList(
    _ list: AXUIElement,
    direction: Direction,
    count: Int,
    wrapping: Bool = false
) throws -> Int {
    let items = try navigationItems(list, role: kAXListRole)

    let selectedItems = elements(list, kAXSelectedChildrenAttribute)
    let currentIndex = selectedItems.first.flatMap { selectedItem in
        items.firstIndex { CFEqual($0, selectedItem) }
    }
    let destinationIndex = targetIndex(
        currentIndex: currentIndex,
        itemCount: items.count,
        direction: direction,
        count: count,
        wrapping: wrapping
    )

    let selection = [items[destinationIndex]] as CFArray
    try setAttribute(list, kAXSelectedChildrenAttribute, selection)
    _ = AXUIElementSetAttributeValue(list, kAXFocusedAttribute as CFString, kCFBooleanTrue)
    return destinationIndex + 1
}

private func run() throws -> Int {
    let arguments = Array(CommandLine.arguments.dropFirst())
    let command: Command
    if arguments.count == 1, arguments[0] == "toggle-mark" {
        command = .toggleMark
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

    let repeatToken: String
    switch command {
    case let .holdRepeat(direction):
        repeatToken = readHoldToken(for: direction)
        if repeatToken.isEmpty { return 0 }
    default:
        repeatToken = ""
    }

    guard AXIsProcessTrusted() else { throw MoveError.accessibilityUnavailable }
    guard let finder = NSRunningApplication.runningApplications(
        withBundleIdentifier: "com.apple.finder"
    ).first, finder.isActive else {
        throw MoveError.finderIsNotFrontmost
    }

    let finderElement = AXUIElementCreateApplication(finder.processIdentifier)
    let focusedElement = try attribute(finderElement, kAXFocusedUIElementAttribute) as! AXUIElement
    let (container, role) = try navigationContainer(from: focusedElement, in: finderElement)

    func move(_ direction: Direction, count: Int, wrapping: Bool) throws -> Int {
        if role == kAXOutlineRole {
            return try moveInOutline(
                container,
                direction: direction,
                count: count,
                wrapping: wrapping
            )
        }
        return try moveInList(
            container,
            direction: direction,
            count: count,
            wrapping: wrapping
        )
    }

    switch command {
    case let .move(direction, count, wrapping):
        return try move(direction, count: count, wrapping: wrapping)
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
    }
}

do {
    print(try run())
} catch {
    fputs("finder_ax_move: \(error)\n", stderr)
    exit(1)
}
