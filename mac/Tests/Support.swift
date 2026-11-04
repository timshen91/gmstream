import CryptoKit
import XCTest

/// A frame's timing with host times already in our clock (offset 0).
func timing(present: UInt64, encoded: UInt64? = nil, received: UInt64 = 0) -> FrameTiming {
    FrameTiming(slotNs: present, encodedNs: encoded ?? present, received: received) { $0 }
}

/// The host side of a plain UDP session on the loopback interface: receives
/// what a Connection sends and answers to the address it came from.
final class FakeHost {
    let fd: Int32
    let port: UInt16
    private var client: sockaddr_in?
    private var sendSeq: UInt32 = 0

    init() {
        let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        addr.sin_port = 0
        _ = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) } }
        self.fd = fd
        port = UInt16(bigEndian: addr.sin_port)
    }

    deinit { close(fd) }

    /// The next datagram, or nil after `timeout` seconds.
    func receive(timeout: Double = 2) -> Data? {
        var tv = timeval(tv_sec: Int(timeout), tv_usec: Int32((timeout - Double(Int(timeout))) * 1e6))
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var buf = [UInt8](repeating: 0, count: 65536)
        var from = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let n = withUnsafeMutablePointer(to: &from) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buf, buf.count, 0, $0, &len) }
        }
        guard n > 0 else { return nil }
        client = from
        return Data(buf[0..<n])
    }

    /// The next control datagram (type 3), opened, skipping keep-awake ones and
    /// answering handshakes.
    func receiveControl(timeout: Double = 2) -> (seq: UInt32, payload: Data)? {
        while let d = receive(timeout: timeout) {
            if d[0] == datagramHandshake { answerHandshake(d) }
            if d[0] == datagramControl, let recv {
                let seq = d.u32(1)
                if autoAck {
                    sendAck(next: max(nextControl, seq + 1))
                    if seq < nextControl { continue } // sent again
                    nextControl = seq + 1
                }
                let box = try! AES.GCM.SealedBox(nonce: Self.nonce(0, seq), ciphertext: d.subdata(in: 5..<d.count - tagSize),
                                                 tag: d.subdata(in: d.count - tagSize..<d.count))
                return (seq, try! AES.GCM.open(box, using: recv, authenticating: d.subdata(in: 0..<5)))
            }
        }
        return nil
    }

    /// The next mouse datagram, opened: its number and payload.
    func receiveMouse(timeout: Double = 2) -> (seq: UInt32, payload: Data)? {
        while let d = receive(timeout: timeout) {
            if d[0] == datagramHandshake { answerHandshake(d) }
            if d[0] == datagramMouse, let recv {
                let box = try! AES.GCM.SealedBox(nonce: Self.nonce(1, d.u32(1)), ciphertext: d.subdata(in: 5..<d.count - tagSize),
                                                 tag: d.subdata(in: d.count - tagSize..<d.count))
                return (d.u32(1), try! AES.GCM.open(box, using: recv, authenticating: d.subdata(in: 0..<5)))
            }
        }
        return nil
    }

    /// The next sealed datagram of this type (nonce kind as in Connection.SeqKind).
    func receiveDatagram(_ type: UInt8, nonceKind: UInt8, timeout: Double = 2) -> (seq: UInt32, payload: Data)? {
        while let d = receive(timeout: timeout) {
            if d[0] == datagramHandshake { answerHandshake(d) }
            if d[0] == type, let recv {
                let box = try! AES.GCM.SealedBox(nonce: Self.nonce(nonceKind, d.u32(1)), ciphertext: d.subdata(in: 5..<d.count - tagSize),
                                                 tag: d.subdata(in: d.count - tagSize..<d.count))
                return (d.u32(1), try! AES.GCM.open(box, using: recv, authenticating: d.subdata(in: 0..<5)))
            }
        }
        return nil
    }

    /// Tells the client which control datagram comes next.
    func sendAck(next: UInt32) {
        var w = Writer()
        w.u8(datagramAck)
        w.u32(acks)
        var body = Writer()
        body.u32(next)
        let box = try! AES.GCM.seal(body.data, using: sendKey!, nonce: Self.nonce(2, acks), authenticating: w.data)
        acks += 1
        w.data.append(box.ciphertext)
        w.data.append(box.tag)
        send(w.data)
    }
    private var acks: UInt32 = 0
    /// As the real host does; off to see what happens without acknowledgements.
    var autoAck = true
    private var nextControl: UInt32 = 0

    /// What a host's key signed last, for checking the client's Auth.
    private(set) var transcript = Data()
    var signingKey = try! Curve25519.Signing.PrivateKey(rawRepresentation: Data(repeating: 7, count: 32))
    private var sendKey: SymmetricKey?, recv: SymmetricKey?
    private var answered: Data?, lastReply: Data?

    private static func nonce(_ kind: UInt8, _ seq: UInt32) -> AES.GCM.Nonce {
        var n = Data(count: 8)
        n[0] = kind
        withUnsafeBytes(of: seq.bigEndian) { n.append(contentsOf: $0) }
        return try! AES.GCM.Nonce(data: n)
    }

    /// As src/auth.rs does.
    private func answerHandshake(_ d: Data) {
        let theirs = d.subdata(in: 1..<33)
        if theirs == answered, let lastReply { return send(lastReply) }
        answered = theirs
        let ours = Curve25519.KeyAgreement.PrivateKey()
        let hostKey = signingKey.publicKey.rawRepresentation
        var hash = SHA256()
        for part in [Data("gmstream-udp-1".utf8), theirs, ours.publicKey.rawRepresentation, hostKey] { hash.update(data: part) }
        transcript = Data(hash.finalize())
        let shared = try! ours.sharedSecretFromKeyAgreement(with: Curve25519.KeyAgreement.PublicKey(rawRepresentation: theirs))
        func derive(_ info: String) -> SymmetricKey {
            shared.hkdfDerivedSymmetricKey(using: SHA256.self, salt: transcript, sharedInfo: Data(info.utf8), outputByteCount: 32)
        }
        sendKey = derive("h2c")
        recv = derive("c2h")
        var reply = Data([datagramHandshake])
        reply.append(ours.publicKey.rawRepresentation)
        reply.append(hostKey)
        reply.append(try! signingKey.signature(for: Data("gmstream host".utf8) + transcript))
        lastReply = reply
        send(reply)
    }

    func send(_ d: Data) {
        guard var to = client else { return XCTFail("the client has not sent anything yet") }
        _ = d.withUnsafeBytes { b in
            withUnsafePointer(to: &to) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(fd, b.baseAddress, b.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
        }
    }

    /// Control stream bytes for one message (type, length, payload).
    static func message(_ type: UInt8, _ payload: Data = Data()) -> Data {
        var w = Writer()
        w.u8(type)
        w.u32(UInt32(payload.count))
        w.data.append(payload)
        return w.data
    }

    /// A control datagram carrying `bytes` under sequence number `seq`.
    func sendControl(_ bytes: Data, seq: UInt32) {
        var w = Writer()
        w.u8(datagramControl)
        w.u32(seq)
        let box = try! AES.GCM.seal(bytes, using: sendKey!, nonce: Self.nonce(0, seq), authenticating: w.data)
        w.data.append(box.ciphertext)
        w.data.append(box.tag)
        send(w.data)
    }
}

/// Records what a Connection reports.
final class Recorder: ConnectionDelegate {
    private let lock = NSLock()
    private(set) var events: [String] = []
    private(set) var frames: [Frame] = []
    private(set) var packets: [PyroWavePacket] = []
    private(set) var audioPackets: [Data] = []
    var onEvent: (String) -> Void = { _ in }

    private func record(_ e: String) {
        lock.lock(); events.append(e); lock.unlock()
        onEvent(e)
    }

    func received(frame: Frame) { lock.lock(); frames.append(frame); lock.unlock(); onEvent("frame") }
    func audio(_ packet: Data) { lock.lock(); audioPackets.append(packet); lock.unlock(); onEvent("audio") }
    func pyrowave(_ p: PyroWavePacket) { lock.lock(); packets.append(p); lock.unlock(); onEvent("pyrowave") }
    func helloAck(codec: UInt8) { record("helloAck \(codec)") }
    func cursor(width: Int, height: Int, hotX: Int, hotY: Int, rgba: Data) {
        record("cursor \(width)x\(height) hot \(hotX),\(hotY) \([UInt8](rgba))")
    }
    func relative(_ on: Bool) { record("relative \(on)") }
    func cursorClip(_ clip: CGRect?) { record("cursorClip \(clip.map { "\($0)" } ?? "none")") }
    func rumble(index: Int, strong: UInt16, weak: UInt16, ms: Int) { record("rumble \(index) \(strong) \(weak) \(ms)") }
    func encodeFormat(_ text: String) {}
    func closed(reason: String) { record("closed \(reason)") }

    var snapshot: (events: [String], frames: [Frame], packets: [PyroWavePacket], audio: [Data]) {
        lock.lock(); defer { lock.unlock() }
        return (events, frames, packets, audioPackets)
    }
}
