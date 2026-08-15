import Foundation

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
