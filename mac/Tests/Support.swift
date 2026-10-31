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

    /// The next control datagram (type 3), skipping keep-awake ones.
    func receiveControl(timeout: Double = 2) -> (seq: UInt32, payload: Data)? {
        while let d = receive(timeout: timeout) {
            if d[0] == datagramControl { return (d.u32(1), d.subdata(in: 5..<d.count)) }
        }
        return nil
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
        w.data.append(bytes)
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
    func pointerLock(_ locked: Bool) { record("pointerLock \(locked)") }
    func rumble(index: Int, strong: UInt16, weak: UInt16, ms: Int) { record("rumble \(index) \(strong) \(weak) \(ms)") }
    func closed(reason: String) { record("closed \(reason)") }

    var snapshot: (events: [String], frames: [Frame], packets: [PyroWavePacket], audio: [Data]) {
        lock.lock(); defer { lock.unlock() }
        return (events, frames, packets, audioPackets)
    }
}
