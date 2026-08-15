import Darwin
import Foundation

@main
enum FinderAXMoveCommand {
    static func main() {
        do {
            print(try run())
        } catch {
            fputs("finder_ax_move: \(error)\n", stderr)
            exit(1)
        }
    }
}
