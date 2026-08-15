import AppKit

extension JumpController {
    @objc func acceptSelection() {
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
