import Darwin
import Foundation

// Best-effort notification of the key the user actually pressed, for the
// demo-recording overlay described in FR-DIAG-004.
//
// Every helper that consumes a key reports it the same way, so the datagram
// format lives here. Nothing waits for a reply and a missing socket is not an
// error: the Finder operation must not depend on anyone listening.
enum KeystrokeSocket {
    static func notify(
        keyCode: UInt16,
        modifiers: UInt64 = 0,
        suppressKeyCode: Int = -1,
        suppressModifiers: UInt64 = 0
    ) {
        let path = "/tmp/keystroke-finer-\(getuid()).sock"
        guard path.utf8.count < MemoryLayout.size(ofValue: sockaddr_un().sun_path),
              access(path, F_OK) == 0 else { return }

        let socketDescriptor = socket(AF_UNIX, SOCK_DGRAM, 0)
        guard socketDescriptor >= 0 else { return }
        defer { close(socketDescriptor) }

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
        let message = "\(keyCode) \(modifiers) \(suppressKeyCode) \(suppressModifiers)\n"
        message.withCString { bytes in
            withUnsafePointer(to: &address) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
                    _ = sendto(
                        socketDescriptor,
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
}
