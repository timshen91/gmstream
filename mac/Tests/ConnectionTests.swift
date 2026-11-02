import XCTest

/// The plain UDP transport (PROTOCOL.md, "Plain UDP") against a fake host on
/// the loopback interface.
final class ConnectionTests: XCTestCase {
    var host: FakeHost!
    var conn: Connection!
    var recorder: Recorder!

    override func setUpWithError() throws {
        host = FakeHost()
        conn = try Connection(host: "127.0.0.1", port: host.port)
        recorder = Recorder()
        conn.delegate = recorder
    }

    override func tearDown() {
        conn.stop()
        conn = nil
        host = nil
    }

    /// Starts the session and reads its Hello and first Ping.
    @discardableResult
    func start() -> Data {
        conn.start(width: 1280, height: 720, refreshMhz: 60_000, kbps: 40_000, codecMask: 0b1011, preferred: 3)
        let hello = host.receiveControl()!
        XCTAssertEqual(hello.seq, 0)
        _ = host.receiveControl() // ping
        return hello.payload
    }

    /// Waits until the recorder has `n` events (main-thread ones arrive
    /// through the main queue, which waiting runs).
    func waitForEvents(_ n: Int, timeout: Double = 2) {
        let deadline = Date().addingTimeInterval(timeout)
        while recorder.snapshot.events.count < n && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
    }

    func waitUntil(timeout: Double = 2, _ done: () -> Bool) {
        let deadline = Date().addingTimeInterval(timeout)
        while !done() && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
    }

    func testHelloCarriesTheSessionParameters() {
        let m = start()
        XCTAssertEqual(m[0], Msg.hello)
        XCTAssertEqual(Int(m.u32(1)), 19)
        let p = m.subdata(in: 5..<m.count)
        XCTAssertEqual(p.u16(0), protocolVersion)
        XCTAssertEqual(p.u16(2), 1280)
        XCTAssertEqual(p.u16(4), 720)
        XCTAssertEqual(p.u32(6), 60_000)
        XCTAssertEqual(p.u32(10), 40_000)
        XCTAssertEqual(p[14], 0b1011)
        XCTAssertEqual(p[15], 3)
        XCTAssertEqual(p[16], 0, "plain UDP sends no video flags")
        XCTAssertGreaterThanOrEqual(Int(p.u16(17)), maxDatagram, "max_datagram: at least the control size, from the link's MTU")
    }

    func testStartPingsWithOurClock() {
        conn.start(width: 64, height: 64, refreshMhz: 60_000, kbps: 1000, codecMask: 1, preferred: 0)
        _ = host.receiveControl()
        let before = nowNs() / 1000
        let ping = host.receiveControl()!
        XCTAssertEqual(ping.seq, 1)
        XCTAssertEqual(ping.payload[0], Msg.ping)
        XCTAssertEqual(ping.payload.u32(1), 8)
        let t = ping.payload.u64(5)
        XCTAssertLessThanOrEqual(t, before + 1)
        XCTAssertGreaterThan(t, before - 1_000_000, "microseconds of this Mac's clock")
    }

    func testLongMessagesAreSplitIntoNumberedDatagrams() {
        let payload = Data((0..<3000).map { UInt8($0 & 0xff) })
        conn.send(Msg.gamepad, payload)
        var seqs: [UInt32] = []
        var bytes = Data()
        while bytes.count < 5 + payload.count, let c = host.receiveControl() {
            XCTAssertLessThanOrEqual(c.payload.count + 5, maxDatagram)
            seqs.append(c.seq)
            bytes.append(c.payload)
        }
        XCTAssertEqual(seqs, [0, 1, 2])
        XCTAssertEqual(bytes, FakeHost.message(Msg.gamepad, payload))
    }

    func testControlStreamIsReassembledInOrder() {
        start()
        var cursor = Writer()
        cursor.u16(2); cursor.u16(1); cursor.i16(-1); cursor.i16(5)
        cursor.data.append(contentsOf: [1, 2, 3, 4, 5, 6, 7, 8])
        var rumble = Writer()
        rumble.u8(1); rumble.u16(1000); rumble.u16(2000); rumble.u16(300)
        let stream = FakeHost.message(Msg.helloAck, Data([3]))
            + FakeHost.message(Msg.pong + 0x40) // an unknown type: skipped
            + FakeHost.message(Msg.cursor, cursor.data)
            + FakeHost.message(Msg.relative, Data([1]))
            + FakeHost.message(Msg.cursorClip, Data([100, 0, 50, 0, 44, 1, 244, 1])) // 100,50 - 300,500
            + FakeHost.message(Msg.cursorClip, Data(repeating: 0, count: 8))
            + FakeHost.message(Msg.rumble, rumble.data)
            + FakeHost.message(Msg.error, Data("bye".utf8))
        // Seven-byte pieces, each odd/even pair swapped, and a stale repeat.
        let pieces = stride(from: 0, to: stream.count, by: 7).map { stream.subdata(in: $0..<min($0 + 7, stream.count)) }
        var order = Array(pieces.indices)
        for i in stride(from: 0, to: order.count - 1, by: 2) { order.swapAt(i, i + 1) }
        for (n, i) in order.enumerated() {
            host.sendControl(pieces[i], seq: UInt32(i))
            if n == 4 { host.sendControl(pieces[0], seq: 0) }
        }
        waitForEvents(7)
        XCTAssertEqual(recorder.snapshot.events, [
            "helloAck 3",
            "cursor 2x1 hot -1,5 [1, 2, 3, 4, 5, 6, 7, 8]",
            "relative true",
            "cursorClip (100.0, 50.0, 200.0, 450.0)",
            "cursorClip none",
            "rumble 1 1000 2000 300",
            "closed bye",
        ])
    }

    /// A video frame (PROTOCOL.md, Video): the 32-byte header, then the bitstream.
    func frameBytes(id: UInt32, key: Bool, size: Int) -> Data {
        var w = Writer()
        w.u32(id)
        w.u8(key ? 1 : 0)
        w.u8(1)
        w.u16(640); w.u16(360)
        w.u16(0) // reserved
        w.u64(111_000); w.u64(222_000)
        w.u32(0) // reserved
        XCTAssertEqual(w.data.count, 32)
        return w.data + Data((0..<size).map { UInt8(($0 * 7) & 0xff) })
    }

    func videoFragment(id: UInt32, index: Int, count: Int, _ part: Data) -> Data {
        var w = Writer()
        w.u8(datagramVideo)
        w.u32(id)
        w.u16(UInt16(index)); w.u16(UInt16(count))
        return w.data + part
    }

    func testVideoFragmentsMakeAFrameInAnyOrder() {
        start()
        let bytes = frameBytes(id: 7, key: true, size: 2968)
        let parts = stride(from: 0, to: bytes.count, by: 1000).map { bytes.subdata(in: $0..<min($0 + 1000, bytes.count)) }
        XCTAssertEqual(parts.count, 3)
        for i in [2, 0, 0, 1] { host.send(videoFragment(id: 7, index: i, count: 3, parts[i])) }
        waitUntil { !recorder.snapshot.frames.isEmpty }
        let frames = recorder.snapshot.frames
        XCTAssertEqual(frames.count, 1)
        let f = frames[0]
        XCTAssertEqual(f.id, 7)
        XCTAssertTrue(f.key)
        XCTAssertEqual(f.codec, 1)
        XCTAssertEqual(f.width, 640)
        XCTAssertEqual(f.height, 360)
        XCTAssertEqual(f.slotNs, 111_000)
        XCTAssertEqual(f.encodedNs, 222_000)
        XCTAssertEqual(f.bitstream, bytes.subdata(in: 32..<bytes.count))
    }

    func testAFrameMissingAFragmentIsNeverDelivered() {
        start()
        let bytes = frameBytes(id: 8, key: false, size: 2000)
        host.send(videoFragment(id: 8, index: 0, count: 3, bytes.subdata(in: 0..<1000)))
        host.send(videoFragment(id: 8, index: 2, count: 3, bytes.subdata(in: 2000..<bytes.count)))
        let next = frameBytes(id: 9, key: false, size: 100)
        host.send(videoFragment(id: 9, index: 0, count: 1, next))
        waitUntil { !recorder.snapshot.frames.isEmpty }
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        XCTAssertEqual(recorder.snapshot.frames.map(\.id), [9])
    }

    func testAudioAndPyroWaveDatagrams() {
        start()
        var a = Writer()
        a.u8(datagramAudio); a.u32(5); a.u64(123_456)
        host.send(a.data + Data([0xF8, 0xFF, 0xFE]))
        var p = Writer()
        p.u8(datagramPyroWave); p.u32(42); p.u16(3); p.u16(10); p.u16(3840); p.u16(2160); p.u64(1_000); p.u64(2_000)
        host.send(p.data + Data([1, 2, 3, 4]))
        waitUntil { !recorder.snapshot.audio.isEmpty && !recorder.snapshot.packets.isEmpty }
        XCTAssertEqual(recorder.snapshot.audio, [Data([0xF8, 0xFF, 0xFE])])
        let pkt = recorder.snapshot.packets.first
        XCTAssertEqual(pkt?.frameId, 42)
        XCTAssertEqual(pkt?.index, 3)
        XCTAssertEqual(pkt?.count, 10)
        XCTAssertEqual(pkt?.width, 3840)
        XCTAssertEqual(pkt?.height, 2160)
        XCTAssertEqual(pkt?.slotNs, 1_000)
        XCTAssertEqual(pkt?.encodedNs, 2_000)
        XCTAssertEqual(pkt?.payload, Data([1, 2, 3, 4]))
        XCTAssertGreaterThan(pkt?.receivedNs ?? 0, 0)
    }

    func testPongGivesTheHostClock() {
        conn.start(width: 64, height: 64, refreshMhz: 60_000, kbps: 1000, codecMask: 1, preferred: 0)
        _ = host.receiveControl()
        let ping = host.receiveControl()!
        XCTAssertNil(conn.toHost(localNs: 1))
        let t0 = ping.payload.u64(5)
        let offset: UInt64 = 5_000_000_000
        var pong = Writer()
        pong.u64(t0)
        pong.u64(t0 * 1000 + offset) // the host's clock at our ping
        host.sendControl(FakeHost.message(Msg.pong, pong.data), seq: 0)
        waitUntil { conn.clockOffsetNs != nil }
        let off = try? XCTUnwrap(conn.clockOffsetNs)
        // Host time minus the round trip's midpoint: within the loopback round trip.
        XCTAssertEqual(Double(off ?? 0), Double(offset), accuracy: 2e6)
        let x: UInt64 = 1_000_000_000
        XCTAssertEqual(conn.toLocal(hostNs: conn.toHost(localNs: x)!), x)
        XCTAssertGreaterThan(conn.rttNs, 0)
    }

    func testKeepAwakeSendsTinyDatagramsAt60Hz() {
        conn.keepAwake(true)
        var awake = 0
        let end = Date().addingTimeInterval(0.5)
        while Date() < end, let d = host.receive(timeout: 0.2) {
            XCTAssertEqual(d, Data([datagramAwake]))
            awake += 1
        }
        XCTAssertEqual(Double(awake), 30, accuracy: 8)
        conn.keepAwake(false)
        while host.receive(timeout: 0.05) != nil {} // in flight
        XCTAssertNil(host.receive(timeout: 0.2))
    }

    func testStopIsQuiet() {
        start()
        conn.stop()
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        XCTAssertEqual(recorder.snapshot.events, [], "stopping is not a network error")
        conn.send(Msg.keyframe) // after stop: dropped, not sent on a closed socket
        XCTAssertNil(host.receive(timeout: 0.1))
    }
}
