import XCTest

/// A window and screen for a Session, set as each test needs.
final class FakeShell: SessionShell {
    var pixelSize = CGSize(width: 1280, height: 720)
    var scale = 2.0
    var maxFPS = 60.0
    var shortestRefresh = 1.0 / 60
    var fullScreen = false
    var vrr = false
    var vrrCapable = false
    var displayColorSpace: CGColorSpace? = CGColorSpace(name: CGColorSpace.displayP3)
    var focused = true
    var capturesMouse = false
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
        options.pacingMode = .auto
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
        shell.vrrCapable = true
        options.pacingMode = .manual
        options.vrrMode = .steam
        session.apply(.phaseLock)
        XCTAssertEqual(next(Msg.vrrPolicy), Data([0]), "manual, Steam: its toggle left alone")
        options.pacingMode = .lowLatency
        session.apply(.phaseLock)
        XCTAssertEqual(next(Msg.vrrPolicy), Data([2]), "low latency, a capable display: on")
        XCTAssertFalse(session.pacing.phaseLock)
        XCTAssertFalse(session.renderer.holdFrames)
    }

    func testVsyncStaysOnUntilTheGridIsMeasured() {
        options.immediateFullScreen = true
        options.pacingMode = .manual
        options.phaseLock = true
        shell.fullScreen = true
        session.applyPresentMode()
        XCTAssertFalse(session.immediate, "phase lock with no grid yet")
        XCTAssertTrue(session.renderer.layer.displaySyncEnabled)
        options.phaseLock = false
        options.vrrMode = .on
        shell.vrrCapable = true
        session.applyPresentMode()
        XCTAssertTrue(session.immediate, "no phase lock: vsync off at once")
        XCTAssertFalse(session.renderer.layer.displaySyncEnabled)
        options.pacingMode = .auto
        session.applyPresentMode()
        XCTAssertFalse(session.immediate, "the setting is Manual's")
        options.immediateFullScreen = false
        options.pacingMode = .lowLatency
        session.applyPresentMode()
        XCTAssertTrue(session.immediate, "low latency: vsync off, whatever the setting says")
        shell.fullScreen = false
        session.applyPresentMode()
        XCTAssertFalse(session.immediate, "only in full screen")
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

    func testAClickCapturesThePointer() {
        shell.capturesMouse = true
        shell.focused = false
        _ = connect()
        XCTAssertFalse(shell.locks.contains(true), "not focused: not captured yet")
        session.button(Session.leftButton, down: true)
        XCTAssertEqual(shell.locks.last, true)
        session.lockActive = true // the shell captured it
        session.button(Session.rightButton, down: true)
        let b = next(Msg.button)
        XCTAssertEqual(b?.u16(0), Session.rightButton, "the capturing click is not sent")
        XCTAssertEqual(b?[2], 1)
    }

    func testTheMouseIsCapturedInEverySetting() {
        shell.capturesMouse = true
        _ = connect()
        for mode in CursorMode.allCases {
            for mouse in MouseMode.allCases {
                for c in Confinement.allCases {
                    (options.cursorMode, options.mouseMode, options.confinement) = (mode, mouse, c)
                    session.apply(.cursor)
                    XCTAssertEqual(shell.locks.last, true, "\(mode.name) \(mouse.name) \(c.name)")
                }
            }
        }
        session.relative(true)
        session.relative(false)
        XCTAssertEqual(shell.locks.last, true, "whatever the game asks")
    }

    func testAShellThatCantCaptureOnlyDoesForRelative() {
        _ = connect()
        session.relative(true)
        XCTAssertEqual(shell.locks.last, true, "Auto follows the game")
        session.relative(false)
        XCTAssertEqual(shell.locks.last, false)
    }

    func testAutoFollowsTheGameAndManualDoesNot() {
        XCTAssertFalse(session.confined, "Auto, no clip: free")
        XCTAssertFalse(session.sendsMotion, "Auto, absolute")
        session.relative(true)
        XCTAssertFalse(session.confined, "relative says nothing about the clip")
        XCTAssertTrue(session.sendsMotion, "Auto, relative")
        session.cursorClip(CGRect(x: 0, y: 0, width: 10, height: 10))
        XCTAssertTrue(session.confined, "Auto, a clip")
        options.confinement = .none
        XCTAssertFalse(session.confined, "None stands, whatever the game asks")
        options.mouseMode = .absolute
        XCTAssertFalse(session.sendsMotion, "as does the mouse's")
        session.relative(false)
        options.mouseMode = .relative
        XCTAssertTrue(session.sendsMotion, "with the game asking for positions")
    }

    func testMouseAndConfinementAreIndependent() {
        options.mouseMode = .relative
        XCTAssertTrue(session.sendsMotion)
        XCTAssertFalse(session.confined)
        options.mouseMode = .absolute
        session.cursorClip(CGRect(x: 0, y: 0, width: 10, height: 10))
        XCTAssertFalse(session.sendsMotion)
        XCTAssertTrue(session.confined)
    }

    func testTheCursorFollowsGamescopeUnlessSetOtherwise() {
        XCTAssertNil(session.clientCursor, "Auto, no image: hidden")
        session.cursor(width: 2, height: 2, hotX: 1, hotY: 0, rgba: Data(count: 16))
        XCTAssertEqual(session.clientCursor?.hotX, 1, "Auto, an image: drawn")
        options.cursorMode = .hide
        XCTAssertNil(session.clientCursor)
        session.cursor(width: 0, height: 0, hotX: 0, hotY: 0, rgba: Data())
        options.cursorMode = .show
        XCTAssertNil(session.clientCursor?.image, "Show, no image: the arrow")
        XCTAssertNotNil(session.clientCursor)
    }

    func testTheCursorSourceIsWhatGamescopeSends() {
        XCTAssertEqual(session.cursorSource, "remote")
        session.cursor(width: 2, height: 2, hotX: 1, hotY: 0, rgba: Data(count: 16))
        XCTAssertEqual(session.cursorSource, "hardware")
        options.cursorMode = .hide
        XCTAssertEqual(session.cursorSource, "hardware", "what we draw doesn't change what gamescope sends")
        session.cursor(width: 0, height: 0, hotX: 0, hotY: 0, rgba: Data())
        options.cursorMode = .show
        XCTAssertEqual(session.cursorSource, "remote", "Show's arrow is ours, not the game's")
    }

    func testCursorAreaIsTheClipInsideTheVideo() {
        session.streamSize = CGSize(width: 3840, height: 2160)
        let view = CGSize(width: 1920, height: 1200) // half size, letterboxed: y 60...1140
        XCTAssertEqual(session.cursorArea(in: view), session.videoRect(in: view), "no clip: all of it")
        session.cursorClip(CGRect(x: 483, y: 3, width: 2874, height: 2154)) // WC3's on a 4K stream
        let r = session.cursorArea(in: view)
        XCTAssertEqual(r.minX, 241.5, accuracy: 1e-9)
        XCTAssertEqual(r.minY, 61.5, accuracy: 1e-9)
        XCTAssertEqual(r.width, 1437, accuracy: 1e-9)
        XCTAssertEqual(r.height, 1077, accuracy: 1e-9)
        options.confinement = .none
        XCTAssertEqual(session.cursorArea(in: view), session.videoRect(in: view), "None: all of it")
        options.confinement = .auto
        session.relative(true)
        XCTAssertEqual(session.cursorArea(in: view), r, "with relative too")
        session.relative(false)
        session.cursorClip(nil) // gamescope: Steam took the mouse
        XCTAssertFalse(session.confined)
        XCTAssertEqual(session.cursorArea(in: view), session.videoRect(in: view))
    }

    func testPointerPositionsAndScrolling() {
        _ = connect()
        session.position(x: 0.25, y: 1.5)
        let p = host.receiveMouse()!.payload
        XCTAssertEqual(p[0], 1)
        XCTAssertEqual(Float(bitPattern: p.u32(1)), 0.25)
        XCTAssertEqual(Float(bitPattern: p.u32(5)), 1, "clamped to the video")
        session.button(Session.leftButton, down: true)
        let c = next(Msg.position)!
        XCTAssertEqual(Float(bitPattern: c.u32(0)), 0.25, "a click goes where the mouse is, in order")
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
        options.cursorMode = .show
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
