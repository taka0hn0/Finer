import Foundation

enum JumpCompletion {
    static func navigateAndLearn(
        candidate: JumpCandidate,
        zoxide: ZoxideClient?
    ) throws {
        try FinderNavigator.navigate(to: candidate)
        let visitedFolder = candidate.kind == .folder
            ? candidate.path
            : URL(fileURLWithPath: candidate.path)
                .deletingLastPathComponent().path
        zoxide?.add(path: visitedFolder)
    }
}
