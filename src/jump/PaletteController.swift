import AppKit

final class JumpController: NSObject, NSApplicationDelegate,
    NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate
{
    let zoxide: ZoxideClient?
    let spotlight: SpotlightClient?
    let zoxideLoadError: Error?
    let spotlightLoadError: Error?
    var folderCandidates: [JumpCandidate] = []
    var spotlightCandidates: [JumpCandidate] = []
    var filteredCandidates: [JumpCandidate] = []
    var fileSearchWorkItem: DispatchWorkItem?
    var fileSearchError: Error?
    var isSearchingFiles = false
    var shouldAcceptAfterFileSearch = false
    var inputSourceObserver: NSObjectProtocol?
    var isCompletingSelection = false
    var escapeGate = PaletteEscapeGate()
    let iconCache = NSCache<NSString, NSImage>()
    let diagnostics = JumpDiagnostics()

    let panel: JumpPanel
    let searchField = NSTextField()
    let tableView = NSTableView()
    let statusLabel = NSTextField(labelWithString: "")

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

}
