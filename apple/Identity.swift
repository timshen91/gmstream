// This device's key, and the hosts it has met. A host admits a device by its
// public key in ~/.ssh/authorized_keys; a device remembers each host's key the
// first time and refuses it if it changes, as ssh does (src/auth.rs).

import CryptoKit
import Foundation

final class Identity {
    static let shared = Identity()
    let key: Curve25519.Signing.PrivateKey

    /// A file of the owner's alone, like ~/.ssh/id_ed25519.
    init() {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("GMStream", isDirectory: true)
        let file = dir.appendingPathComponent("device.key")
        if let raw = try? Data(contentsOf: file), let k = try? Curve25519.Signing.PrivateKey(rawRepresentation: raw) {
            key = k
            return
        }
        key = Curve25519.Signing.PrivateKey()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: file.path, contents: key.rawRepresentation, attributes: [.posixPermissions: 0o600])
    }

    /// The line for the host's ~/.ssh/authorized_keys. It admits this device to
    /// gmstream and gives no shell: ssh itself runs nothing with this key.
    var authorizedLine: String {
        var blob = Writer()
        let name = Data("ssh-ed25519".utf8)
        blob.data.append(contentsOf: withUnsafeBytes(of: UInt32(name.count).bigEndian) { Array($0) })
        blob.data.append(name)
        let pub = key.publicKey.rawRepresentation
        blob.data.append(contentsOf: withUnsafeBytes(of: UInt32(pub.count).bigEndian) { Array($0) })
        blob.data.append(pub)
        let device = ProcessInfo.processInfo.hostName.split(separator: ".").first.map(String.init) ?? "device"
        return "restrict,command=\"/bin/false\" ssh-ed25519 \(blob.data.base64EncodedString()) gmstream-\(device)"
    }

    /// Whether `key` is the one this host had before; the first is remembered.
    static func hostKeyMatches(host: String, key: Data) -> Bool {
        let d = UserDefaults.standard
        var known = d.dictionary(forKey: "hostKeys") as? [String: Data] ?? [:]
        if let k = known[host] { return k == key }
        known[host] = key
        d.set(known, forKey: "hostKeys")
        return true
    }
}
