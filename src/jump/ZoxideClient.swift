import Foundation

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
