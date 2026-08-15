import AppKit
import Foundation

final class JumpPanel: NSPanel {
    var handleNavigationKey: ((NSEvent) -> Bool)?
    var recordLifecycleEvent: ((String) -> Void)?

    override func sendEvent(_ event: NSEvent) {
        if event.type == .keyDown, handleNavigationKey?(event) == true {
            return
        }
        super.sendEvent(event)
    }

    override func cancelOperation(_ sender: Any?) {
        recordLifecycleEvent?("panel-cancel-operation")
        super.cancelOperation(sender)
    }

    override func close() {
        recordLifecycleEvent?("panel-close")
        super.close()
    }

    override func orderOut(_ sender: Any?) {
        recordLifecycleEvent?("panel-order-out")
        super.orderOut(sender)
    }
}

struct PaletteEscapeGate {
    static let markerKeyCode: UInt16 = 90 // F20 on macOS.
    private static let markerLifetime: TimeInterval = 0.1

    private var markerDeadline = -TimeInterval.infinity

    mutating func markPhysicalEscape(at time: TimeInterval) {
        markerDeadline = time + Self.markerLifetime
    }

    mutating func consumePhysicalEscape(at time: TimeInterval) -> Bool {
        let isMarked = time <= markerDeadline
        markerDeadline = -TimeInterval.infinity
        return isMarked
    }

    mutating func clear() {
        markerDeadline = -TimeInterval.infinity
    }
}

enum PaletteKeyCode {
    static let selectAll: UInt16 = 80 // F19 on macOS.
}

final class JumpDiagnostics {
    private let handle: FileHandle?

    init() {
        let temporaryDirectory = URL(fileURLWithPath: NSTemporaryDirectory())
        let enabledURL = temporaryDirectory
            .appendingPathComponent("finer-jump-diagnostics-enabled")
        guard FileManager.default.fileExists(atPath: enabledURL.path) else {
            handle = nil
            return
        }

        let logURL = temporaryDirectory
            .appendingPathComponent("finer-jump-events.log")
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        handle = try? FileHandle(forWritingTo: logURL)
        record("launch pid=\(ProcessInfo.processInfo.processIdentifier)")
    }

    deinit {
        try? handle?.close()
    }

    func record(_ message: String) {
        guard let handle else { return }
        let timestamp = String(
            format: "%.6f",
            ProcessInfo.processInfo.systemUptime
        )
        handle.write(Data("\(timestamp) \(message)\n".utf8))
        handle.synchronizeFile()
    }
}

final class JumpController: NSObject, NSApplicationDelegate,
    NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate
{
    private let zoxide: ZoxideClient?
    private let spotlight: SpotlightClient?
    private let zoxideLoadError: Error?
    private let spotlightLoadError: Error?
    private var folderCandidates: [JumpCandidate] = []
    private var spotlightCandidates: [JumpCandidate] = []
    private var filteredCandidates: [JumpCandidate] = []
    private var fileSearchWorkItem: DispatchWorkItem?
    private var fileSearchError: Error?
    private var isSearchingFiles = false
    private var shouldAcceptAfterFileSearch = false
    private var inputSourceObserver: NSObjectProtocol?
    private var isCompletingSelection = false
    private var escapeGate = PaletteEscapeGate()
    private let iconCache = NSCache<NSString, NSImage>()
    private let diagnostics = JumpDiagnostics()

    private let panel: JumpPanel
    private let searchField = NSTextField()
    private let tableView = NSTableView()
    private let statusLabel = NSTextField(labelWithString: "")

    override init() {
        do {
            zoxide = try ZoxideClient.discover()
            zoxideLoadError = nil
        } catch {
            zoxide = nil
            zoxideLoadError = error
        }
        do {
            spotlight = try SpotlightClient.discover()
            spotlightLoadError = nil
        } catch {
            spotlight = nil
            spotlightLoadError = error
        }

        panel = JumpPanel(
            contentRect: NSRect(x: 0, y: 0, width: 820, height: 430),
            styleMask: [.titled, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        super.init()
        iconCache.countLimit = 128
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        inputSourceObserver = NotificationCenter.default.addObserver(
            forName: NSTextInputContext.keyboardSelectionDidChangeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            self.diagnostics.record("input-source-changed")
            self.restoreSearchFocus()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                self.restoreSearchFocus()
            }
        }
        configurePanel()
        loadCandidates()
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
        panel.center()
        panel.makeFirstResponder(searchField)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        diagnostics.record("ignored-terminate-after-last-window")
        return false
    }

    func applicationWillTerminate(_ notification: Notification) {
        diagnostics.record("application-will-terminate")
        fileSearchWorkItem?.cancel()
        spotlight?.cancel()
        if let inputSourceObserver {
            NotificationCenter.default.removeObserver(inputSourceObserver)
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        diagnostics.record("application-did-become-active")
        restoreSearchFocus()
    }

    private func restoreSearchFocus() {
        guard panel.isVisible, !isCompletingSelection else {
            return
        }
        if panel.isKeyWindow, searchField.currentEditor() != nil {
            return
        }
        if !NSApp.isActive {
            NSApp.activate(ignoringOtherApps: true)
        }
        panel.makeKeyAndOrderFront(nil)
        if searchField.currentEditor() == nil {
            panel.makeFirstResponder(searchField)
        }
    }

    private func configurePanel() {
        panel.title = "Finer — Jump"
        panel.titleVisibility = .hidden
        panel.titlebarAppearsTransparent = true
        panel.isMovableByWindowBackground = true
        panel.isReleasedWhenClosed = false
        panel.hidesOnDeactivate = false
        panel.level = .floating
        panel.collectionBehavior = [.moveToActiveSpace, .fullScreenAuxiliary]
        panel.backgroundColor = .windowBackgroundColor
        panel.handleNavigationKey = { [weak self] event in
            self?.handleKey(event) ?? false
        }
        panel.recordLifecycleEvent = { [weak self] event in
            self?.diagnostics.record(event)
        }

        guard let contentView = panel.contentView else {
            return
        }

        searchField.isEditable = true
        searchField.isSelectable = true
        searchField.isBezeled = true
        searchField.bezelStyle = .roundedBezel
        searchField.cell?.usesSingleLineMode = true
        searchField.cell?.lineBreakMode = .byClipping
        searchField.placeholderString = "Jump to a folder or file…"
        searchField.font = .systemFont(ofSize: 18, weight: .regular)
        searchField.focusRingType = .none
        searchField.delegate = self
        searchField.translatesAutoresizingMaskIntoConstraints = false

        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("folder"))
        column.resizingMask = .autoresizingMask
        tableView.addTableColumn(column)
        tableView.headerView = nil
        tableView.rowHeight = 48
        tableView.intercellSpacing = NSSize(width: 0, height: 2)
        tableView.selectionHighlightStyle = .regular
        tableView.dataSource = self
        tableView.delegate = self
        tableView.doubleAction = #selector(acceptSelection)
        tableView.target = self
        tableView.translatesAutoresizingMaskIntoConstraints = false

        let scrollView = NSScrollView()
        scrollView.documentView = tableView
        scrollView.hasVerticalScroller = true
        scrollView.drawsBackground = false
        scrollView.borderType = .noBorder
        scrollView.translatesAutoresizingMaskIntoConstraints = false

        statusLabel.textColor = .secondaryLabelColor
        statusLabel.font = .systemFont(ofSize: 12)
        statusLabel.lineBreakMode = .byTruncatingTail
        statusLabel.translatesAutoresizingMaskIntoConstraints = false

        contentView.addSubview(searchField)
        contentView.addSubview(scrollView)
        contentView.addSubview(statusLabel)

        NSLayoutConstraint.activate([
            searchField.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 30),
            searchField.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 22),
            searchField.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -22),
            searchField.heightAnchor.constraint(equalToConstant: 32),

            scrollView.topAnchor.constraint(equalTo: searchField.bottomAnchor, constant: 14),
            scrollView.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 14),
            scrollView.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -14),
            scrollView.bottomAnchor.constraint(equalTo: statusLabel.topAnchor, constant: -8),

            statusLabel.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 22),
            statusLabel.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -22),
            statusLabel.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -12),
            statusLabel.heightAnchor.constraint(equalToConstant: 18),
        ])
    }

    private func loadCandidates() {
        do {
            folderCandidates = try zoxide?.candidates() ?? []
        } catch {
            folderCandidates = []
        }
        updateFilter()
    }

    func controlTextDidChange(_ notification: Notification) {
        diagnostics.record("text-changed length=\(searchField.stringValue.count)")
        scheduleFileSearch()
        updateFilter()
    }

    private func scheduleFileSearch() {
        fileSearchWorkItem?.cancel()
        fileSearchWorkItem = nil
        spotlight?.cancel()
        shouldAcceptAfterFileSearch = false
        spotlightCandidates = []
        fileSearchError = nil
        isSearchingFiles = false

        let query = searchField.stringValue
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard query.count >= 2 else {
            return
        }
        guard let spotlight else {
            fileSearchError = spotlightLoadError ?? JumpError.spotlightUnavailable
            return
        }

        isSearchingFiles = true
        let workItem = DispatchWorkItem { [weak self, weak spotlight] in
            guard let self, let spotlight else { return }
            spotlight.search(query: query) { [weak self] result in
                guard let self,
                      self.searchField.stringValue
                        .trimmingCharacters(in: .whitespacesAndNewlines) == query else {
                    return
                }
                self.isSearchingFiles = false
                switch result {
                case let .success(candidates):
                    self.spotlightCandidates = candidates
                    self.fileSearchError = nil
                case let .failure(error):
                    self.spotlightCandidates = []
                    self.fileSearchError = error
                }
                self.updateFilter()
                if self.shouldAcceptAfterFileSearch {
                    self.shouldAcceptAfterFileSearch = false
                    self.acceptSelection()
                }
            }
        }
        fileSearchWorkItem = workItem
        DispatchQueue.main.asyncAfter(
            deadline: .now() + 0.075,
            execute: workItem
        )
    }

    private func updateFilter() {
        filteredCandidates = CandidateFilter.matches(
            folderCandidates + spotlightCandidates,
            query: searchField.stringValue
        )
        tableView.reloadData()
        if !filteredCandidates.isEmpty {
            tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
        }
        updateStatus()
    }

    private func updateStatus() {
        let query = searchField.stringValue
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let folderCount = filteredCandidates.reduce(into: 0) { count, candidate in
            if candidate.kind == .folder {
                count += 1
            }
        }
        let fileCount = filteredCandidates.count - folderCount
        let navigationHelp = "↑↓ select  ·  Return jump  ·  Esc close"

        if isSearchingFiles {
            statusLabel.stringValue =
                "\(folderCount) folders  ·  Searching folders and files…  ·  \(navigationHelp)"
        } else if let fileSearchError {
            statusLabel.stringValue =
                "\(folderCount) folders  ·  \(fileSearchError.localizedDescription)"
        } else if query.count < 2 {
            if filteredCandidates.isEmpty, let zoxideLoadError {
                statusLabel.stringValue =
                    "\(zoxideLoadError.localizedDescription) Type 2+ characters to search folders and files."
            } else {
                statusLabel.stringValue =
                    "\(folderCount) folders  ·  Type 2+ characters to include unvisited folders and files  ·  \(navigationHelp)"
            }
        } else if filteredCandidates.isEmpty {
            statusLabel.stringValue = "No matching folder or file"
        } else {
            statusLabel.stringValue =
                "\(folderCount) folders  ·  \(fileCount) files  ·  \(navigationHelp)"
        }
    }

    func numberOfRows(in tableView: NSTableView) -> Int {
        filteredCandidates.count
    }

    func tableView(
        _ tableView: NSTableView,
        viewFor tableColumn: NSTableColumn?,
        row: Int
    ) -> NSView? {
        guard filteredCandidates.indices.contains(row) else {
            return nil
        }

        let identifier = NSUserInterfaceItemIdentifier("folderCell")
        let cell: NSTableCellView
        if let reusable = tableView.makeView(withIdentifier: identifier, owner: self)
            as? NSTableCellView {
            cell = reusable
        } else {
            cell = NSTableCellView()
            cell.identifier = identifier

            let kindImage = NSImageView()
            kindImage.identifier = NSUserInterfaceItemIdentifier("kind")
            kindImage.imageScaling = .scaleProportionallyDown
            kindImage.translatesAutoresizingMaskIntoConstraints = false

            let nameLabel = NSTextField(labelWithString: "")
            nameLabel.identifier = NSUserInterfaceItemIdentifier("name")
            nameLabel.font = .systemFont(ofSize: 15, weight: .medium)
            nameLabel.lineBreakMode = .byTruncatingTail
            nameLabel.translatesAutoresizingMaskIntoConstraints = false

            let pathLabel = NSTextField(labelWithString: "")
            pathLabel.identifier = NSUserInterfaceItemIdentifier("path")
            pathLabel.font = .systemFont(ofSize: 11)
            pathLabel.textColor = .secondaryLabelColor
            pathLabel.lineBreakMode = .byTruncatingMiddle
            pathLabel.translatesAutoresizingMaskIntoConstraints = false

            cell.addSubview(kindImage)
            cell.addSubview(nameLabel)
            cell.addSubview(pathLabel)
            NSLayoutConstraint.activate([
                kindImage.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 10),
                kindImage.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                kindImage.widthAnchor.constraint(equalToConstant: 22),
                kindImage.heightAnchor.constraint(equalToConstant: 22),
                nameLabel.topAnchor.constraint(equalTo: cell.topAnchor, constant: 5),
                nameLabel.leadingAnchor.constraint(equalTo: kindImage.trailingAnchor, constant: 9),
                nameLabel.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -10),
                pathLabel.topAnchor.constraint(equalTo: nameLabel.bottomAnchor, constant: 2),
                pathLabel.leadingAnchor.constraint(equalTo: nameLabel.leadingAnchor),
                pathLabel.trailingAnchor.constraint(equalTo: nameLabel.trailingAnchor),
            ])
        }

        let candidate = filteredCandidates[row]
        let isFolder = candidate.kind == .folder
        if let kindImage = cell.subviews.first(
            where: { $0.identifier?.rawValue == "kind" }
        ) as? NSImageView {
            kindImage.image = nativeIcon(for: candidate)
            kindImage.contentTintColor = nil
            kindImage.setAccessibilityLabel(isFolder ? "Folder" : "File")
        }
        (cell.subviews.first { $0.identifier?.rawValue == "name" } as? NSTextField)?
            .stringValue = candidate.name
        (cell.subviews.first { $0.identifier?.rawValue == "path" } as? NSTextField)?
            .stringValue = candidate.displayPath
        return cell
    }

    private func nativeIcon(for candidate: JumpCandidate) -> NSImage {
        let cacheKey = candidate.path as NSString
        if let cached = iconCache.object(forKey: cacheKey) {
            return cached
        }

        let workspaceIcon = NSWorkspace.shared.icon(forFile: candidate.path)
        let icon = (workspaceIcon.copy() as? NSImage) ?? workspaceIcon
        icon.size = NSSize(width: 20, height: 20)
        iconCache.setObject(icon, forKey: cacheKey)
        return icon
    }

    private func handleKey(_ event: NSEvent) -> Bool {
        let now = ProcessInfo.processInfo.systemUptime
        let hasMarkedText = (searchField.currentEditor() as? NSTextView)?
            .hasMarkedText() ?? false
        let characters = event.charactersIgnoringModifiers?
            .unicodeScalars
            .map { String(format: "%04X", $0.value) }
            .joined(separator: ",") ?? "none"
        diagnostics.record(
            "key code=\(event.keyCode) chars=\(characters) flags=\(event.modifierFlags.rawValue) marked=\(hasMarkedText)"
        )
        if event.keyCode == PaletteEscapeGate.markerKeyCode {
            escapeGate.markPhysicalEscape(at: now)
            diagnostics.record("escape-marker-armed")
            return true
        }

        if event.keyCode == PaletteKeyCode.selectAll
            || (event.modifierFlags.contains(.command)
                && event.charactersIgnoringModifiers?.lowercased() == "a") {
            panel.makeFirstResponder(searchField)
            searchField.selectText(nil)
            diagnostics.record("search-select-all")
            return true
        }

        if hasMarkedText {
            if event.keyCode == 53 {
                escapeGate.clear()
                diagnostics.record("marked-escape-forwarded")
            }
            return false
        }

        if event.keyCode == 53 {
            if !escapeGate.consumePhysicalEscape(at: now) {
                diagnostics.record("unmarked-escape-consumed")
                return true
            }
            diagnostics.record("physical-escape-terminate")
            NSApp.terminate(nil)
            return true
        }
        if event.keyCode == 51 || event.keyCode == 117 {
            if searchField.currentEditor() == nil {
                panel.makeFirstResponder(searchField)
            }
            return false
        }
        if event.keyCode == 125
            || (event.modifierFlags.contains(.control) && event.charactersIgnoringModifiers == "j") {
            moveSelection(by: 1)
            return true
        }
        if event.keyCode == 126
            || (event.modifierFlags.contains(.control) && event.charactersIgnoringModifiers == "k") {
            moveSelection(by: -1)
            return true
        }
        if event.keyCode == 36 || event.keyCode == 76 {
            diagnostics.record("return-accept-requested")
            acceptSelection()
            return true
        }
        return false
    }

    private func moveSelection(by delta: Int) {
        guard !filteredCandidates.isEmpty else {
            return
        }
        let current = tableView.selectedRow >= 0 ? tableView.selectedRow : 0
        let next = min(max(current + delta, 0), filteredCandidates.count - 1)
        tableView.selectRowIndexes(IndexSet(integer: next), byExtendingSelection: false)
        tableView.scrollRowToVisible(next)
    }

    @objc private func acceptSelection() {
        if isSearchingFiles {
            diagnostics.record("accept-deferred-for-file-search")
            shouldAcceptAfterFileSearch = true
            statusLabel.stringValue = "Searching folders and files…"
            return
        }
        let row = tableView.selectedRow
        guard filteredCandidates.indices.contains(row) else {
            diagnostics.record("accept-rejected-no-row")
            NSSound.beep()
            return
        }

        let candidate = filteredCandidates[row]
        diagnostics.record("accept-start kind=\(candidate.kind == .folder ? "folder" : "file")")
        isCompletingSelection = true
        do {
            try JumpCompletion.navigateAndLearn(candidate: candidate, zoxide: zoxide)
            panel.orderOut(nil)
            diagnostics.record("accept-success-terminate")
            NSApp.terminate(nil)
        } catch {
            diagnostics.record("accept-failed")
            isCompletingSelection = false
            statusLabel.stringValue = error.localizedDescription
            panel.makeKeyAndOrderFront(nil)
            panel.makeFirstResponder(searchField)
        }
    }
}
