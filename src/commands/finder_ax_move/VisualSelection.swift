import AppKit
import ApplicationServices
import Foundation

// Visual Mode. The origin is pinned when `v` is pressed and kept in a state
// file, so every later keystroke extends the range from that fixed item
// (FR-MODE-009).

func targetIndex(
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

func visualEndpointIndex(
    anchorIndex: Int,
    selected: [AXUIElement],
    items: [AXUIElement]
) -> Int {
    let selectedIndices = selected.compactMap {
        navigationItemIndex(containing: $0, in: items)
    }
    guard !selectedIndices.isEmpty else { return anchorIndex }

    let first = selectedIndices.min() ?? anchorIndex
    let last = selectedIndices.max() ?? anchorIndex
    if first == anchorIndex { return last }
    if last == anchorIndex { return first }
    if selectedIndices.contains(anchorIndex) {
        return abs(first - anchorIndex) > abs(last - anchorIndex) ? first : last
    }
    return selectedIndices.last ?? anchorIndex
}

func isGridContainer(_ container: AXUIElement, role: String) -> Bool {
    role == kAXGridRole
        || stringAttribute(container, kAXSubroleAttribute) == "AXCollectionList"
}

func visualEdgeIndex(
    container: AXUIElement,
    role: String,
    items: [AXUIElement],
    endpointIndex: Int,
    last: Bool
) -> Int {
    guard isGridContainer(container, role: role),
          items.indices.contains(endpointIndex),
          let endpointPosition = pointAttribute(items[endpointIndex], kAXPositionAttribute) else {
        return last ? items.count - 1 : 0
    }

    var targetIndex = endpointIndex
    var targetY = endpointPosition.y
    for (index, item) in items.enumerated() {
        guard let position = pointAttribute(item, kAXPositionAttribute),
              abs(position.x - endpointPosition.x) <= 8 else { continue }
        if (last && position.y > targetY) || (!last && position.y < targetY) {
            targetIndex = index
            targetY = position.y
        }
    }
    return targetIndex
}

func startVisualSelection(
    container: AXUIElement,
    role: String,
    items: [AXUIElement]
) throws -> Int {
    let selected = selectedItems(container, role: role, items: items)
    guard let currentItem = selected.last,
          let currentIndex = navigationItemIndex(containing: currentItem, in: items) else {
        throw MoveError.missingSelectionURL
    }
    try writeVisualAnchor(indexHint: currentIndex, item: items[currentIndex])
    return currentIndex + 1
}

func extendVisualSelection(
    container: AXUIElement,
    role: String,
    items: [AXUIElement],
    direction: Direction,
    count: Int
) throws -> Int? {
    guard let anchorIndex = readVisualAnchor(in: items) else { return nil }
    let selected = selectedItems(container, role: role, items: items)
    let endpointIndex = visualEndpointIndex(
        anchorIndex: anchorIndex,
        selected: selected,
        items: items
    )

    let destinationIndex: Int
    switch direction {
    case .down, .up:
        destinationIndex = targetIndex(
            currentIndex: endpointIndex,
            itemCount: items.count,
            direction: direction,
            count: count,
            wrapping: false
        )
    case .first, .last:
        destinationIndex = visualEdgeIndex(
            container: container,
            role: role,
            items: items,
            endpointIndex: endpointIndex,
            last: direction == .last
        )
    }

    let lowerBound = min(anchorIndex, destinationIndex)
    let upperBound = max(anchorIndex, destinationIndex)
    let selection = Array(items[lowerBound...upperBound])
    try setSelection(selection, in: container, role: role, allItems: items)
    return destinationIndex + 1
}

func extendVisualSelectionAfterPendingStart(
    container: AXUIElement,
    role: String,
    items: [AXUIElement],
    direction: Direction,
    count: Int
) throws -> Int {
    for attempt in 0..<20 {
        if let position = try extendVisualSelection(
            container: container,
            role: role,
            items: items,
            direction: direction,
            count: count
        ) {
            return position
        }
        if attempt < 19 { Thread.sleep(forTimeInterval: 0.001) }
    }
    throw MoveError.stateFile("Visual selection anchor is unavailable")
}
