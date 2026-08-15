import AppKit
import ApplicationServices
import Foundation

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

func itemDisplayName(
    _ element: AXUIElement,
    depth: Int = 0
) -> String? {
    let role = stringAttribute(element, kAXRoleAttribute)
    if role == kAXTextFieldRole,
       let value = stringAttribute(element, kAXValueAttribute),
       !value.isEmpty {
        return value
    }
    if let title = stringAttribute(element, kAXTitleAttribute),
       !title.isEmpty {
        return title
    }
    guard depth < 3 else { return nil }
    for child in elements(element, kAXChildrenAttribute) {
        if let name = itemDisplayName(child, depth: depth + 1) {
            return name
        }
    }
    return nil
}

func createUntitledFolder(in parentURL: URL) throws -> URL {
    let fileManager = FileManager.default
    for suffix in 1...10_000 {
        let name = suffix == 1
            ? "untitled folder"
            : "untitled folder \(suffix)"
        let candidate = parentURL.appendingPathComponent(
            name,
            isDirectory: true
        )
        if fileManager.fileExists(atPath: candidate.path) {
            continue
        }

        do {
            try fileManager.createDirectory(
                at: candidate,
                withIntermediateDirectories: false
            )
            return candidate
        } catch {
            if fileManager.fileExists(atPath: candidate.path) {
                continue
            }
            throw MoveError.folderCreation(error.localizedDescription)
        }
    }
    throw MoveError.folderCreation(
        "Could not choose an unused default name"
    )
}

func selectCreatedFolder(
    _ createdURL: URL,
    in container: AXUIElement,
    role: String
) throws {
    let createdPath = localFilePath(createdURL)
    let createdName = createdURL.lastPathComponent
    let deadline = Date().addingTimeInterval(1.0)

    repeat {
        let directChildren = elements(container, kAXChildrenAttribute)
        let items: [AXUIElement]
        if role == kAXOutlineRole {
            // Unlike normal movement, locating a uniquely named new folder
            // does not need to validate every row as selectable first.
            items = elements(container, kAXRowsAttribute)
        } else if stringAttribute(
            container,
            kAXSubroleAttribute
        ) == "AXCollectionList" {
            items = directChildren.flatMap {
                elements($0, kAXChildrenAttribute)
            }
        } else {
            items = directChildren
        }

        var match: (item: AXUIElement, index: Int)?
        for offset in 0..<((items.count + 1) / 2) {
            let leadingIndex = offset
            let trailingIndex = items.count - 1 - offset
            for index in leadingIndex == trailingIndex
                ? [leadingIndex]
                : [leadingIndex, trailingIndex] {
                let item = items[index]
                if itemDisplayName(item) == createdName {
                    match = (item, index)
                    break
                }
                if let itemURL = urlAttribute(item),
                   localFilePath(itemURL) == createdPath {
                    match = (item, index)
                    break
                }
            }
            if match != nil { break }
        }

        if let match {
            try setSelection(
                [match.item],
                in: container,
                role: role,
                allItems: items
            )
            _ = AXUIElementSetAttributeValue(
                container,
                kAXFocusedAttribute as CFString,
                kCFBooleanTrue
            )
            scrollCreatedItemToVisible(
                match.item,
                index: match.index,
                itemCount: items.count,
                container: container
            )
            return
        }
        Thread.sleep(forTimeInterval: 0.01)
    } while Date() < deadline

    throw MoveError.createdFolderUnavailable
}

func performScrollToVisible(
    _ element: AXUIElement,
    depth: Int = 0
) -> Bool {
    if AXUIElementPerformAction(
        element,
        "AXScrollToVisible" as CFString
    ) == .success {
        return true
    }
    guard depth < 3 else { return false }
    for child in elements(element, kAXChildrenAttribute) {
        if performScrollToVisible(child, depth: depth + 1) {
            return true
        }
    }
    return false
}

func verticalScrollBar(
    from container: AXUIElement
) -> AXUIElement? {
    var current = container
    for _ in 0..<8 {
        if let value = try? attribute(
               current,
               kAXVerticalScrollBarAttribute
           ),
           CFGetTypeID(value) == AXUIElementGetTypeID() {
            return (value as! AXUIElement)
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

func scrollCreatedItemToVisible(
    _ item: AXUIElement,
    index: Int,
    itemCount: Int,
    container: AXUIElement
) {
    if performScrollToVisible(item) {
        return
    }

    guard itemCount > 1,
          let scrollBar = verticalScrollBar(from: container) else {
        return
    }
    let position = NSNumber(
        value: Double(index) / Double(itemCount - 1)
    )
    guard AXUIElementSetAttributeValue(
              scrollBar,
              kAXValueAttribute as CFString,
              position
          ) == .success else {
        return
    }
    Thread.sleep(forTimeInterval: 0.01)
    _ = performScrollToVisible(item)
}

func finderFocusedElementRole(
    _ finderElement: AXUIElement
) -> String? {
    guard let focusedValue = try? attribute(
              finderElement,
              kAXFocusedUIElementAttribute
          ),
          CFGetTypeID(focusedValue) == AXUIElementGetTypeID() else {
        return nil
    }
    return stringAttribute(
        focusedValue as! AXUIElement,
        kAXRoleAttribute
    )
}

func finderFocusedElementIsTextInput(
    _ finderElement: AXUIElement
) -> Bool {
    (finderFocusedElementRole(finderElement) ?? "").hasPrefix("AXText")
}

func stabilizeNativeNewFolderEditing(
    previousURL: URL,
    container: AXUIElement,
    role: String,
    finderElement: AXUIElement
) {
    let previousPath = localFilePath(previousURL)
    let selectionDeadline = Date().addingTimeInterval(0.75)
    var createdItem: AXUIElement?

    repeat {
        if let selected = selectedItemWithURL(
               in: container,
               role: role
           ),
           localFilePath(selected.url) != previousPath {
            createdItem = selected.item
            break
        }
        Thread.sleep(forTimeInterval: 0.005)
    } while Date() < selectionDeadline

    guard let createdItem else { return }
    _ = performScrollToVisible(createdItem)

    let stabilityDeadline = Date().addingTimeInterval(0.75)
    var previousPosition: CGPoint?
    var stableVisibleSamples = 0

    repeat {
        guard let itemPosition = pointAttribute(
                  createdItem,
                  kAXPositionAttribute
              ),
              let itemSize = sizeAttribute(
                  createdItem,
                  kAXSizeAttribute
              ),
              let containerPosition = pointAttribute(
                  container,
                  kAXPositionAttribute
              ),
              let containerSize = sizeAttribute(
                  container,
                  kAXSizeAttribute
              ) else {
            break
        }

        let itemFrame = CGRect(
            origin: itemPosition,
            size: itemSize
        )
        let containerFrame = CGRect(
            origin: containerPosition,
            size: containerSize
        )
        let isVisible = itemFrame.intersects(containerFrame)
        let isStable = previousPosition.map {
            abs($0.x - itemPosition.x) < 0.5
                && abs($0.y - itemPosition.y) < 0.5
        } ?? false

        stableVisibleSamples = isVisible && isStable
            ? stableVisibleSamples + 1
            : 0
        previousPosition = itemPosition
        if stableVisibleSamples >= 3 {
            break
        }
        Thread.sleep(forTimeInterval: 0.01)
    } while Date() < stabilityDeadline

    if !finderFocusedElementIsTextInput(finderElement) {
        try? performRenameMenuAction(finderElement: finderElement)
    }
}

func createNewFolderAtSelectionLevel(
    finder: NSRunningApplication,
    finderElement: AXUIElement,
    container: AXUIElement,
    role: String
) throws -> Int {
    let selection = selectedItemContext(
        in: finderElement,
        fallbackContainer: container,
        fallbackRole: role
    )

    guard let selection else {
        guard finder.isActive else {
            throw MoveError.finderIsNotFrontmost
        }
        try performNewFolderMenuAction(finderElement: finderElement)
        return 0
    }

    let selectedAttribute = selection.role == kAXOutlineRole
        ? kAXSelectedRowsAttribute
        : kAXSelectedChildrenAttribute
    let selectedDisclosureLevel = integerAttribute(
        selection.item,
        kAXDisclosureLevelAttribute
    )
    let baseDisclosureLevel = selection.role == kAXOutlineRole
        ? elements(
            selection.container,
            kAXRowsAttribute
        ).compactMap { row -> Int? in
            guard urlAttribute(row) != nil else { return nil }
            return integerAttribute(row, kAXDisclosureLevelAttribute)
        }.min()
        : nil
    let isNestedListItem = selectedDisclosureLevel != nil
        && baseDisclosureLevel != nil
        && selectedDisclosureLevel! > baseDisclosureLevel!
    if !isNestedListItem,
       let menuItem = finderNewFolderMenuItem(
           finderElement: finderElement
       ) {
        do {
            try setAttribute(
                selection.container,
                selectedAttribute,
                [] as CFArray
            )
            try setAttribute(
                selection.container,
                kAXFocusedAttribute,
                kCFBooleanTrue
            )
            guard AXUIElementPerformAction(
                      menuItem,
                      kAXPressAction as CFString
                  ) == .success else {
                throw MoveError.menuAction
            }
            stabilizeNativeNewFolderEditing(
                previousURL: selection.url,
                container: selection.container,
                role: selection.role,
                finderElement: finderElement
            )
            return 0
        } catch {
            try? setSelection(
                [selection.item],
                in: selection.container,
                role: selection.role,
                allItems: [selection.item]
            )
        }
    }

    let selectedURL = URL(fileURLWithPath: localFilePath(selection.url))
    let createdURL = try createUntitledFolder(
        in: selectedURL.deletingLastPathComponent()
    )
    NSWorkspace.shared.noteFileSystemChanged(
        createdURL.deletingLastPathComponent().path
    )
    try selectCreatedFolder(
        createdURL,
        in: selection.container,
        role: selection.role
    )

    guard finder.isActive else {
        throw MoveError.finderIsNotFrontmost
    }
    try performRenameMenuAction(finderElement: finderElement)
    return 0
}
