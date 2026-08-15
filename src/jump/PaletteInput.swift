import AppKit

extension JumpController {
    func handleKey(_ event: NSEvent) -> Bool {
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
}
