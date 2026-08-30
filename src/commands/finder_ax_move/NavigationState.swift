import AppKit
import ApplicationServices
import Foundation

// The state files under ~/.local/state/finder-vim that carry a cursor between
// two short-lived processes: the mark list, the navigation anchor required by
// FR-MODE-007, and the Visual Mode origin. Every path honours its test override
// so an isolated HOME can relocate it.

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
