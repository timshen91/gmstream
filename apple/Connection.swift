// The gmstream protocol over plain UDP (PROTOCOL.md, "Plain UDP"): control
// messages as a byte stream in numbered datagrams, video as fragmented
// frames, audio datagrams. No retransmission. The control stream is encrypted,
// after a handshake that identifies both ends (src/auth.rs); video and audio are
// not.

import CryptoKit
import Foundation

enum Msg {
    // Client → host.
    static let hello: UInt8 = 0x01, configure: UInt8 = 0x02, keyframe: UInt8 = 0x03, bitrate: UInt8 = 0x04,
        key: UInt8 = 0x05, button: UInt8 = 0x06, motion: UInt8 = 0x07, position: UInt8 = 0x08,
        scroll: UInt8 = 0x09, gamepad: UInt8 = 0x0a, gamepadRemove: UInt8 = 0x0b, ping: UInt8 = 0x0c, displayGrid: UInt8 = 0x0d,
        vrrPolicy: UInt8 = 0x0e, touch: UInt8 = 0x0f, auth: UInt8 = 0x10
    // Host → client.
    static let helloAck: UInt8 = 0x81, cursor: UInt8 = 0x82, relative: UInt8 = 0x83,
        rumble: UInt8 = 0x84, pong: UInt8 = 0x85, error: UInt8 = 0x86, cursorClip: UInt8 = 0x87,
        encodeFormat: UInt8 = 0x88
}

let protocolVersion: UInt16 = 5
let datagramAudio: UInt8 = 1, datagramVideo: UInt8 = 2, datagramControl: UInt8 = 3, datagramPyroWave: UInt8 = 4,
    datagramAwake: UInt8 = 5
let maxDatagram = 1400
let datagramHandshake: UInt8 = 6
let datagramMouse: UInt8 = 7, datagramAck: UInt8 = 8
/// A piece of a frame, sent while the host is still encoding the rest.
let datagramVideoChunk: UInt8 = 9
let videoChunkHeader = 43
let handshakeSize = 129 // padded, so a forged request gets back no more than it sent
let tagSize = 16

/// Little-endian message building.
struct Writer {
    var data = Data()
    mutating func u8(_ v: UInt8) { data.append(v) }
    mutating func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func i16(_ v: Int16) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func u64(_ v: UInt64) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func f32(_ v: Float) { u32(v.bitPattern) }
}

extension Data {
    func u16(_ at: Int) -> UInt16 { UInt16(self[startIndex + at]) | UInt16(self[startIndex + at + 1]) << 8 }
    func i16(_ at: Int) -> Int16 { Int16(bitPattern: u16(at)) }
    func u32(_ at: Int) -> UInt32 { UInt32(u16(at)) | UInt32(u16(at + 2)) << 16 }
    func u64(_ at: Int) -> UInt64 { UInt64(u32(at)) | UInt64(u32(at + 4)) << 32 }
}

/// Monotonic nanoseconds on this machine.
func nowNs() -> UInt64 { clock_gettime_nsec_np(CLOCK_UPTIME_RAW) }

/// A frame gathered from the pieces the host sends while it encodes it
/// (PROTOCOL.md, Video chunks). The pieces carry where their bytes go, so the
/// frame ends up byte for byte as the encoder produced it; the host sends the
/// few bytes it had to go back and change in later pieces, which is why they
/// are laid down in index order.
private final class ChunkedFrame {
    var parts: [(offset: Int, data: Data)?] = []
    var got = 0
    var count = 0 // only the last piece knows it
    var total = 0
    var key = false
    var codec: UInt8 = 0
    var width = 0, height = 0
    var slotNs: UInt64 = 0, encodedNs: UInt64 = 0
    var overlapNs: UInt64 = 0 // sent before the bitstream was finished (host)
    var firstNs: UInt64 = 0 // when its first datagram reached the kernel

    func put(index: Int, offset: Int, data: Data) {
        if index >= parts.count {
            parts.append(contentsOf: [(offset: Int, data: Data)?](repeating: nil,
                                                                  count: index + 1 - parts.count))
        }
        guard parts[index] == nil else { return }
        parts[index] = (offset, data)
        got += 1
    }

    var whole: Bool { count > 0 && got >= count }

    /// The frame's bitstream, pieces laid down in index order.
    func bitstream() -> Data {
        var out = Data(count: total)
        out.withUnsafeMutableBytes { raw in
            guard let dst = raw.baseAddress else { return }
            for part in parts {
                guard let p = part, p.offset < total else { continue }
                let n = min(p.data.count, total - p.offset)
                p.data.withUnsafeBytes { src in
                    if let from = src.baseAddress {
                        memcpy(dst + p.offset, from, n)
                    }
                }
            }
        }
        return out
    }
}

/// One encoded frame as the host sent it (PROTOCOL.md, Video).
struct Frame {
    let id: UInt32
    let key: Bool
    let codec: UInt8
    let width: Int, height: Int
    let slotNs: UInt64, encodedNs: UInt64
    let bitstream: Data
    let receivedNs: UInt64
    /// When its first datagram arrived. With a frame sent in pieces as the
    /// host encodes it, that is well before the frame was finished, and
    /// receivedNs - firstNs is how long it took to arrive.
    var firstNs: UInt64 = 0
    /// How long before the bitstream was finished the host sent this frame's
    /// first piece (its own clock). The network's share of the latency runs
    /// from there, so this much of it happened during the encode.
    var overlapNs: UInt64 = 0
}

protocol ConnectionDelegate: AnyObject {
    /// On the network thread.
    func received(frame: Frame)
    /// An Opus packet, on the network thread.
    func audio(_ packet: Data)
    /// One PyroWave packet of a frame, on the network thread.
    func pyrowave(_ p: PyroWavePacket)
    /// The rest on the main thread.
    func helloAck(codec: UInt8)
    func cursor(width: Int, height: Int, hotX: Int, hotY: Int, rgba: Data)
    /// The game wants motion, not positions.
    func relative(_ on: Bool)
    /// Where the game keeps the cursor, in stream pixels; nil: nowhere in particular.
    func cursorClip(_ clip: CGRect?)
    func rumble(index: Int, strong: UInt16, weak: UInt16, ms: Int)
    /// What the host's encoder takes in, in words, for the stats line.
    func encodeFormat(_ text: String)
    func closed(reason: String)
}

final class Connection {
    weak var delegate: ConnectionDelegate?
    private let fd: Int32
    private let sendLock = NSLock()
    private var sendSeq: UInt32 = 0
    private let hostName: String
    private let ephemeral = Curve25519.KeyAgreement.PrivateKey()
    private var sendKey: SymmetricKey? // under sendLock
    private var recvKey: SymmetricKey? // receive thread only
    private var unsent: [Data] = []    // control bytes before the keys, under sendLock
    // Sent and not yet acknowledged, oldest first, under sendLock. The host holds
    // a stream back for a missing datagram, so they go again until it has them.
    private var unacked: [(seq: UInt32, datagram: Data, sentNs: UInt64)] = []
    private var resendTimer: DispatchSourceTimer?
    private static let resendAfterNs: UInt64 = 30_000_000
    private var mouseSeq: UInt32 = 0, mouseTotal = (x: 0.0, y: 0.0)
    private var lastMouse: Data?, mouseRepeated = true
    private var running = true
    private var started = false
    private var awakeTimer: DispatchSourceTimer? // under sendLock

    // Host → client control stream reassembly (network thread only).
    private var controlNext: UInt32 = 0
    private var controlPending: [UInt32: Data] = [:]
    private var controlBuffer = Data()

    // Video reassembly (network thread only).
    private var fragments: [UInt32: (parts: [Data?], got: Int, first: UInt64)] = [:]
    // Frames the host sends in pieces while it encodes them (type 9).
    private var chunked: [UInt32: ChunkedFrame] = [:]

    // Clock: host CLOCK_MONOTONIC ns minus ours, from the fastest ping.
    private let clockLock = NSLock()
    private var bestRtt = UInt64.max
    private var offsetNs: Int64?
    private(set) var rttNs: UInt64 = 0
    private var lastHeard = nowNs()

    /// Blocks while the name resolves (see resolve), so not on the main thread.
    init(host: String, port: UInt16) throws {
        hostName = host
        var addr = try Self.resolve(host: host, port: port)
        let family = addr.withUnsafeBytes { Int32($0.load(as: sockaddr.self).sa_family) }
        fd = socket(family, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw NSError(domain: "gmstream", code: Int(errno)) }
        // A refused send (e.g. EPIPE when macOS denies local network access)
        // is an error to report, not a signal that kills the app.
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        // A keyframe arrives as a burst of hundreds of datagrams.
        var size: Int32 = 8 << 20
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, socklen_t(MemoryLayout<Int32>.size))
        // Traffic class: interactive video (Wi-Fi's video queue). Measured:
        // trims this Mac's side of burst p99 (4.9 → 3.6 ms); the host would
        // have to mark its packets too for the downstream. GM_NET_SERVICE
        // overrides (4 voice, 5 responsive A/V, 0 best effort).
        var svc = ProcessInfo.processInfo.environment["GM_NET_SERVICE"].flatMap(Int32.init) ?? NET_SERVICE_TYPE_VI
        setsockopt(fd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &svc, socklen_t(MemoryLayout<Int32>.size))
        // Each datagram's arrival time from the kernel (mach time): what the
        // network did, apart from when this app got to read it.
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &on, socklen_t(MemoryLayout<Int32>.size))
        let rc = addr.withUnsafeMutableBytes { connect(fd, $0.baseAddress!.assumingMemoryBound(to: sockaddr.self), socklen_t($0.count)) }
        guard rc == 0 else {
            let e = errno
            close(fd)
            throw NSError(domain: "gmstream", code: Int(e), userInfo: [NSLocalizedDescriptionKey: "connect: \(String(cString: strerror(e)))"])
        }
        lastHeard = nowNs() // the host's silence counts from here, not from before resolving
        linkMaxDatagram = Self.linkMaxDatagram(fd: fd)
    }

    /// The largest datagram the link to the host carries unfragmented: the
    /// outgoing interface's MTU minus the IP and UDP headers (~64 KB over a
    /// Thunderbolt bridge set to MTU 65518, 1472 on Wi-Fi). The host sends
    /// PyroWave packets up to this size.
    private var linkMaxDatagram = maxDatagram

    private static func linkMaxDatagram(fd: Int32) -> Int {
        var local = sockaddr_storage()
        var len = socklen_t(MemoryLayout<sockaddr_storage>.size)
        let fallback = maxDatagram
        guard withUnsafeMutablePointer(to: &local, {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) }
        }) == 0 else { return fallback }
        let family = Int32(local.ss_family)
        var ifs: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifs) == 0 else { return fallback }
        defer { freeifaddrs(ifs) }
        // The interface holding our local address, then its link entry's MTU.
        var name: String?
        var mtus: [String: Int] = [:]
        for p in sequence(first: ifs, next: { $0?.pointee.ifa_next }) {
            guard let a = p?.pointee, let sa = a.ifa_addr else { continue }
            let ifname = String(cString: a.ifa_name)
            if Int32(sa.pointee.sa_family) == AF_LINK, let data = a.ifa_data {
                mtus[ifname] = Int(data.assumingMemoryBound(to: if_data.self).pointee.ifi_mtu)
            } else if Int32(sa.pointee.sa_family) == family, sameAddress(sa, &local) {
                name = ifname
            }
        }
        guard let name, let mtu = mtus[name] else { return fallback }
        let headers = family == AF_INET6 ? 48 : 28
        return max(fallback, min(mtu - headers, 65507))
    }

    private static func sameAddress(_ a: UnsafePointer<sockaddr>, _ b: inout sockaddr_storage) -> Bool {
        withUnsafePointer(to: &b) { bp in
            bp.withMemoryRebound(to: sockaddr.self, capacity: 1) { b in
                switch Int32(a.pointee.sa_family) {
                case AF_INET:
                    return a.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { x in
                        b.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { y in
                            x.pointee.sin_addr.s_addr == y.pointee.sin_addr.s_addr
                        }
                    }
                case AF_INET6:
                    return a.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { x in
                        b.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { y in
                            withUnsafeBytes(of: x.pointee.sin6_addr) { xa in
                                withUnsafeBytes(of: y.pointee.sin6_addr) { ya in xa.elementsEqual(ya) }
                            }
                        }
                    }
                default:
                    return false
                }
            }
        }
    }

    /// Never started (a newer connection attempt won): the receive loop that
    /// would close the socket never ran.
    deinit {
        if !started { close(fd) }
    }

    /// The host's address: IPv6 and IPv4 asked for at once, the first answer
    /// wins. getaddrinfo with AF_UNSPEC waits for both, and for a .local name
    /// whose host has no IPv6 on that link (a host over a Thunderbolt
    /// bridge) macOS gives up on the AAAA query only after 5 s.
    static func resolve(host: String, port: UInt16) throws -> Data {
        final class Race {
            let lock = NSLock(), done = DispatchSemaphore(value: 0)
            var found: Data?, failed = 0, error: Int32 = 0
        }
        let race = Race()
        for family in [AF_INET6, AF_INET] {
            DispatchQueue.global(qos: .userInitiated).async {
                var hints = addrinfo(ai_flags: 0, ai_family: family, ai_socktype: SOCK_DGRAM, ai_protocol: IPPROTO_UDP,
                                     ai_addrlen: 0, ai_canonname: nil, ai_addr: nil, ai_next: nil)
                var res: UnsafeMutablePointer<addrinfo>?
                let rc = getaddrinfo(host, String(port), &hints, &res)
                let addr = rc == 0 ? res.map { Data(bytes: $0.pointee.ai_addr, count: Int($0.pointee.ai_addrlen)) } : nil
                if res != nil { freeaddrinfo(res) }
                race.lock.lock()
                defer { race.lock.unlock() }
                if let addr {
                    if race.found == nil { race.found = addr; race.done.signal() }
                } else {
                    race.failed += 1
                    race.error = rc
                    if race.failed == 2 { race.done.signal() }
                }
            }
        }
        race.done.wait()
        race.lock.lock()
        defer { race.lock.unlock() }
        guard let found = race.found else {
            throw NSError(domain: "gmstream", code: Int(race.error), userInfo: [NSLocalizedDescriptionKey: "cannot resolve \(host): \(String(cString: gai_strerror(race.error)))"])
        }
        return found
    }

    func start(width: Int, height: Int, refreshMhz: UInt32, kbps: UInt32, codecMask: UInt8, preferred: UInt8) {
        var w = Writer()
        w.u16(protocolVersion)
        w.u16(UInt16(width)); w.u16(UInt16(height))
        w.u32(refreshMhz); w.u32(kbps)
        w.u8(codecMask); w.u8(preferred)
        // Plain UDP always sends video as datagrams; bit 2 asks for each frame
        // in pieces as the host encodes it, bit 3 says this client can show
        // HDR10. The second is a capability, not a request: whether the
        // session is HDR stays SteamOS's setting.
        w.u8((options.streamFrames ? 4 : 0) | (canShowHDR() ? 8 : 0))
        w.u16(UInt16(linkMaxDatagram))
        started = true
        send(Msg.hello, w.data)
        sendHandshake()
        let t = DispatchSource.makeTimerSource(queue: .global(qos: .userInteractive))
        t.schedule(deadline: .now() + .milliseconds(15), repeating: .milliseconds(15), leeway: .milliseconds(2))
        t.setEventHandler { [weak self] in self?.resend() }
        t.resume()
        sendLock.lock()
        resendTimer = t
        sendLock.unlock()
        let thread = Thread { [weak self] in self?.receiveLoop() }
        thread.qualityOfService = .userInteractive
        thread.name = "gmstream-net"
        thread.start()
        ping()
    }

    func stop() {
        keepAwake(false)
        sendLock.lock()
        resendTimer?.cancel()
        resendTimer = nil
        running = false
        sendLock.unlock()
        shutdown(fd, SHUT_RDWR)
    }

    // MARK: Sending

    func send(_ type: UInt8, _ payload: Data = Data()) {
        var m = Writer()
        m.u8(type)
        m.u32(UInt32(payload.count))
        m.data.append(payload)
        sendLock.lock()
        defer { sendLock.unlock() }
        guard running else { return } // the socket is closed (or about to be)
        guard let key = sendKey else { return unsent.append(m.data) }
        transmit(m.data, key)
    }

    /// Control stream bytes, sealed. Under sendLock.
    private func transmit(_ bytes: Data, _ key: SymmetricKey) {
        var off = 0
        while off < bytes.count {
            let n = min(maxDatagram - 5 - tagSize, bytes.count - off)
            var d = Writer()
            d.u8(datagramControl)
            d.u32(sendSeq)
            guard let box = try? AES.GCM.seal(bytes[(bytes.startIndex + off)..<(bytes.startIndex + off + n)], using: key,
                                              nonce: Self.nonce(.control, sendSeq), authenticating: d.data) else { return }
            d.data.append(box.ciphertext)
            d.data.append(box.tag)
            unacked.append((sendSeq, d.data, nowNs()))
            sendSeq += 1
            let sent = d.data.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, 0) }
            if sent < 0 && running {
                // e.g. "No route to host" when macOS denies local network access.
                let reason = "Sending to the host failed: \(String(cString: strerror(errno)))"
                DispatchQueue.main.async { self.delegate?.closed(reason: reason) }
            }
            off += n
        }
    }

    /// The mouse, unordered: how far it has gone since the session began, so a
    /// lost datagram is in the next one. In stream pixels.
    func motion(dx: Double, dy: Double) {
        sendLock.lock()
        defer { sendLock.unlock() }
        mouseTotal.x += dx
        mouseTotal.y += dy
        var m = Writer()
        m.u8(0)
        m.data.append(contentsOf: withUnsafeBytes(of: mouseTotal.x.bitPattern.littleEndian) { Array($0) })
        m.data.append(contentsOf: withUnsafeBytes(of: mouseTotal.y.bitPattern.littleEndian) { Array($0) })
        sendMouse(m.data)
    }

    /// The mouse at (x, y) in the video, 0...1: the latest one counts.
    func position(x: Float, y: Float) {
        sendLock.lock()
        defer { sendLock.unlock() }
        var m = Writer()
        m.u8(1)
        m.f32(x); m.f32(y)
        sendMouse(m.data)
    }

    /// Under sendLock.
    private func sendMouse(_ payload: Data) {
        guard running, let key = sendKey else { return }
        var d = Writer()
        d.u8(datagramMouse)
        d.u32(mouseSeq)
        guard let box = try? AES.GCM.seal(payload, using: key, nonce: Self.nonce(.mouse, mouseSeq), authenticating: d.data) else { return }
        mouseSeq += 1
        d.data.append(box.ciphertext)
        d.data.append(box.tag)
        _ = d.data.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, 0) }
        lastMouse = payload
        mouseRepeated = false
    }

    /// Every 15 ms: control datagrams not acknowledged in time go again, and the
    /// mouse's last word once more, as nothing would follow it if it were lost.
    private func resend() {
        sendLock.lock()
        defer { sendLock.unlock() }
        guard running else { return }
        let now = nowNs()
        for i in unacked.indices where now - unacked[i].sentNs > Self.resendAfterNs {
            unacked[i].sentNs = now
            _ = unacked[i].datagram.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, 0) }
        }
        if !mouseRepeated, let lastMouse {
            sendMouse(lastMouse)
            mouseRepeated = true
        }
    }

    private enum SeqKind: UInt8 { case control = 0, mouse = 1, ack = 2 }

    private static func nonce(_ kind: SeqKind, _ seq: UInt32) -> AES.GCM.Nonce {
        var n = Data(count: 8)
        n[0] = kind.rawValue
        withUnsafeBytes(of: seq.bigEndian) { n.append(contentsOf: $0) }
        return try! AES.GCM.Nonce(data: n)
    }

    /// Our half of the key exchange (src/auth.rs), again until it is answered.
    private func sendHandshake() {
        sendLock.lock()
        defer { sendLock.unlock() }
        guard running, sendKey == nil else { return }
        var d = Data([datagramHandshake])
        d.append(ephemeral.publicKey.rawRepresentation)
        d.append(Data(count: handshakeSize - d.count))
        _ = d.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, 0) }
    }

    /// The host's half: its keys, then Auth and whatever was waiting.
    private func handshakeReply(_ d: Data) {
        guard recvKey == nil else { return }
        let s = d.startIndex
        let theirs = d.subdata(in: s + 1..<s + 33), hostKey = d.subdata(in: s + 33..<s + 65), signature = d.subdata(in: s + 65..<s + 129)
        var hash = SHA256()
        hash.update(data: Data("gmstream-udp-1".utf8))
        hash.update(data: ephemeral.publicKey.rawRepresentation)
        hash.update(data: theirs)
        hash.update(data: hostKey)
        let transcript = Data(hash.finalize())
        guard let host = try? Curve25519.Signing.PublicKey(rawRepresentation: hostKey),
              host.isValidSignature(signature, for: Data("gmstream host".utf8) + transcript),
              let peer = try? Curve25519.KeyAgreement.PublicKey(rawRepresentation: theirs),
              let shared = try? ephemeral.sharedSecretFromKeyAgreement(with: peer) else { return }
        guard Identity.hostKeyMatches(host: hostName, key: hostKey) else {
            let reason = "\(hostName)'s key has changed. If that's expected, forget the host in Settings."
            return DispatchQueue.main.async { self.delegate?.closed(reason: reason) }
        }
        func derive(_ info: String) -> SymmetricKey {
            shared.hkdfDerivedSymmetricKey(using: SHA256.self, salt: transcript, sharedInfo: Data(info.utf8), outputByteCount: 32)
        }
        recvKey = derive("h2c")
        let key = derive("c2h")
        var auth = Writer()
        auth.u8(Msg.auth)
        auth.u32(96)
        auth.data.append(Identity.shared.key.publicKey.rawRepresentation)
        guard let mine = try? Identity.shared.key.signature(for: Data("gmstream client".utf8) + transcript) else { return }
        auth.data.append(mine)
        sendLock.lock()
        defer { sendLock.unlock() }
        guard running else { return }
        sendKey = key
        transmit(auth.data, key)
        for bytes in unsent { transmit(bytes, key) }
        unsent = []
    }

    /// Keeps this Mac's Wi-Fi out of power save: a 1-byte datagram 60 times
    /// a second, which the host drops. Measured: a Mac that only receives
    /// (pings every 500 ms) dozes about 10 times a minute and wakes at the
    /// access point's next beacon (up to 102.4 ms later), so the stream
    /// stalls 30-110 ms and the router drops what it can't hold. 200 Hz and
    /// 60 Hz stopped every stall; 10 Hz did not help.
    func keepAwake(_ on: Bool) {
        sendLock.lock()
        defer { sendLock.unlock() }
        awakeTimer?.cancel()
        awakeTimer = nil
        guard on, running else { return }
        let t = DispatchSource.makeTimerSource(queue: .global(qos: .userInteractive))
        t.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / 60), leeway: .milliseconds(1))
        t.setEventHandler { [weak self] in self?.sendAwake() }
        t.resume()
        awakeTimer = t
    }

    private func sendAwake() {
        sendLock.lock()
        defer { sendLock.unlock() }
        guard running else { return }
        var b = datagramAwake
        _ = Darwin.send(fd, &b, 1, 0)
    }

    /// Also the keepalive: the host ends a session after 10 s of silence.
    func ping() {
        sendHandshake() // lost, if nothing has come back
        var w = Writer()
        w.u64(nowNs() / 1000)
        send(Msg.ping, w.data)
        // lastHeard is written on the network thread. Read it once, and only
        // subtract when it really is in the past: the receive thread can set
        // it between the two reads, and an unsigned underflow traps (it did,
        // twice, as EXC_BREAKPOINT in here).
        let last = lastHeard, now = nowNs()
        if now > last, now - last > 5_000_000_000 {
            DispatchQueue.main.async { self.delegate?.closed(reason: "No packets from the host for 5 s") }
        }
    }

    /// Host clock minus ours, if known.
    var clockOffsetNs: Int64? {
        clockLock.lock(); defer { clockLock.unlock() }
        return offsetNs
    }

    /// Our clock in the host's time base, if known.
    func toHost(localNs: UInt64) -> UInt64? {
        clockLock.lock()
        defer { clockLock.unlock() }
        guard let off = offsetNs else { return nil }
        return UInt64(Int64(localNs) + off)
    }

    /// Host clock in our time base, if known.
    func toLocal(hostNs: UInt64) -> UInt64? {
        clockLock.lock()
        defer { clockLock.unlock() }
        guard let off = offsetNs else { return nil }
        return UInt64(Int64(hostNs) - off)
    }

    // MARK: Receiving

    private func receiveLoop() {
        // The socket lives until stop() ends this loop; closed under the send
        // lock so no send uses its number after (it may be reused).
        defer {
            sendLock.lock()
            close(fd)
            sendLock.unlock()
        }
        var buf = [UInt8](repeating: 0, count: 65536)
        var control = [UInt8](repeating: 0, count: 64)
        var tb = mach_timebase_info_data_t()
        mach_timebase_info(&tb)
        while running {
            var arrived: UInt64 = 0
            let n = buf.withUnsafeMutableBytes { b in
                control.withUnsafeMutableBytes { c in
                    var iov = iovec(iov_base: b.baseAddress, iov_len: b.count)
                    return withUnsafeMutablePointer(to: &iov) { iovp in
                        var msg = msghdr(msg_name: nil, msg_namelen: 0, msg_iov: iovp, msg_iovlen: 1,
                                         msg_control: c.baseAddress, msg_controllen: socklen_t(c.count), msg_flags: 0)
                        let n = recvmsg(fd, &msg, 0)
                        // One control message: SCM_TIMESTAMP_MONOTONIC, a
                        // UInt64 after the 12-byte header.
                        if n > 0 && msg.msg_controllen >= 20 {
                            let h = c.load(as: cmsghdr.self)
                            if h.cmsg_level == SOL_SOCKET && h.cmsg_type == SCM_TIMESTAMP_MONOTONIC {
                                arrived = c.loadUnaligned(fromByteOffset: 12, as: UInt64.self) * UInt64(tb.numer) / UInt64(tb.denom)
                            }
                        }
                        return n
                    }
                }
            }
            if n <= 0 {
                if errno == EINTR { continue }
                if running {
                    let reason = n < 0 ? String(cString: strerror(errno)) : "closed"
                    DispatchQueue.main.async { self.delegate?.closed(reason: "Network error: \(reason)") }
                }
                return
            }
            lastHeard = nowNs()
            // A thread of our own has no autorelease pool unless we drain one:
            // Metal objects autoreleased by the decoders (PyroWave's compute
            // encoders) would pile up for the whole session.
            autoreleasepool { datagram(Data(buf[0..<n]), n: n, arrived: arrived) }
        }
    }

    private func datagram(_ d: Data, n: Int, arrived: UInt64) {
        if let trace = Trace.shared {
            let t = arrived > 0 ? arrived : lastHeard
            let kind = d[0]
            let id = n >= 5 ? d.u32(1) : 0
            let frag = (kind == datagramVideo || kind == datagramPyroWave ||
                        kind == datagramVideoChunk) && n >= 9
            let host: UInt64 = kind == datagramAudio && n >= 13 ? d.u64(5) : kind == datagramPyroWave && n > 29 ? d.u64(21) : 0
            trace.record(t: t, aux: lastHeard &- t, kind: kind, size: n, id: id,
                         index: frag ? Int(d.u16(5)) : 0, count: frag ? Int(d.u16(7)) : 0, host: host)
        }
        switch d[0] {
        case datagramVideo where n > 9:
            videoFragment(id: d.u32(1), index: Int(d.u16(5)), count: Int(d.u16(7)),
                          part: d.subdata(in: 9..<n), arrived: arrived > 0 ? arrived : lastHeard)
        case datagramVideoChunk where n > videoChunkHeader:
            videoChunk(d, n: n, arrived: arrived > 0 ? arrived : lastHeard)
        case datagramHandshake where n >= handshakeSize:
            handshakeReply(d)
        case datagramControl where n > 5 + tagSize:
            let seq = d.u32(1), s = d.startIndex
            guard let key = recvKey,
                  let box = try? AES.GCM.SealedBox(nonce: Self.nonce(.control, seq), ciphertext: d.subdata(in: s + 5..<s + n - tagSize),
                                                   tag: d.subdata(in: s + n - tagSize..<s + n)),
                  let payload = try? AES.GCM.open(box, using: key, authenticating: d.subdata(in: s..<s + 5)) else { return }
            controlDatagram(seq: seq, payload: payload)
        case datagramAck where n == 5 + 4 + tagSize:
            let seq = d.u32(1), s = d.startIndex
            guard let key = recvKey,
                  let box = try? AES.GCM.SealedBox(nonce: Self.nonce(.ack, seq), ciphertext: d.subdata(in: s + 5..<s + 9),
                                                   tag: d.subdata(in: s + 9..<s + n)),
                  let next = try? AES.GCM.open(box, using: key, authenticating: d.subdata(in: s..<s + 5)) else { return }
            let upTo = Data(next).u32(0)
            sendLock.lock()
            unacked.removeAll { $0.seq < upTo }
            sendLock.unlock()
        case datagramPyroWave where n > 29:
            delegate?.pyrowave(PyroWavePacket(
                frameId: d.u32(1), index: Int(d.u16(5)), count: Int(d.u16(7)),
                width: Int(d.u16(9)), height: Int(d.u16(11)), slotNs: d.u64(13), encodedNs: d.u64(21),
                payload: d.subdata(in: 29..<n), receivedNs: arrived > 0 ? arrived : lastHeard, readNs: lastHeard))
        case datagramAudio where n > 13:
            delegate?.audio(d.subdata(in: 13..<n))
        default:
            break
        }
    }

    /// One piece of a frame the host is still encoding (PROTOCOL.md, type 9).
    private func videoChunk(_ d: Data, n: Int, arrived: UInt64) {
        let id = d.u32(1)
        let f = chunked[id] ?? {
            let f = ChunkedFrame()
            f.firstNs = arrived
            chunked[id] = f
            // Pieces of the newest few frames only; an incomplete one is lost.
            if chunked.count > 4, let oldest = chunked.keys.min() { chunked[oldest] = nil }
            return f
        }()
        let flags = d[d.startIndex + 17]
        f.codec = d[d.startIndex + 18]
        f.width = Int(d.u16(19))
        f.height = Int(d.u16(21))
        f.slotNs = d.u64(23)
        if flags & 2 != 0 { // the last piece: now we know how many, and the rest
            f.key = flags & 1 != 0
            f.count = Int(d.u16(7))
            f.total = Int(d.u32(13))
            f.encodedNs = d.u64(31)
            // How much of the sending happened while the frame was still
            // being encoded.
            f.overlapNs = UInt64(d.u32(39))
        }
        f.put(index: Int(d.u16(5)), offset: Int(d.u32(9)),
              data: d.subdata(in: videoChunkHeader..<n))
        guard f.whole, f.total > 0 else { return }
        chunked[id] = nil
        let frame = Frame(id: id, key: f.key, codec: f.codec, width: f.width,
                          height: f.height, slotNs: f.slotNs, encodedNs: f.encodedNs,
                          // The last piece's kernel arrival, not the time we
                          // got around to reading it: that lag is this
                          // client's, and the "read" stage shows it.
                          bitstream: f.bitstream(), receivedNs: arrived, firstNs: f.firstNs,
                          overlapNs: f.overlapNs)
        delegate?.received(frame: frame)
    }

    private func videoFragment(id: UInt32, index: Int, count: Int, part: Data,
                               arrived: UInt64) {
        var f = fragments[id] ?? (parts: [Data?](repeating: nil, count: count), got: 0, first: arrived)
        guard index < f.parts.count, f.parts[index] == nil else { return }
        f.parts[index] = part
        f.got += 1
        if f.got < f.parts.count {
            fragments[id] = f
            if fragments.count > 16, let oldest = fragments.keys.min() { fragments[oldest] = nil }
            return
        }
        fragments[id] = nil
        var data = Data(capacity: f.parts.reduce(0) { $0 + $1!.count })
        for p in f.parts { data.append(p!) }
        guard data.count >= 32 else { return }
        let frame = Frame(id: data.u32(0), key: data[4] & 1 != 0, codec: data[5],
                          width: Int(data.u16(6)), height: Int(data.u16(8)),
                          slotNs: data.u64(12), encodedNs: data.u64(20),
                          bitstream: data.subdata(in: 32..<data.count), receivedNs: arrived,
                          firstNs: f.first)
        delegate?.received(frame: frame)
    }

    private func controlDatagram(seq: UInt32, payload: Data) {
        if seq == controlNext {
            controlBuffer.append(payload)
            controlNext += 1
            while let p = controlPending.removeValue(forKey: controlNext) {
                controlBuffer.append(p)
                controlNext += 1
            }
            parseControl()
        } else if seq > controlNext {
            controlPending[seq] = payload
        }
    }

    private func parseControl() {
        while controlBuffer.count >= 5 {
            let len = Int(controlBuffer.u32(1))
            guard controlBuffer.count >= 5 + len else { return }
            let type = controlBuffer[controlBuffer.startIndex]
            let body = controlBuffer.subdata(in: controlBuffer.startIndex + 5..<controlBuffer.startIndex + 5 + len)
            controlBuffer.removeSubrange(controlBuffer.startIndex..<controlBuffer.startIndex + 5 + len)
            handleControl(type, body)
        }
    }

    private func handleControl(_ type: UInt8, _ body: Data) {
        switch type {
        case Msg.pong where body.count >= 16:
            let now = nowNs()
            let t0 = body.u64(0) * 1000
            let rtt = now - t0
            let host = body.u64(8)
            clockLock.lock()
            if offsetNs == nil || rtt <= bestRtt + bestRtt / 5 {
                bestRtt = min(bestRtt, rtt)
                offsetNs = Int64(host) - Int64(t0 + rtt / 2)
            }
            rttNs = rtt
            clockLock.unlock()
            // This ping's own offset (host − our midpoint), not the fitted one.
            Trace.shared?.record(t: now, aux: rtt, kind: 0xFF, host: UInt64(bitPattern: Int64(host) - Int64(t0 + rtt / 2)))
        case Msg.helloAck where body.count >= 1:
            let codec = body[body.startIndex]
            DispatchQueue.main.async { self.delegate?.helloAck(codec: codec) }
        case Msg.cursor where body.count >= 8:
            let w = Int(body.u16(0)), h = Int(body.u16(2)), hx = Int(body.i16(4)), hy = Int(body.i16(6))
            let rgba = body.subdata(in: body.startIndex + 8..<body.endIndex)
            DispatchQueue.main.async { self.delegate?.cursor(width: w, height: h, hotX: hx, hotY: hy, rgba: rgba) }
        case Msg.relative where body.count >= 1:
            let on = body[body.startIndex] != 0
            DispatchQueue.main.async { self.delegate?.relative(on) }
        case Msg.cursorClip where body.count >= 8:
            let (l, t, r, b) = (Int(body.u16(0)), Int(body.u16(2)), Int(body.u16(4)), Int(body.u16(6)))
            let clip = r > l && b > t ? CGRect(x: l, y: t, width: r - l, height: b - t) : nil
            DispatchQueue.main.async { self.delegate?.cursorClip(clip) }
        case Msg.rumble where body.count >= 7:
            let (i, s, w, ms) = (Int(body[body.startIndex]), body.u16(1), body.u16(3), Int(body.u16(5)))
            DispatchQueue.main.async { self.delegate?.rumble(index: i, strong: s, weak: w, ms: ms) }
        case Msg.encodeFormat:
            let text = String(decoding: body, as: UTF8.self)
            DispatchQueue.main.async { self.delegate?.encodeFormat(text) }
        case Msg.error:
            let text = String(decoding: body, as: UTF8.self)
            DispatchQueue.main.async { self.delegate?.closed(reason: text) }
        default:
            break
        }
    }
}

/// One PyroWave packet and the frame it belongs to (PROTOCOL.md, PyroWave).
struct PyroWavePacket {
    let frameId: UInt32
    let index: Int, count: Int
    let width: Int, height: Int
    let slotNs: UInt64, encodedNs: UInt64
    let payload: Data
    let receivedNs: UInt64 // arrived (kernel time stamp)
    let readNs: UInt64     // read by the network thread
}
