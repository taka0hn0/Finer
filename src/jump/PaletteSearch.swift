import AppKit

extension JumpController {
    func loadCandidates() {
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
}
