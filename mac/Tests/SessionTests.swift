import XCTest

/// A window and screen for a Session, set as each test needs.
final class FakeShell: SessionShell {
    var pixelSize = CGSize(width: 1280, height: 720)
    var scale = 2.0
    var maxFPS = 60.0
    var shortestRefresh = 1.0 / 60
    var fullScreen = false
    var vrr = false
    var displayColorSpace: CGColorSpace? = CGColorSpace(name: CGColorSpace.displayP3)
    var focused = true
    var lines: [String] = []
    var locks: [Bool] = []
    var cursorChanges = 0
    func status(_ line: String) { lines.append(line) }
    func cursorChanged() { cursorChanges += 1 }
    func setPointerLock(_ on: Bool) { locks.append(on) }
    func pacingChanged() {}
}

/// The session against a fake host on loopback: what it sends for the
/// display, the settings and input.
final class SessionTests: XCTestCase {
    var host: FakeHost!
    var shell: FakeShell!
    var session: Session!
    var saved: Options!

    override func setUp() {
        saved = options
        options = Options(["--no-keep-awake"])
        options.pacingAuto = true
        host = FakeHost()
        options.host = "127.0.0.1"
        options.port = host.port
        shell = FakeShell()
        session = Session(shell: shell)
    }

    override func tearDown() {
        session.stop()
        options = saved
    }

    /// Starts the session; returns the Hello's payload.
    func connect() -> Data {
        session.start()
        let deadline = Date().addingTimeInterval(2)
        while session.conn == nil && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
        XCTAssertNotNil(session.conn, "connected")
        return next(Msg.hello)!
    }

    /// The payload of the next control message of `type` (others skipped).
    func next(_ type: UInt8) -> Data? {
        while let m = host.receiveControl(timeout: 1)?.payload {
            if m[0] == type { return m.subdata(in: 5..<m.count) }
        }
        return nil
    }

    func testHelloAsksForTheViewInPixelsAtTheScreensRate() {
        shell.pixelSize = CGSize(width: 2733, height: 2048) // odd: rounded down to even
        shell.maxFPS = 120
        let hello = connect()
        XCTAssertEqual(hello.u16(2), 2732)
        XCTAssertEqual(hello.u16(4), 2048)
        XCTAssertEqual(hello.u32(6), 120_000)
        XCTAssertEqual(next(Msg.vrrPolicy), Data([1]), "auto on a fixed refresh: Steam's VRR off")
    }

    func testAFixedSizeAndRefreshFromTheSettingsWin() {
        options.size = (1920, 1080)
        options.refresh = 90
        let hello = connect()
        XCTAssertEqual(hello.u16(2), 1920)
        XCTAssertEqual(hello.u16(4), 1080)
        XCTAssertEqual(hello.u32(6), 90_000)
    }

    func testAutoPacingFollowsTheDisplay() {
        _ = connect()
        XCTAssertEqual(next(Msg.vrrPolicy), Data([1]))
        XCTAssertTrue(session.pacing.phaseLock)
        XCTAssertFalse(session.renderer.holdFrames)
        shell.fullScreen = true
        shell.vrr = true
        session.applyPresentMode()
        XCTAssertFalse(session.pacing.phaseLock)
        XCTAssertTrue(session.renderer.holdFrames)
        XCTAssertEqual(next(Msg.vrrPolicy), Data([2]), "VRR: Steam's VRR on")
        options.pacingAuto = false
        session.apply(.phaseLock)
        XCTAssertEqual(next(Msg.vrrPolicy), Data([0]), "manual: Steam's toggle left alone")
    }

    func testVsyncStaysOnUntilTheGridIsMeasured() {
        options.immediateFullScreen = true
        shell.fullScreen = true
        session.applyPresentMode()
        XCTAssertFalse(session.immediate, "phase lock with no grid yet")
        XCTAssertTrue(session.renderer.layer.displaySyncEnabled)
        options.phaseLock = false
        options.pacingAuto = false
        session.applyPresentMode()
        XCTAssertTrue(session.immediate, "no phase lock: vsync off at once")
        XCTAssertFalse(session.renderer.layer.displaySyncEnabled)
    }

    func testSettingsChangesReachTheHost() {
        _ = connect()
        options.mbps = 150
        session.apply(.bitrate)
        XCTAssertEqual(next(Msg.bitrate)?.u32(0), 150_000)
        options.size = (1600, 900)
        session.apply(.display)
        let c = next(Msg.configure)
        XCTAssertEqual(c?.u16(0), 1600)
        XCTAssertEqual(c?.u16(2), 900)
    }

    func testKeysAreSentOnceAndAllReleasedAtTheEnd() {
        _ = connect()
        session.key(30, down: true)
        session.key(30, down: true) // already down
        session.key(42, down: true)
        session.key(42, down: false)
        session.key(42, down: false) // already up
        session.releaseAll()
        var keys: [String] = []
        while let k = next(Msg.key) { keys.append("\(k.u16(0))\(k[2] == 1 ? "+" : "-")") }
        XCTAssertEqual(keys, ["30+", "42+", "42-", "30-"])
    }

    func testAClickWhileTheHostWantsThePointerCapturesItInstead() {
        _ = connect()
        shell.focused = false
        session.pointerLock(true)
        XCTAssertEqual(shell.locks, [false], "not focused: not captured yet")
        session.button(Session.leftButton, down: true)
        XCTAssertEqual(shell.locks, [false, true])
        session.lockActive = true // the shell captured it
        session.button(Session.rightButton, down: true)
        let b = next(Msg.button)
        XCTAssertEqual(b?.u16(0), Session.rightButton, "the capturing click is not sent")
        XCTAssertEqual(b?[2], 1)
    }

    func testPointerPositionsAndScrolling() {
        _ = connect()
        session.position(x: 0.25, y: 1.5)
        let p = next(Msg.position)!
        XCTAssertEqual(Float(bitPattern: p.u32(0)), 0.25)
        XCTAssertEqual(Float(bitPattern: p.u32(4)), 1, "clamped to the video")
        session.scroll(dx: 0, dy: 100) // one notch down
        let s = next(Msg.scroll)!
        XCTAssertEqual(Float(bitPattern: s.u32(4)), 15, accuracy: 1e-4)
        XCTAssertEqual(s.i16(8), 0)
        XCTAssertEqual(s.i16(10), 120)
    }

    func testTouches() {
        _ = connect()
        session.touch(3, .down, x: 0.5, y: -0.2)
        session.touch(0, .cancel)
        let d = next(Msg.touch)!
        XCTAssertEqual(Array(d[0..<2]), [3, 0])
        XCTAssertEqual(Float(bitPattern: d.u32(2)), 0.5)
        XCTAssertEqual(Float(bitPattern: d.u32(6)), 0, "clamped to the video")
        XCTAssertEqual(next(Msg.touch)?[1], 3)
    }

    func testVideoIsLetterboxedInTheView() {
        session.streamSize = CGSize(width: 1920, height: 1080)
        XCTAssertGreaterThan(shell.cursorChanges, 0, "the cursor's scale follows the stream's size")
        let r = session.videoRect(in: CGSize(width: 1366, height: 1024)) // an iPad, 4:3
        XCTAssertEqual(r.width, 1366)
        XCTAssertEqual(r.height, 768.375, accuracy: 1e-9)
        XCTAssertEqual(r.minY, (1024 - 768.375) / 2, accuracy: 1e-9)
        XCTAssertEqual(r.minX, 0)
    }

    func testForceClientCursorShowsAnArrowWhereTheHostShowsNone() {
        session.cursor(width: 0, height: 0, hotX: 0, hotY: 0, rgba: Data())
        XCTAssertNil(session.clientCursor, "the host's call by default")
        options.forceClientCursor = true
        let changes = shell.cursorChanges
        session.apply(.cursor)
        XCTAssertEqual(shell.cursorChanges, changes + 1, "the app redraws it at once")
        XCTAssertNotNil(session.clientCursor)
        XCTAssertNil(session.clientCursor?.image, "the app's arrow")
        session.cursor(width: 2, height: 2, hotX: 1, hotY: 0, rgba: Data(count: 16))
        XCTAssertEqual(session.clientCursor?.hotX, 1, "the host's image when it has one")
    }

    func testHostCursorImage() {
        var rgba = Data(count: 2 * 2 * 4)
        rgba[0] = 255; rgba[3] = 255 // top left red, opaque
        session.cursor(width: 2, height: 2, hotX: 1, hotY: 0, rgba: rgba)
        let image = session.hostCursor?.image
        XCTAssertEqual(image?.width, 2)
        let t = image.flatMap { session.renderer.texture(from: $0, width: 2, height: 2) }
        var px = [UInt8](repeating: 0, count: 16)
        t?.getBytes(&px, bytesPerRow: 8, from: MTLRegionMake2D(0, 0, 2, 2), mipmapLevel: 0)
        XCTAssertEqual(Array(px[0..<4]), [255, 0, 0, 255], "top row first")
        XCTAssertEqual(Array(px[4..<8]), [0, 0, 0, 0])
        session.cursor(width: 1, height: 1, hotX: 0, hotY: 0, rgba: Data())
        XCTAssertNotNil(session.hostCursor, "a cursor shown")
        XCTAssertNil(session.hostCursor?.image, "with no usable image")
        session.cursor(width: 0, height: 0, hotX: 0, hotY: 0, rgba: Data())
        XCTAssertNil(session.hostCursor, "none shown")
    }
}
