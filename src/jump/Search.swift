import AppKit
import Darwin
import Foundation

func notifyKeystrokeJumpKey() {
    let path = "/tmp/keystroke-finer-\(getuid()).sock"
    guard access(path, F_OK) == 0,
          path.utf8.count < MemoryLayout.size(ofValue: sockaddr_un().sun_path)
    else { return }

    let descriptor = socket(AF_UNIX, SOCK_DGRAM, 0)
    guard descriptor >= 0 else { return }
    defer { close(descriptor) }

    var address = sockaddr_un()
    address.sun_family = sa_family_t(AF_UNIX)
    withUnsafeMutableBytes(of: &address.sun_path) { buffer in
        buffer.initializeMemory(as: UInt8.self, repeating: 0)
        _ = path.utf8CString.withUnsafeBytes { source in
            source.copyBytes(to: buffer)
        }
    }
    let addressLength = socklen_t(
        MemoryLayout.offset(of: \sockaddr_un.sun_path)! + path.utf8.count + 1
    )
    let message = "6 0 -1 0\n"
    message.withCString { bytes in
        withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                _ = sendto(
                    descriptor,
                    bytes,
                    strlen(bytes),
                    MSG_DONTWAIT,
                    socketAddress,
                    addressLength
                )
            }
        }
    }
}

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

struct ZoxideClient {
    let executableURL: URL

    static func discover() throws -> ZoxideClient {
        let environment = ProcessInfo.processInfo.environment
        if let override = environment["FINER_ZOXIDE_PATH"], !override.isEmpty {
            let url = URL(fileURLWithPath: override)
            guard FileManager.default.isExecutableFile(atPath: url.path) else {
                throw JumpError.zoxideUnavailable
            }
            return ZoxideClient(executableURL: url)
        }

        let fixedCandidates = [
            "/opt/homebrew/bin/zoxide",
            "/usr/local/bin/zoxide",
        ]
        for path in fixedCandidates where FileManager.default.isExecutableFile(atPath: path) {
            return ZoxideClient(executableURL: URL(fileURLWithPath: path))
        }

        for directory in (environment["PATH"] ?? "").split(separator: ":") {
            let path = URL(fileURLWithPath: String(directory))
                .appendingPathComponent("zoxide").path
            if FileManager.default.isExecutableFile(atPath: path) {
                return ZoxideClient(executableURL: URL(fileURLWithPath: path))
            }
        }
        throw JumpError.zoxideUnavailable
    }

    func candidates() throws -> [JumpCandidate] {
        let output = try run(arguments: ["query", "--list", "--score"])
        return output
            .split(whereSeparator: \.isNewline)
            .compactMap { line in
                let text = String(line).trimmingCharacters(in: .whitespaces)
                guard let separator = text.firstIndex(where: \.isWhitespace) else {
                    return nil
                }
                let scoreText = String(text[..<separator])
                let path = String(text[separator...])
                    .trimmingCharacters(in: .whitespaces)
                guard let score = Double(scoreText), !path.isEmpty else {
                    return nil
                }
                return JumpCandidate(
                    path: path,
                    kind: .folder,
                    sourceScore: score
                )
            }
    }

    func add(path: String) {
        _ = try? run(arguments: ["add", path])
    }

    private func run(arguments: [String]) throws -> String {
        let process = Process()
        let stdout = Pipe()
        let stderr = Pipe()
        process.executableURL = executableURL
        process.arguments = arguments
        process.standardOutput = stdout
        process.standardError = stderr

        do {
            try process.run()
        } catch {
            throw JumpError.zoxideFailed(error.localizedDescription)
        }
        process.waitUntilExit()

        let outputData = stdout.fileHandleForReading.readDataToEndOfFile()
        let errorData = stderr.fileHandleForReading.readDataToEndOfFile()
        guard process.terminationStatus == 0 else {
            let message = String(data: errorData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw JumpError.zoxideFailed(
                message?.isEmpty == false ? message! : "exit \(process.terminationStatus)"
            )
        }
        return String(data: outputData, encoding: .utf8) ?? ""
    }
}

final class SpotlightClient {
    private static let resultLimit = 200

    private let executableURL: URL
    private let searchRoot: String
    private let queue = DispatchQueue(label: "dev.finer.spotlight-search")
    private let lock = NSLock()
    private var requestIdentifier = 0
    private var activeProcess: Process?

    init(executableURL: URL, searchRoot: String) {
        self.executableURL = executableURL
        self.searchRoot = searchRoot
    }

    static func discover() throws -> SpotlightClient {
        let environment = ProcessInfo.processInfo.environment
        let executablePath = environment["FINER_SPOTLIGHT_PATH"] ?? "/usr/bin/mdfind"
        guard FileManager.default.isExecutableFile(atPath: executablePath) else {
            throw JumpError.spotlightUnavailable
        }
        let root = environment["FINER_SPOTLIGHT_ROOT"]
            ?? FileManager.default.homeDirectoryForCurrentUser.path
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(
            atPath: root,
            isDirectory: &isDirectory
        ), isDirectory.boolValue else {
            throw JumpError.spotlightUnavailable
        }
        return SpotlightClient(
            executableURL: URL(fileURLWithPath: executablePath),
            searchRoot: root
        )
    }

    func search(
        query: String,
        completion: @escaping (Result<[JumpCandidate], Error>) -> Void
    ) {
        let identifier: Int
        let previousProcess: Process?
        lock.lock()
        requestIdentifier += 1
        identifier = requestIdentifier
        previousProcess = activeProcess
        activeProcess = nil
        lock.unlock()
        previousProcess?.terminate()

        queue.async { [weak self] in
            guard let self else { return }
            do {
                let candidates = try self.run(query: query, identifier: identifier)
                DispatchQueue.main.async {
                    guard self.isCurrent(identifier) else { return }
                    completion(.success(candidates))
                }
            } catch JumpError.searchCancelled {
                return
            } catch {
                DispatchQueue.main.async {
                    guard self.isCurrent(identifier) else { return }
                    completion(.failure(error))
                }
            }
        }
    }

    func candidates(query: String) throws -> [JumpCandidate] {
        let identifier: Int
        let previousProcess: Process?
        lock.lock()
        requestIdentifier += 1
        identifier = requestIdentifier
        previousProcess = activeProcess
        activeProcess = nil
        lock.unlock()
        previousProcess?.terminate()
        return try run(query: query, identifier: identifier)
    }

    func cancel() {
        let process: Process?
        lock.lock()
        requestIdentifier += 1
        process = activeProcess
        activeProcess = nil
        lock.unlock()
        process?.terminate()
    }

    private func run(query: String, identifier: Int) throws -> [JumpCandidate] {
        guard isCurrent(identifier) else {
            throw JumpError.searchCancelled
        }
        let trimmedQuery = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmedQuery.count >= 2 else {
            return []
        }

        let process = Process()
        let stdout = Pipe()
        let stderr = Pipe()
        process.executableURL = executableURL
        process.arguments = [
            "-0",
            "-onlyin", searchRoot,
            "-name", trimmedQuery,
        ]
        process.standardOutput = stdout
        process.standardError = stderr

        lock.lock()
        guard identifier == requestIdentifier else {
            lock.unlock()
            throw JumpError.searchCancelled
        }
        activeProcess = process
        lock.unlock()

        do {
            try process.run()
        } catch {
            clearActiveProcess(process, identifier: identifier)
            throw JumpError.spotlightFailed(error.localizedDescription)
        }

        let outputData = stdout.fileHandleForReading.readDataToEndOfFile()
        let errorData = stderr.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        clearActiveProcess(process, identifier: identifier)

        guard isCurrent(identifier) else {
            throw JumpError.searchCancelled
        }
        guard process.terminationStatus == 0 else {
            let message = String(data: errorData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw JumpError.spotlightFailed(
                message?.isEmpty == false
                    ? message!
                    : "mdfind exited with \(process.terminationStatus)"
            )
        }

        var candidates: [JumpCandidate] = []
        var seenPaths = Set<String>()
        for pathData in outputData.split(separator: 0, omittingEmptySubsequences: true) {
            guard candidates.count < Self.resultLimit,
                  let path = String(data: pathData, encoding: .utf8),
                  !path.isEmpty,
                  seenPaths.insert(path).inserted else {
                continue
            }
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(
                atPath: path,
                isDirectory: &isDirectory
            ) else {
                continue
            }
            candidates.append(
                JumpCandidate(
                    path: path,
                    kind: isDirectory.boolValue ? .folder : .file,
                    sourceScore: Double(Self.resultLimit - candidates.count)
                        / Double(Self.resultLimit)
                )
            )
        }
        return candidates
    }

    private func isCurrent(_ identifier: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return identifier == requestIdentifier
    }

    private func clearActiveProcess(_ process: Process, identifier: Int) {
        lock.lock()
        if identifier == requestIdentifier, activeProcess === process {
            activeProcess = nil
        }
        lock.unlock()
    }
}

enum CandidateFilter {
    static func matches(_ candidates: [JumpCandidate], query: String) -> [JumpCandidate] {
        let candidates = deduplicated(candidates)
        let normalizedQuery = normalize(query)
        guard !normalizedQuery.isEmpty else {
            return candidates.sorted { $0.sourceScore > $1.sourceScore }
        }

        let tokens = normalizedQuery.split(whereSeparator: \.isWhitespace).map(String.init)
        return candidates.compactMap { candidate -> (JumpCandidate, Double)? in
            let normalizedPath = normalize(candidate.path)
            let normalizedName = normalize(candidate.name)
            guard tokens.allSatisfy({ normalizedPath.contains($0) }) else {
                return nil
            }

            var score = log2(max(candidate.sourceScore, 0) + 1)
            for token in tokens {
                if normalizedName == token {
                    score += 1_000
                } else if normalizedName.hasPrefix(token) {
                    score += 500
                } else if normalizedName.contains(token) {
                    score += 250
                } else {
                    score += 50
                }
                if let range = normalizedPath.range(of: token) {
                    score += Double(normalizedPath.distance(from: range.lowerBound, to: normalizedPath.endIndex)) / 10_000
                }
            }
            return (candidate, score)
        }
        .sorted {
            if $0.1 == $1.1 {
                return $0.0.sourceScore > $1.0.sourceScore
            }
            return $0.1 > $1.1
        }
        .map(\.0)
    }

    private static func deduplicated(_ candidates: [JumpCandidate]) -> [JumpCandidate] {
        var seenPaths = Set<String>()
        return candidates.filter { candidate in
            let path = (candidate.path as NSString).standardizingPath
            return seenPaths.insert(path).inserted
        }
    }

    private static func normalize(_ value: String) -> String {
        value.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: .current)
    }
}
