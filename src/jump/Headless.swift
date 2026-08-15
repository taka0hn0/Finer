import Foundation

func runHeadless(arguments: [String]) -> Int32? {
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
    if first == "--navigate-and-learn-folder",
       let path = arguments.dropFirst().first {
        do {
            let zoxide = try? ZoxideClient.discover()
            try JumpCompletion.navigateAndLearn(
                candidate: JumpCandidate(path: path, kind: .folder, sourceScore: 0),
                zoxide: zoxide
            )
            return 0
        } catch {
            FileHandle.standardError.write(
                Data("finer_jump: \(error.localizedDescription)\n".utf8)
            )
            return 1
        }
    }
    if first == "--navigate-and-learn-file",
       let path = arguments.dropFirst().first {
        do {
            let zoxide = try? ZoxideClient.discover()
            try JumpCompletion.navigateAndLearn(
                candidate: JumpCandidate(path: path, kind: .file, sourceScore: 0),
                zoxide: zoxide
            )
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
