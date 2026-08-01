import AppKit
import Foundation

private enum CandidateKind {
    case folder
    case file
}

private struct JumpCandidate {
    let path: String
    let kind: CandidateKind
    let sourceScore: Double

    var name: String {
        let component = URL(fileURLWithPath: path).lastPathComponent
        return component.isEmpty ? path : component
    }

    var displayPath: String {
        switch kind {
        case .folder:
            return path
        case .file:
            return URL(fileURLWithPath: path).deletingLastPathComponent().path
        }
    }
}

private enum JumpError: LocalizedError {
    case zoxideUnavailable
    case zoxideFailed(String)
    case spotlightUnavailable
    case spotlightFailed(String)
    case searchCancelled
    case noFinderWindow
    case navigationFailed(String)

    var errorDescription: String? {
        switch self {
        case .zoxideUnavailable:
            return "zoxide was not found. Install it with Homebrew, then try again."
        case let .zoxideFailed(message):
            return "Could not read zoxide: \(message)"
        case .spotlightUnavailable:
            return "Spotlight search is unavailable."
        case let .spotlightFailed(message):
            return "Could not search files: \(message)"
        case .searchCancelled:
            return "File search was cancelled."
        case .noFinderWindow:
            return "Finder has no window to navigate."
        case let .navigationFailed(message):
            return "Finder navigation failed: \(message)"
        }
    }
}

private struct ZoxideClient {
    let executableURL: URL

    static func discover() throws -> ZoxideClient {
        let environment = ProcessInfo.processInfo.environment
        if let override = environment["FINER_ZOXIDE_PATH"], !override.isEmpty {
            let url = URL(fileURLWithPath: override)
            guard FileManager.default.isExecutableFile(atPath: url.path) else {
                throw JumpError.zoxideUnavailable
            }
            return ZoxideClient(executableURL: url)
        }

        let fixedCandidates = [
            "/opt/homebrew/bin/zoxide",
            "/usr/local/bin/zoxide",
        ]
        for path in fixedCandidates where FileManager.default.isExecutableFile(atPath: path) {
            return ZoxideClient(executableURL: URL(fileURLWithPath: path))
        }

        for directory in (environment["PATH"] ?? "").split(separator: ":") {
            let path = URL(fileURLWithPath: String(directory))
                .appendingPathComponent("zoxide").path
            if FileManager.default.isExecutableFile(atPath: path) {
                return ZoxideClient(executableURL: URL(fileURLWithPath: path))
            }
        }
        throw JumpError.zoxideUnavailable
    }

    func candidates() throws -> [JumpCandidate] {
        let output = try run(arguments: ["query", "--list", "--score"])
        return output
            .split(whereSeparator: \.isNewline)
            .compactMap { line in
                let text = String(line).trimmingCharacters(in: .whitespaces)
                guard let separator = text.firstIndex(where: \.isWhitespace) else {
                    return nil
                }
                let scoreText = String(text[..<separator])
                let path = String(text[separator...])
                    .trimmingCharacters(in: .whitespaces)
                guard let score = Double(scoreText), !path.isEmpty else {
                    return nil
                }
                return JumpCandidate(
                    path: path,
                    kind: .folder,
                    sourceScore: score
                )
            }
    }

    func add(path: String) {
        _ = try? run(arguments: ["add", path])
    }

    private func run(arguments: [String]) throws -> String {
        let process = Process()
        let stdout = Pipe()
        let stderr = Pipe()
        process.executableURL = executableURL
        process.arguments = arguments
        process.standardOutput = stdout
        process.standardError = stderr

        do {
            try process.run()
        } catch {
            throw JumpError.zoxideFailed(error.localizedDescription)
        }
        process.waitUntilExit()

        let outputData = stdout.fileHandleForReading.readDataToEndOfFile()
        let errorData = stderr.fileHandleForReading.readDataToEndOfFile()
        guard process.terminationStatus == 0 else {
            let message = String(data: errorData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw JumpError.zoxideFailed(
                message?.isEmpty == false ? message! : "exit \(process.terminationStatus)"
            )
        }
        return String(data: outputData, encoding: .utf8) ?? ""
    }
}

private final class SpotlightClient {
    private static let resultLimit = 200

    private let executableURL: URL
    private let searchRoot: String
    private let queue = DispatchQueue(label: "dev.finer.spotlight-search")
    private let lock = NSLock()
    private var requestIdentifier = 0
    private var activeProcess: Process?

    init(executableURL: URL, searchRoot: String) {
        self.executableURL = executableURL
        self.searchRoot = searchRoot
    }

    static func discover() throws -> SpotlightClient {
        let environment = ProcessInfo.processInfo.environment
        let executablePath = environment["FINER_SPOTLIGHT_PATH"] ?? "/usr/bin/mdfind"
        guard FileManager.default.isExecutableFile(atPath: executablePath) else {
            throw JumpError.spotlightUnavailable
        }
        let root = environment["FINER_SPOTLIGHT_ROOT"]
            ?? FileManager.default.homeDirectoryForCurrentUser.path
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(
            atPath: root,
            isDirectory: &isDirectory
        ), isDirectory.boolValue else {
            throw JumpError.spotlightUnavailable
        }
        return SpotlightClient(
            executableURL: URL(fileURLWithPath: executablePath),
            searchRoot: root
        )
    }

    func search(
        query: String,
        completion: @escaping (Result<[JumpCandidate], Error>) -> Void
    ) {
        let identifier: Int
        let previousProcess: Process?
        lock.lock()
        requestIdentifier += 1
        identifier = requestIdentifier
        previousProcess = activeProcess
        activeProcess = nil
        lock.unlock()
        previousProcess?.terminate()

        queue.async { [weak self] in
            guard let self else { return }
            do {
                let candidates = try self.run(query: query, identifier: identifier)
                DispatchQueue.main.async {
                    guard self.isCurrent(identifier) else { return }
                    completion(.success(candidates))
                }
            } catch JumpError.searchCancelled {
                return
            } catch {
                DispatchQueue.main.async {
                    guard self.isCurrent(identifier) else { return }
                    completion(.failure(error))
                }
            }
        }
    }

    func candidates(query: String) throws -> [JumpCandidate] {
        let identifier: Int
        let previousProcess: Process?
        lock.lock()
        requestIdentifier += 1
        identifier = requestIdentifier
        previousProcess = activeProcess
        activeProcess = nil
        lock.unlock()
        previousProcess?.terminate()
        return try run(query: query, identifier: identifier)
    }

    func cancel() {
        let process: Process?
        lock.lock()
        requestIdentifier += 1
        process = activeProcess
        activeProcess = nil
        lock.unlock()
        process?.terminate()
    }

    private func run(query: String, identifier: Int) throws -> [JumpCandidate] {
        guard isCurrent(identifier) else {
            throw JumpError.searchCancelled
        }
        let trimmedQuery = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmedQuery.count >= 2 else {
            return []
        }

        let process = Process()
        let stdout = Pipe()
        let stderr = Pipe()
        process.executableURL = executableURL
        process.arguments = [
            "-0",
            "-onlyin", searchRoot,
            "-name", trimmedQuery,
        ]
        process.standardOutput = stdout
        process.standardError = stderr

        lock.lock()
        guard identifier == requestIdentifier else {
            lock.unlock()
            throw JumpError.searchCancelled
        }
        activeProcess = process
        lock.unlock()

        do {
            try process.run()
        } catch {
            clearActiveProcess(process, identifier: identifier)
            throw JumpError.spotlightFailed(error.localizedDescription)
        }

        let outputData = stdout.fileHandleForReading.readDataToEndOfFile()
        let errorData = stderr.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        clearActiveProcess(process, identifier: identifier)

        guard isCurrent(identifier) else {
            throw JumpError.searchCancelled
        }
        guard process.terminationStatus == 0 else {
            let message = String(data: errorData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw JumpError.spotlightFailed(
                message?.isEmpty == false
                    ? message!
                    : "mdfind exited with \(process.terminationStatus)"
            )
        }

        var candidates: [JumpCandidate] = []
        var seenPaths = Set<String>()
        for pathData in outputData.split(separator: 0, omittingEmptySubsequences: true) {
            guard candidates.count < Self.resultLimit,
                  let path = String(data: pathData, encoding: .utf8),
                  !path.isEmpty,
                  seenPaths.insert(path).inserted else {
                continue
            }
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(
                atPath: path,
                isDirectory: &isDirectory
            ), !isDirectory.boolValue else {
                continue
            }
            candidates.append(
                JumpCandidate(
                    path: path,
                    kind: .file,
                    sourceScore: Double(Self.resultLimit - candidates.count)
                        / Double(Self.resultLimit)
                )
            )
        }
        return candidates
    }

    private func isCurrent(_ identifier: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return identifier == requestIdentifier
    }

    private func clearActiveProcess(_ process: Process, identifier: Int) {
        lock.lock()
        if identifier == requestIdentifier, activeProcess === process {
            activeProcess = nil
        }
        lock.unlock()
    }
}

private enum CandidateFilter {
    static func matches(_ candidates: [JumpCandidate], query: String) -> [JumpCandidate] {
        let normalizedQuery = normalize(query)
        guard !normalizedQuery.isEmpty else {
            return candidates.sorted { $0.sourceScore > $1.sourceScore }
        }

        let tokens = normalizedQuery.split(whereSeparator: \.isWhitespace).map(String.init)
        return candidates.compactMap { candidate -> (JumpCandidate, Double)? in
            let normalizedPath = normalize(candidate.path)
            let normalizedName = normalize(candidate.name)
            guard tokens.allSatisfy({ normalizedPath.contains($0) }) else {
                return nil
            }

            var score = log2(max(candidate.sourceScore, 0) + 1)
            for token in tokens {
                if normalizedName == token {
                    score += 1_000
                } else if normalizedName.hasPrefix(token) {
                    score += 500
                } else if normalizedName.contains(token) {
                    score += 250
                } else {
                    score += 50
                }
                if let range = normalizedPath.range(of: token) {
                    score += Double(normalizedPath.distance(from: range.lowerBound, to: normalizedPath.endIndex)) / 10_000
                }
            }
            return (candidate, score)
        }
        .sorted {
            if $0.1 == $1.1 {
                return $0.0.sourceScore > $1.0.sourceScore
            }
            return $0.1 > $1.1
        }
        .map(\.0)
    }

    private static func normalize(_ value: String) -> String {
        value.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: .current)
    }
}

private enum FinderNavigator {
    static func navigate(to candidate: JumpCandidate) throws {
        switch candidate.kind {
        case .folder:
            try navigate(to: candidate.path)
        case .file:
            try reveal(file: candidate.path)
        }
    }

    static func navigate(to path: String) throws {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory),
              isDirectory.boolValue else {
            throw JumpError.navigationFailed("the folder no longer exists")
        }

        if navigateDirectly(to: path) {
            return
        }

        try runHelper(arguments: ["jump-to", path])
    }

    static func reveal(file path: String) throws {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory),
              !isDirectory.boolValue else {
            throw JumpError.navigationFailed("the file no longer exists")
        }

        if revealDirectly(file: path) {
            return
        }

        try runHelper(arguments: ["reveal-file", path])
    }

    private static func runHelper(arguments: [String]) throws {
        let process = Process()
        let stderr = Pipe()
        process.executableURL = try helperURL()
        process.arguments = arguments
        process.standardOutput = FileHandle.nullDevice
        process.standardError = stderr
        do {
            try process.run()
        } catch {
            throw JumpError.navigationFailed(error.localizedDescription)
        }
        process.waitUntilExit()

        guard process.terminationStatus == 0 else {
            let errorData = stderr.fileHandleForReading.readDataToEndOfFile()
            let message = String(data: errorData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw JumpError.navigationFailed(
                message?.isEmpty == false
                    ? message!
                    : "finder_ax_move exited with \(process.terminationStatus)"
            )
        }
    }

    private static func navigateDirectly(to path: String) -> Bool {
        let environment = ProcessInfo.processInfo.environment
        let executablePath = environment["FINER_OSASCRIPT_PATH"]
            ?? "/usr/bin/osascript"
        guard FileManager.default.isExecutableFile(atPath: executablePath) else {
            return false
        }

        let script = """
        on run argv
            set destinationPath to item 1 of argv
            tell application "/System/Library/CoreServices/Finder.app"
                if (count of Finder windows) is 0 then error "No Finder window"
                set destinationFolder to POSIX file destinationPath as alias
                set target of front Finder window to destinationFolder
                activate
            end tell
        end run
        """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executablePath)
        process.arguments = ["-e", script, path]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
        }
    }

    private static func revealDirectly(file path: String) -> Bool {
        let environment = ProcessInfo.processInfo.environment
        let executablePath = environment["FINER_OSASCRIPT_PATH"]
            ?? "/usr/bin/osascript"
        guard FileManager.default.isExecutableFile(atPath: executablePath) else {
            return false
        }

        let parentPath = URL(fileURLWithPath: path)
            .deletingLastPathComponent().path
        let script = """
        on run argv
            set destinationPath to item 1 of argv
            set selectedPath to item 2 of argv
            tell application "/System/Library/CoreServices/Finder.app"
                if (count of Finder windows) is 0 then error "No Finder window"
                set destinationFolder to POSIX file destinationPath as alias
                set selectedFile to POSIX file selectedPath as alias
                set target of front Finder window to destinationFolder
                set selection to {selectedFile}
                activate
            end tell
        end run
        """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executablePath)
        process.arguments = ["-e", script, parentPath, path]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
        }
    }

    private static func helperURL() throws -> URL {
        let environment = ProcessInfo.processInfo.environment
        if let override = environment["FINER_AX_MOVE_PATH"],
           FileManager.default.isExecutableFile(atPath: override) {
            return URL(fileURLWithPath: override)
        }

        let sibling = URL(fileURLWithPath: CommandLine.arguments[0])
            .deletingLastPathComponent()
            .appendingPathComponent("finder_ax_move")
        guard FileManager.default.isExecutableFile(atPath: sibling.path) else {
            throw JumpError.navigationFailed("finder_ax_move was not found")
        }
        return sibling
    }
}

private final class JumpPanel: NSPanel {
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

private struct PaletteEscapeGate {
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

private enum PaletteKeyCode {
    static let selectAll: UInt16 = 80 // F19 on macOS.
}

private final class JumpDiagnostics {
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

private final class JumpController: NSObject, NSApplicationDelegate,
    NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate
{
    private let zoxide: ZoxideClient?
    private let spotlight: SpotlightClient?
    private let zoxideLoadError: Error?
    private let spotlightLoadError: Error?
    private var folderCandidates: [JumpCandidate] = []
    private var fileCandidates: [JumpCandidate] = []
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
        fileCandidates = []
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
                    self.fileCandidates = candidates
                    self.fileSearchError = nil
                case let .failure(error):
                    self.fileCandidates = []
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
            folderCandidates + fileCandidates,
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
                "\(folderCount) folders  ·  Searching files…  ·  \(navigationHelp)"
        } else if let fileSearchError {
            statusLabel.stringValue =
                "\(folderCount) folders  ·  \(fileSearchError.localizedDescription)"
        } else if query.count < 2 {
            if filteredCandidates.isEmpty, let zoxideLoadError {
                statusLabel.stringValue =
                    "\(zoxideLoadError.localizedDescription) Type 2+ characters to search files."
            } else {
                statusLabel.stringValue =
                    "\(folderCount) folders  ·  Type 2+ characters to include files  ·  \(navigationHelp)"
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
            statusLabel.stringValue = "Searching files…"
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
            try FinderNavigator.navigate(to: candidate)
            panel.orderOut(nil)
            let visitedFolder = candidate.kind == .folder
                ? candidate.path
                : URL(fileURLWithPath: candidate.path)
                    .deletingLastPathComponent().path
            zoxide?.add(path: visitedFolder)
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

private func runHeadless(arguments: [String]) -> Int32? {
    guard let first = arguments.first else {
        return nil
    }
    if first == "--self-test-palette-escape" {
        var gate = PaletteEscapeGate()
        guard !gate.consumePhysicalEscape(at: 1.0) else { return 1 }
        gate.markPhysicalEscape(at: 2.0)
        guard gate.consumePhysicalEscape(at: 2.05) else { return 1 }
        guard !gate.consumePhysicalEscape(at: 2.06) else { return 1 }
        gate.markPhysicalEscape(at: 3.0)
        guard !gate.consumePhysicalEscape(at: 3.11) else { return 1 }
        gate.markPhysicalEscape(at: 4.0)
        gate.clear()
        guard !gate.consumePhysicalEscape(at: 4.01) else { return 1 }
        return 0
    }
    if first == "--query" {
        let query = arguments.dropFirst().first ?? ""
        do {
            let zoxide = try ZoxideClient.discover()
            for candidate in CandidateFilter.matches(try zoxide.candidates(), query: query) {
                print(candidate.path)
            }
            return 0
        } catch {
            FileHandle.standardError.write(
                Data("finer_jump: \(error.localizedDescription)\n".utf8)
            )
            return 1
        }
    }
    if first == "--query-files" {
        let query = arguments.dropFirst().first ?? ""
        do {
            let spotlight = try SpotlightClient.discover()
            for candidate in CandidateFilter.matches(
                try spotlight.candidates(query: query),
                query: query
            ) {
                print(candidate.path)
            }
            return 0
        } catch {
            FileHandle.standardError.write(
                Data("finer_jump: \(error.localizedDescription)\n".utf8)
            )
            return 1
        }
    }
    if first == "--query-all" {
        let query = arguments.dropFirst().first ?? ""
        do {
            let folders = (try? ZoxideClient.discover().candidates()) ?? []
            let files = try SpotlightClient.discover().candidates(query: query)
            for candidate in CandidateFilter.matches(folders + files, query: query) {
                let kind = candidate.kind == .folder ? "folder" : "file"
                print("\(kind)\t\(candidate.path)")
            }
            return 0
        } catch {
            FileHandle.standardError.write(
                Data("finer_jump: \(error.localizedDescription)\n".utf8)
            )
            return 1
        }
    }
    if first == "--navigate", let path = arguments.dropFirst().first {
        do {
            try FinderNavigator.navigate(to: path)
            return 0
        } catch {
            FileHandle.standardError.write(
                Data("finer_jump: \(error.localizedDescription)\n".utf8)
            )
            return 1
        }
    }
    if first == "--navigate-file", let path = arguments.dropFirst().first {
        do {
            try FinderNavigator.reveal(file: path)
            return 0
        } catch {
            FileHandle.standardError.write(
                Data("finer_jump: \(error.localizedDescription)\n".utf8)
            )
            return 1
        }
    }
    return nil
}

private let arguments = Array(CommandLine.arguments.dropFirst())
if let exitCode = runHeadless(arguments: arguments) {
    exit(exitCode)
}

private let app = NSApplication.shared
private let controller = JumpController()
app.delegate = controller
app.setActivationPolicy(.accessory)
app.run()
