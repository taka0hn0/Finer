import Foundation

enum CandidateKind {
    case folder
    case file
}

struct JumpCandidate {
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

enum JumpError: LocalizedError {
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
            return "Could not search folders or files: \(message)"
        case .searchCancelled:
            return "Spotlight search was cancelled."
        case .noFinderWindow:
            return "Finder has no window to navigate."
        case let .navigationFailed(message):
            return "Finder navigation failed: \(message)"
        }
    }
}
