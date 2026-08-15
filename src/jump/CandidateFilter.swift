import Foundation

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
