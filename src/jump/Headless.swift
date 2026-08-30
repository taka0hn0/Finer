import Foundation

// Commands used by the headless tests and by callers that already know which
// item they want, so no palette is shown. Returning nil means the argument was
// not a headless command and the AppKit palette should start instead.

// Every headless command reports the same way: nothing on stdout for a failure,
// the reason on stderr, and a non-zero exit status.
private func runReporting(_ body: () throws -> Void) -> Int32 {
    do {
        try body()
        return 0
    } catch {
        FileHandle.standardError.write(
            Data("finer_jump: \(error.localizedDescription)\n".utf8)
        )
        return 1
    }
}

func runHeadless(arguments: [String]) -> Int32? {
    guard let first = arguments.first else { return nil }
    let value = arguments.dropFirst().first

    switch first {
    case "--self-test-palette-escape":
        return runPaletteEscapeSelfTest()

    case "--query":
        return runReporting {
            let zoxide = try ZoxideClient.discover()
            let query = value ?? ""
            for candidate in CandidateFilter.matches(
                try zoxide.candidates(),
                query: query
            ) {
                print(candidate.path)
            }
        }

    case "--query-files":
        return runReporting {
            let spotlight = try SpotlightClient.discover()
            let query = value ?? ""
            for candidate in CandidateFilter.matches(
                try spotlight.candidates(query: query),
                query: query
            ) {
                print(candidate.path)
            }
        }

    case "--query-all":
        return runReporting {
            let query = value ?? ""
            // zoxide history is optional here: without it the palette still
            // answers from Spotlight, which is what this command reports.
            let folders = (try? ZoxideClient.discover().candidates()) ?? []
            let files = try SpotlightClient.discover().candidates(query: query)
            for candidate in CandidateFilter.matches(folders + files, query: query) {
                let kind = candidate.kind == .folder ? "folder" : "file"
                print("\(kind)\t\(candidate.path)")
            }
        }

    case "--navigate":
        guard let path = value else { return nil }
        return runReporting { try FinderNavigator.navigate(to: path) }

    case "--navigate-file":
        guard let path = value else { return nil }
        return runReporting { try FinderNavigator.reveal(file: path) }

    case "--navigate-and-learn-folder":
        guard let path = value else { return nil }
        return runReporting {
            try JumpCompletion.navigateAndLearn(
                candidate: JumpCandidate(path: path, kind: .folder, sourceScore: 0),
                zoxide: try? ZoxideClient.discover()
            )
        }

    case "--navigate-and-learn-file":
        guard let path = value else { return nil }
        return runReporting {
            try JumpCompletion.navigateAndLearn(
                candidate: JumpCandidate(path: path, kind: .file, sourceScore: 0),
                zoxide: try? ZoxideClient.discover()
            )
        }

    default:
        return nil
    }
}

// Exercises the Escape gate without a window: an unmarked Escape is consumed,
// a marked one within its lifetime terminates, and a marker cannot be reused
// past its deadline or after an explicit clear.
private func runPaletteEscapeSelfTest() -> Int32 {
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
