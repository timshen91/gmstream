import CryptoKit
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
        let auth = host.receiveControl()!
        XCTAssertEqual(auth.seq, 0)
        XCTAssertEqual(auth.payload[0], Msg.auth)
        let hello = host.receiveControl()!
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

    func testAuthSignsTheExchangeWithThisDevicesKey() {
        conn.start(width: 1280, height: 720, refreshMhz: 60_000, kbps: 40_000, codecMask: 0b1011, preferred: 3)
        let m = host.receiveControl()!.payload
        XCTAssertEqual(Int(m.u32(1)), 96)
        let public_ = m.subdata(in: 5..<37), signature = m.subdata(in: 37..<101)
        XCTAssertEqual(public_, Identity.shared.key.publicKey.rawRepresentation)
        let key = try! Curve25519.Signing.PublicKey(rawRepresentation: public_)
        XCTAssertTrue(key.isValidSignature(signature, for: Data("gmstream client".utf8) + host.transcript))
    }

    func testAChangedHostKeyIsRefused() {
        start()
        UserDefaults.standard.removeObject(forKey: "hostKeys")
        let other = FakeHost()
        defer { UserDefaults.standard.removeObject(forKey: "hostKeys") }
        XCTAssertTrue(Identity.hostKeyMatches(host: "127.0.0.1", key: host.signingKey.publicKey.rawRepresentation))
        other.signingKey = Curve25519.Signing.PrivateKey()
        let c = try! Connection(host: "127.0.0.1", port: other.port)
        let r = Recorder()
        c.delegate = r
        c.start(width: 1280, height: 720, refreshMhz: 60_000, kbps: 40_000, codecMask: 0b1011, preferred: 3)
        XCTAssertNil(other.receiveControl(timeout: 0.3), "no Auth, no Hello")
        let deadline = Date().addingTimeInterval(2)
        while r.snapshot.events.isEmpty && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
        XCTAssertTrue(r.snapshot.events.first?.contains("key has changed") == true, "\(r.snapshot.events)")
        c.stop()
    }

    func testAForgedControlDatagramIsDropped() {
        start()
        var forged = Writer()
        forged.u8(datagramControl)
        forged.u32(0)
        forged.data.append(FakeHost.message(Msg.error, Data("forged".utf8)))
        forged.data.append(Data(count: tagSize))
        host.send(forged.data)
        host.sendControl(FakeHost.message(Msg.relative, Data([1])), seq: 0)
        waitForEvents(1)
        XCTAssertEqual(recorder.snapshot.events, ["relative true"])
    }

    func testMotionSaysHowFarTheMouseHasGone() {
        start()
        conn.motion(dx: 3, dy: -1)
        conn.motion(dx: 2.5, dy: 0)
        let a = host.receiveMouse()!, b = host.receiveMouse()!
        XCTAssertEqual(a.seq, 0)
        XCTAssertEqual(b.seq, 1)
        XCTAssertEqual(b.payload[0], 0)
        XCTAssertEqual(Double(bitPattern: b.payload.u64(1)), 5.5, "the first is in it, should it be lost")
        XCTAssertEqual(Double(bitPattern: b.payload.u64(9)), -1)
        let c = host.receiveMouse()!
        XCTAssertEqual(c.seq, 2, "the last word goes twice: nothing follows it")
        XCTAssertEqual(c.payload, b.payload)
    }

    func testControlIsSentAgainUntilAcknowledged() {
        start()
        host.autoAck = false
        conn.send(Msg.key, Data([1, 2, 3]))
        let first = host.receiveControl()!
        let again = host.receiveControl(timeout: 0.5)!
        XCTAssertEqual(again.seq, first.seq, "sent again")
        host.sendAck(next: first.seq + 1)
        var late = 0
        let deadline = Date().addingTimeInterval(0.1)
        while Date() < deadline { if host.receiveControl(timeout: 0.05) != nil { late += 1 } }
        XCTAssertLessThanOrEqual(late, 1, "one may have crossed the acknowledgement")
        XCTAssertNil(host.receiveControl(timeout: 0.2), "acknowledged: no more")
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
        XCTAssertEqual(p[16], 0, "PyroWave preferred: no codec flags (frames in pieces, bit 2, are AV1 and HEVC's)")
        XCTAssertGreaterThanOrEqual(Int(p.u16(17)), maxDatagram, "max_datagram: at least the control size, from the link's MTU")
    }

    /// The Hello flags byte for a preferred codec.
    func helloFlags(preferred: UInt8) -> UInt8 {
        conn.start(width: 64, height: 64, refreshMhz: 60_000, kbps: 1000, codecMask: 0b1111, preferred: preferred)
        _ = host.receiveControl() // Auth
        return host.receiveControl()!.payload[5 + 16]
    }

    func testFramesInPiecesGoOnlyWithAV1AndHEVC() {
        XCTAssertEqual(helloFlags(preferred: 2) & 4, 4, "AV1: frames in pieces")
        XCTAssertEqual(helloFlags(preferred: 4) & 4, 0, "Riptide: not")
    }

    func testNoCodecFlagsWithH264() {
        XCTAssertEqual(helloFlags(preferred: 0) & 4, 0)
    }

    func testStartPingsWithOurClock() {
        conn.start(width: 64, height: 64, refreshMhz: 60_000, kbps: 1000, codecMask: 1, preferred: 0)
        _ = host.receiveControl() // Auth
        _ = host.receiveControl() // Hello
        let before = nowNs() / 1000
        let ping = host.receiveControl()!
        XCTAssertEqual(ping.seq, 2)
        XCTAssertEqual(ping.payload[0], Msg.ping)
        XCTAssertEqual(ping.payload.u32(1), 8)
        let t = ping.payload.u64(5)
        XCTAssertLessThanOrEqual(t, before + 1)
        XCTAssertGreaterThan(t, before - 1_000_000, "microseconds of this Mac's clock")
    }

    func testLongMessagesAreSplitIntoNumberedDatagrams() {
        start() // Auth, Hello, Ping: 0, 1, 2
        let payload = Data((0..<3000).map { UInt8($0 & 0xff) })
        conn.send(Msg.gamepad, payload)
        var seqs: [UInt32] = []
        var bytes = Data()
        while bytes.count < 5 + payload.count, let c = host.receiveControl() {
            XCTAssertLessThanOrEqual(c.payload.count + 5 + tagSize, maxDatagram)
            seqs.append(c.seq)
            bytes.append(c.payload)
        }
        XCTAssertEqual(seqs, [3, 4, 5])
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

    /// One piece of a frame the host is still encoding (PROTOCOL.md, type 9).
    func videoChunk(id: UInt32, index: Int, offset: Int, _ part: Data,
                    last: Bool = false, count: Int = 0, total: Int = 0,
                    key: Bool = false, overlapNs: UInt32 = 0) -> Data {
        var w = Writer()
        w.u8(datagramVideoChunk)
        w.u32(id)
        w.u16(UInt16(index)); w.u16(UInt16(last ? count : 0))
        w.u32(UInt32(offset)); w.u32(UInt32(last ? total : 0))
        // Only the last piece carries the keyframe bit: until the encoder
        // hands the frame back the host does not know.
        w.u8((key && last ? 1 : 0) | (last ? 2 : 0))
        w.u8(2) // AV1
        w.u16(640); w.u16(360)
        w.u64(111_000); w.u64(last ? 222_000 : 0)
        // How much of the sending happened before the bitstream was finished.
        w.u32(last ? overlapNs : 0)
        XCTAssertEqual(w.data.count, videoChunkHeader)
        return w.data + part
    }

    /// The last piece says how much of the sending happened before the
    /// bitstream was finished, so the client can count the network's share
    /// from the first byte out rather than from the end of the encode.
    func testLastChunkCarriesTheOverlap() {
        start()
        let part = Data(repeating: 7, count: 16)
        host.send(videoChunk(id: 3, index: 0, offset: 0, part))
        host.send(videoChunk(id: 3, index: 1, offset: 16, part, last: true, count: 2,
                             total: 32, overlapNs: 4_250_000))
        waitUntil { !recorder.snapshot.frames.isEmpty }
        let frame = recorder.snapshot.frames[0]
        XCTAssertEqual(frame.overlapNs, 4_250_000)
        XCTAssertEqual(frame.bitstream.count, 32)
    }

    func testChunksMakeTheFrameTheEncoderProduced() {
        start()
        // As the host sends it: whole chunks of what the GPU had written, then
        // the bytes it went back and changed, then the held-back start.
        let bitstream = Data((0..<2500).map { UInt8(($0 * 11 + 3) & 0xff) })
        var ds: [Data] = []
        var index = 0
        for offset in stride(from: 128, to: 2176, by: 512) {
            // What the GPU had written at the time, with the OBU size fields
            // it had not filled in yet.
            var early = bitstream.subdata(in: offset..<offset + 512)
            early[early.startIndex] = 0
            early[early.startIndex + 1] = 0
            ds.append(videoChunk(id: 3, index: index, offset: offset, early))
            index += 1
        }
        for offset in stride(from: 128, to: 2176, by: 512) {
            ds.append(videoChunk(id: 3, index: index, offset: offset,
                                 bitstream.subdata(in: offset..<offset + 2)))
            index += 1
        }
        ds.append(videoChunk(id: 3, index: index, offset: 2176,
                             bitstream.subdata(in: 2176..<2500)))
        index += 1
        ds.append(videoChunk(id: 3, index: index, offset: 0,
                             bitstream.subdata(in: 0..<128),
                             last: true, count: index + 1, total: 2500, key: true))
        for d in ds { host.send(d) }
        waitUntil { !recorder.snapshot.frames.isEmpty }
        let frames = recorder.snapshot.frames
        XCTAssertEqual(frames.count, 1)
        XCTAssertEqual(frames[0].id, 3)
        XCTAssertTrue(frames[0].key)
        XCTAssertEqual(frames[0].codec, 2)
        XCTAssertEqual(frames[0].width, 640)
        XCTAssertEqual(frames[0].height, 360)
        XCTAssertEqual(frames[0].slotNs, 111_000)
        XCTAssertEqual(frames[0].encodedNs, 222_000)
        XCTAssertEqual(frames[0].bitstream, bitstream, "byte for byte what the encoder produced")
    }

    func testAChunkThatRanPastTheEndOfTheFrameIsTrimmed() {
        start()
        // The GPU writes in blocks, so the last whole chunk can carry padding
        // past the end of the bitstream.
        let bitstream = Data((0..<300).map { UInt8($0 & 0xff) })
        host.send(videoChunk(id: 4, index: 0, offset: 128,
                             bitstream.subdata(in: 128..<300) + Data(repeating: 0xee, count: 84)))
        host.send(videoChunk(id: 4, index: 1, offset: 0, bitstream.subdata(in: 0..<128),
                             last: true, count: 2, total: 300))
        waitUntil { !recorder.snapshot.frames.isEmpty }
        XCTAssertEqual(recorder.snapshot.frames.first?.bitstream, bitstream)
    }

    func testChunksArriveInAnyOrderAndLaterOnesWin() {
        start()
        let bitstream = Data((0..<256).map { UInt8($0) })
        let last = videoChunk(id: 5, index: 2, offset: 0, bitstream.subdata(in: 0..<128),
                              last: true, count: 3, total: 256)
        let fixup = videoChunk(id: 5, index: 1, offset: 128, bitstream.subdata(in: 128..<130))
        let early = videoChunk(id: 5, index: 0, offset: 128,
                               Data([0, 0]) + bitstream.subdata(in: 130..<256))
        for d in [last, fixup, early] { host.send(d) }
        waitUntil { !recorder.snapshot.frames.isEmpty }
        XCTAssertEqual(recorder.snapshot.frames.first?.bitstream, bitstream,
                       "the fixup is at a higher index, so it is laid down after")
    }

    func testAFrameMissingAChunkIsNeverDelivered() {
        start()
        let bitstream = Data((0..<600).map { UInt8($0 & 0xff) })
        host.send(videoChunk(id: 6, index: 0, offset: 128, bitstream.subdata(in: 128..<400)))
        // Index 1 (400..<600) never arrives.
        host.send(videoChunk(id: 6, index: 2, offset: 0, bitstream.subdata(in: 0..<128),
                             last: true, count: 3, total: 600))
        host.send(videoChunk(id: 7, index: 0, offset: 0, bitstream.subdata(in: 0..<128),
                             last: true, count: 1, total: 128))
        waitUntil { !recorder.snapshot.frames.isEmpty }
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        XCTAssertEqual(recorder.snapshot.frames.map(\.id), [7])
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

    func testRiptideDatagrams() {
        start()
        // Packet 7 of a frame of 1920x1080 in 4 stripes (128 rows, then 256): in
        // stripe 1, whose 5 packets are numbers 4 to 8, 2 of them inter packets.
        var p = Writer()
        p.u8(datagramRiptide); p.u32(42); p.u16(7); p.u16(1920); p.u16(1080); p.u64(1_000); p.u64(2_000)
        p.u8(1); p.u32(40); p.u8(1); p.u8(4); p.u16(128); p.u16(256); p.u16(4); p.u16(5); p.u8(2)
        XCTAssertEqual(p.data.count, riptideHeader)
        host.send(p.data + Data([9, 8, 7]))
        // The last stripe's: the frame's packet count is then known.
        var q = Writer()
        q.u8(datagramRiptide); q.u32(42); q.u16(20); q.u16(1920); q.u16(1080); q.u64(1_000); q.u64(3_000)
        q.u8(1); q.u32(40); q.u8(3); q.u8(4); q.u16(128); q.u16(256); q.u16(18); q.u16(6); q.u8(1)
        host.send(q.data + Data([6]))
        waitUntil { recorder.snapshot.riptide.count == 2 }
        let pkt = recorder.snapshot.riptide.first
        XCTAssertEqual(pkt?.frameId, 42)
        XCTAssertEqual(pkt?.index, 7)
        XCTAssertEqual(pkt?.count, 0, "not the last stripe: the total is not known yet")
        XCTAssertEqual(pkt?.width, 1920)
        XCTAssertEqual(pkt?.height, 1080)
        XCTAssertEqual(pkt?.slotNs, 1_000)
        XCTAssertEqual(pkt?.encodedNs, 2_000)
        XCTAssertEqual(pkt?.predicted, true)
        XCTAssertEqual(pkt?.repaired, 40)
        XCTAssertEqual(pkt?.stripe, 1)
        XCTAssertEqual(pkt?.stripes, 4)
        XCTAssertEqual(pkt?.firstRows, 128)
        XCTAssertEqual(pkt?.stripeRows, 256)
        XCTAssertEqual(pkt?.stripeInter, 2)
        XCTAssertEqual(pkt?.stripeFirst, 4)
        XCTAssertEqual(pkt?.stripeCount, 5)
        XCTAssertEqual(pkt?.payload, Data([9, 8, 7]))
        XCTAssertEqual(recorder.snapshot.riptide.last?.count, 24)
    }

    func testRiptideLossReportIsSealed() {
        start()
        conn.riptideLoss(frameId: 77, count: 120, ranges: [(0, 2), (100, 0xffff)])
        let d = host.receiveDatagram(datagramRiptideLoss, nonceKind: 3)
        XCTAssertNotNil(d)
        guard let (seq, m) = d else { return }
        XCTAssertEqual(seq, 0)
        XCTAssertEqual(m.u32(0), 77)
        XCTAssertEqual(m.u16(4), 120)
        XCTAssertEqual(m.u16(6), 2)
        XCTAssertEqual([m.u16(8), m.u16(10), m.u16(12), m.u16(14)], [0, 2, 100, 0xffff])
    }

    func testPongGivesTheHostClock() {
        conn.start(width: 64, height: 64, refreshMhz: 60_000, kbps: 1000, codecMask: 1, preferred: 0)
        _ = host.receiveControl() // Auth
        _ = host.receiveControl() // Hello
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
