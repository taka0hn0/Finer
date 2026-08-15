import AppKit

extension JumpController {
    func configurePanel() {
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
}
