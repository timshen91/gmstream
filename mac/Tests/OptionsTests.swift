import XCTest

/// Settings: saved values, command-line overrides, and the pacing they pick.
/// UserDefaults.standard here is the test runner's own domain, not the app's.
final class OptionsTests: XCTestCase {
    static let keys = ["host", "port", "codec", "hevc", "mbps", "refresh", "size", "showStats", "immediateFullScreen",
                       "vividColors", "keepWiFiAwake", "phaseLock", "holdFrames", "pacingAuto", "touchscreen", "forceClientCursor"]

    override func setUp() { clear() }
    override func tearDown() { clear() }
    func clear() { Self.keys.forEach { UserDefaults.standard.removeObject(forKey: $0) } }

    func testDefaults() {
        let o = Options([])
        XCTAssertEqual(o.host, "steamdeck.local")
        XCTAssertEqual(o.port, 8444)
        XCTAssertEqual(o.codec, 0)
        XCTAssertEqual(o.mbps, 40)
        XCTAssertNil(o.refresh)
        XCTAssertNil(o.size)
        XCTAssertTrue(o.showStats)
        XCTAssertFalse(o.immediateFullScreen)
        XCTAssertFalse(o.vividColors)
        XCTAssertTrue(o.keepWiFiAwake)
        XCTAssertTrue(o.phaseLock)
        XCTAssertFalse(o.holdFrames)
        XCTAssertTrue(o.pacingAuto)
        XCTAssertTrue(o.touchscreen)
        XCTAssertFalse(o.forceClientCursor)
        XCTAssertFalse(o.mock)
    }

    func testCommandLine() {
        let o = Options(["--pyrowave", "--bitrate", "200", "--refresh", "120", "--size", "1281x721", "--port", "9000",
                         "-NSDocumentRevisionsDebugMode", "YES", "myhost", "--no-vsync", "--vivid", "--no-keep-awake",
                         "--hold", "--fullscreen", "--screen", "DELL", "--mock", "--layer", "a=1,b=2"])
        XCTAssertEqual(o.codec, 3)
        XCTAssertEqual(o.mbps, 200)
        XCTAssertEqual(o.refresh, 120)
        XCTAssertEqual(o.size?.0, 1280)
        XCTAssertEqual(o.size?.1, 720)
        XCTAssertEqual(o.port, 9000)
        XCTAssertEqual(o.host, "myhost", "a flag's value is skipped, not taken as the host")
        XCTAssertTrue(o.immediateFullScreen)
        XCTAssertTrue(o.vividColors)
        XCTAssertFalse(o.keepWiFiAwake)
        XCTAssertTrue(o.holdFrames)
        XCTAssertFalse(o.pacingAuto, "--hold means manual pacing")
        XCTAssertTrue(o.fullScreen)
        XCTAssertEqual(o.screen, "DELL")
        XCTAssertTrue(o.mock)
        XCTAssertEqual(o.layerKeys, ["a": 1, "b": 2])
        XCTAssertEqual(Options(["--hevc"]).codec, 1)
        XCTAssertEqual(Options(["--av1"]).codec, 2)
        XCTAssertTrue(Options(["--manual-pacing", "--auto-pacing"]).pacingAuto)
        XCTAssertFalse(Options(["--no-phase"]).phaseLock)
        XCTAssertFalse(Options(["--touch-mouse"]).touchscreen)
        XCTAssertTrue(Options(["--force-client-cursor"]).forceClientCursor)
    }

    func testSavedValuesThenTheCommandLineOnTop() {
        UserDefaults.standard.set(123, forKey: "mbps")
        UserDefaults.standard.set(3, forKey: "codec")
        UserDefaults.standard.set(false, forKey: "pacingAuto")
        UserDefaults.standard.set("", forKey: "host") // empty: the default
        let saved = Options([])
        XCTAssertEqual(saved.mbps, 123)
        XCTAssertEqual(saved.codec, 3)
        XCTAssertFalse(saved.pacingAuto)
        XCTAssertEqual(saved.host, "steamdeck.local")
        XCTAssertEqual(Options(["--bitrate", "50"]).mbps, 50)
        UserDefaults.standard.removeObject(forKey: "codec")
        UserDefaults.standard.set(true, forKey: "hevc") // older settings
        XCTAssertEqual(Options([]).codec, 1)
    }

    func testSaveRoundTrips() {
        var o = Options([])
        o.host = "h"; o.port = 1234; o.codec = 3; o.mbps = 300; o.refresh = 117; o.size = (2560, 1600)
        o.showStats = false; o.immediateFullScreen = true; o.vividColors = true; o.keepWiFiAwake = false
        o.phaseLock = false; o.holdFrames = true; o.pacingAuto = false; o.touchscreen = false
        o.forceClientCursor = true
        o.save()
        let r = Options([])
        XCTAssertEqual(r.host, "h")
        XCTAssertEqual(r.port, 1234)
        XCTAssertEqual(r.codec, 3)
        XCTAssertEqual(r.mbps, 300)
        XCTAssertEqual(r.refresh, 117)
        XCTAssertEqual(r.sizeText, "2560x1600")
        XCTAssertFalse(r.showStats)
        XCTAssertTrue(r.immediateFullScreen)
        XCTAssertTrue(r.vividColors)
        XCTAssertFalse(r.keepWiFiAwake)
        XCTAssertFalse(r.phaseLock)
        XCTAssertTrue(r.holdFrames)
        XCTAssertFalse(r.pacingAuto)
        XCTAssertFalse(r.touchscreen)
        XCTAssertTrue(r.forceClientCursor)
        o.refresh = nil; o.size = nil
        o.save()
        XCTAssertNil(Options([]).refresh, "0 saved: match the display")
        XCTAssertNil(Options([]).size)
    }

    func testParseSize() {
        XCTAssertEqual(Options.parseSize("2560x1600").map { [$0.0, $0.1] }, [2560, 1600])
        XCTAssertEqual(Options.parseSize(" 1921 X 1081 ").map { [$0.0, $0.1] }, [1920, 1080], "rounded down to even")
        XCTAssertNil(Options.parseSize(""))
        XCTAssertNil(Options.parseSize("1920"))
        XCTAssertNil(Options.parseSize("0x1080"))
        XCTAssertNil(Options.parseSize("wide x tall"))
        XCTAssertNil(Options.parseSize("1x2x3"))
    }

    func testParseKeys() {
        XCTAssertEqual(Options.parseKeys("a=1,b=0.5,junk,c=x,=3"), ["a": 1, "b": 0.5])
        XCTAssertEqual(Options.parseKeys(""), [:])
    }
}

final class PacingTests: XCTestCase {
    /// A screen with a refresh range (minimum and maximum intervals).
    final class FakeScreen: NSScreen {
        var range: (min: Double, max: Double) = (1.0 / 120, 1.0 / 120)
        override var minimumRefreshInterval: TimeInterval { range.min }
        override var maximumRefreshInterval: TimeInterval { range.max }
    }

    func options(auto: Bool, phaseLock: Bool = true, hold: Bool = false) -> Options {
        var o = Options(["--no-keep-awake"])
        o.pacingAuto = auto
        o.phaseLock = phaseLock
        o.holdFrames = hold
        return o
    }

    func testAutoOnAFixedRefreshDisplayOrInAWindowLocksPhaseAndTurnsSteamVRROff() {
        let fixed = FakeScreen()
        let vrr = FakeScreen()
        vrr.range = (1.0 / 120, 1.0 / 48)
        for (fullScreen, screen) in [(true, fixed), (false, vrr), (true, nil)] as [(Bool, NSScreen?)] {
            let p = Pacing(options: options(auto: true), fullScreen: fullScreen, screen: screen)
            XCTAssertFalse(p.vrr)
            XCTAssertTrue(p.phaseLock)
            XCTAssertFalse(p.hold)
            XCTAssertEqual(p.steamVRR, 1)
        }
    }

    func testAutoInFullScreenOnAVRRDisplayHoldsAndTurnsSteamVRROn() {
        let screen = FakeScreen()
        screen.range = (1.0 / 120, 1.0 / 48)
        let p = Pacing(options: options(auto: true), fullScreen: true, screen: screen)
        XCTAssertTrue(p.vrr)
        XCTAssertFalse(p.phaseLock)
        XCTAssertTrue(p.hold)
        XCTAssertEqual(p.steamVRR, 2)
    }

    func testManualTakesTheSettingsAndLeavesSteamAlone() {
        let screen = FakeScreen()
        screen.range = (1.0 / 120, 1.0 / 48)
        let p = Pacing(options: options(auto: false, phaseLock: false, hold: true), fullScreen: true, screen: screen)
        XCTAssertFalse(p.phaseLock)
        XCTAssertTrue(p.hold)
        XCTAssertEqual(p.steamVRR, 0)
        let q = Pacing(options: options(auto: false, phaseLock: true, hold: false), fullScreen: false, screen: nil)
        XCTAssertTrue(q.phaseLock)
        XCTAssertFalse(q.hold)
        XCTAssertEqual(q.steamVRR, 0)
    }
}
