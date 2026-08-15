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
