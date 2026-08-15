import AppKit

extension JumpController {
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
}
