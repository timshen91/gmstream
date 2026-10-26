// The gmstream protocol over plain UDP (PROTOCOL.md, "Plain UDP"): control
// messages as a byte stream in numbered datagrams, video as fragmented
// frames, audio datagrams (ignored for now). No encryption, no retransmission.

import Foundation

enum Msg {
    // Client → host.
    static let hello: UInt8 = 0x01, configure: UInt8 = 0x02, keyframe: UInt8 = 0x03, bitrate: UInt8 = 0x04,
        key: UInt8 = 0x05, button: UInt8 = 0x06, motion: UInt8 = 0x07, position: UInt8 = 0x08,
        scroll: UInt8 = 0x09, gamepad: UInt8 = 0x0a, gamepadRemove: UInt8 = 0x0b, ping: UInt8 = 0x0c, displayGrid: UInt8 = 0x0d
    // Host → client.
    static let helloAck: UInt8 = 0x81, cursor: UInt8 = 0x82, pointerLock: UInt8 = 0x83,
        rumble: UInt8 = 0x84, pong: UInt8 = 0x85, error: UInt8 = 0x86
}

let protocolVersion: UInt16 = 2
let datagramAudio: UInt8 = 1, datagramVideo: UInt8 = 2, datagramControl: UInt8 = 3, datagramPyroWave: UInt8 = 4,
    datagramAwake: UInt8 = 5
let maxDatagram = 1400

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

/// One encoded frame as the host sent it (PROTOCOL.md, Video).
struct Frame {
    let id: UInt32
    let key: Bool
    let codec: UInt8
    let width: Int, height: Int
    let slotNs: UInt64, encodedNs: UInt64
    let bitstream: Data
    let receivedNs: UInt64
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
    func pointerLock(_ locked: Bool)
    func rumble(index: Int, strong: UInt16, weak: UInt16, ms: Int)
    func closed(reason: String)
}

final class Connection {
    weak var delegate: ConnectionDelegate?
    private let fd: Int32
    private let sendLock = NSLock()
    private var sendSeq: UInt32 = 0
    private var running = true
    private var awakeTimer: DispatchSourceTimer? // under sendLock

    // Host → client control stream reassembly (network thread only).
    private var controlNext: UInt32 = 0
    private var controlPending: [UInt32: Data] = [:]
    private var controlBuffer = Data()

    // Video reassembly (network thread only).
    private var fragments: [UInt32: (parts: [Data?], got: Int)] = [:]

    // Clock: host CLOCK_MONOTONIC ns minus ours, from the fastest ping.
    private let clockLock = NSLock()
    private var bestRtt = UInt64.max
    private var offsetNs: Int64?
    private(set) var rttNs: UInt64 = 0
    private var lastHeard = nowNs()

    init(host: String, port: UInt16) throws {
        var hints = addrinfo(ai_flags: 0, ai_family: AF_UNSPEC, ai_socktype: SOCK_DGRAM, ai_protocol: IPPROTO_UDP,
                             ai_addrlen: 0, ai_canonname: nil, ai_addr: nil, ai_next: nil)
        var res: UnsafeMutablePointer<addrinfo>?
        let rc = getaddrinfo(host, String(port), &hints, &res)
        guard rc == 0, let ai = res else {
            throw NSError(domain: "gmstream", code: Int(rc), userInfo: [NSLocalizedDescriptionKey: "cannot resolve \(host): \(String(cString: gai_strerror(rc)))"])
        }
        defer { freeaddrinfo(res) }
        fd = socket(ai.pointee.ai_family, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw NSError(domain: "gmstream", code: Int(errno)) }
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
        guard connect(fd, ai.pointee.ai_addr, ai.pointee.ai_addrlen) == 0 else {
            let e = errno
            close(fd)
            throw NSError(domain: "gmstream", code: Int(e), userInfo: [NSLocalizedDescriptionKey: "connect: \(String(cString: strerror(e)))"])
        }
    }

    func start(width: Int, height: Int, refreshMhz: UInt32, kbps: UInt32, codecMask: UInt8, preferred: UInt8) {
        var w = Writer()
        w.u16(protocolVersion)
        w.u16(UInt16(width)); w.u16(UInt16(height))
        w.u32(refreshMhz); w.u32(kbps)
        w.u8(codecMask); w.u8(preferred)
        w.u8(0) // flags: plain UDP always sends video as datagrams
        send(Msg.hello, w.data)
        let thread = Thread { [weak self] in self?.receiveLoop() }
        thread.qualityOfService = .userInteractive
        thread.name = "gmstream-net"
        thread.start()
        ping()
    }

    func stop() {
        keepAwake(false)
        running = false
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
        var off = 0
        while off < m.data.count {
            let n = min(maxDatagram - 5, m.data.count - off)
            var d = Writer()
            d.u8(datagramControl)
            d.u32(sendSeq)
            sendSeq += 1
            d.data.append(m.data[off..<off + n])
            let sent = d.data.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, 0) }
            if sent < 0 && running {
                // e.g. "No route to host" when macOS denies local network access.
                let reason = "Sending to the host failed: \(String(cString: strerror(errno)))"
                DispatchQueue.main.async { self.delegate?.closed(reason: reason) }
            }
            off += n
        }
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
        var w = Writer()
        w.u64(nowNs() / 1000)
        send(Msg.ping, w.data)
        if nowNs() - lastHeard > 5_000_000_000 {
            DispatchQueue.main.async { self.delegate?.closed(reason: "No packets from the host for 5 s") }
        }
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
            let d = Data(buf[0..<n])
            if let trace = Trace.shared {
                let t = arrived > 0 ? arrived : lastHeard
                let kind = d[0]
                let id = n >= 5 ? d.u32(1) : 0
                let frag = (kind == datagramVideo || kind == datagramPyroWave) && n >= 9
                let host: UInt64 = kind == datagramAudio && n >= 13 ? d.u64(5) : kind == datagramPyroWave && n > 29 ? d.u64(21) : 0
                trace.record(t: t, aux: lastHeard &- t, kind: kind, size: n, id: id,
                             index: frag ? Int(d.u16(5)) : 0, count: frag ? Int(d.u16(7)) : 0, host: host)
            }
            switch d[0] {
            case datagramVideo where n > 9:
                videoFragment(id: d.u32(1), index: Int(d.u16(5)), count: Int(d.u16(7)), part: d.subdata(in: 9..<n))
            case datagramControl where n > 5:
                controlDatagram(seq: d.u32(1), payload: d.subdata(in: 5..<n))
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
    }

    private func videoFragment(id: UInt32, index: Int, count: Int, part: Data) {
        var f = fragments[id] ?? (parts: [Data?](repeating: nil, count: count), got: 0)
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
                          bitstream: data.subdata(in: 32..<data.count), receivedNs: nowNs())
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
            clockLock.lock()
            if offsetNs == nil || rtt <= bestRtt + bestRtt / 5 {
                bestRtt = min(bestRtt, rtt)
                offsetNs = Int64(body.u64(8)) - Int64(t0 + rtt / 2)
            }
            rttNs = rtt
            let off = offsetNs
            clockLock.unlock()
            if let off { Trace.shared?.record(t: now, aux: rtt, kind: 0xFF, host: UInt64(bitPattern: off)) }
        case Msg.helloAck where body.count >= 1:
            let codec = body[body.startIndex]
            DispatchQueue.main.async { self.delegate?.helloAck(codec: codec) }
        case Msg.cursor where body.count >= 8:
            let w = Int(body.u16(0)), h = Int(body.u16(2)), hx = Int(body.i16(4)), hy = Int(body.i16(6))
            let rgba = body.subdata(in: body.startIndex + 8..<body.endIndex)
            DispatchQueue.main.async { self.delegate?.cursor(width: w, height: h, hotX: hx, hotY: hy, rgba: rgba) }
        case Msg.pointerLock where body.count >= 1:
            let locked = body[body.startIndex] != 0
            DispatchQueue.main.async { self.delegate?.pointerLock(locked) }
        case Msg.rumble where body.count >= 7:
            let (i, s, w, ms) = (Int(body[body.startIndex]), body.u16(1), body.u16(3), Int(body.u16(5)))
            DispatchQueue.main.async { self.delegate?.rumble(index: i, strong: s, weak: w, ms: ms) }
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
