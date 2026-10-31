// The client's settings, and the pacing they (or the display) choose.

import Foundation

/// This launch's settings; the Settings window (or sheet) changes and saves them.
var options = Options(Array(CommandLine.arguments.dropFirst()))

/// Settings: saved ones (Settings window, Cmd+,; on iPad a three-finger
/// tap), then command-line options on top for this launch.
struct Options {
    var host = "steamdeck.local"
    var port: UInt16 = 8444
    var codec: UInt8 = 0 // preferred: 0 H.264, 1 HEVC, 2 AV1, 3 PyroWave
    var mbps: UInt32 = 40
    var refresh: Double? // nil: the display's rate
    var size: (Int, Int)? // nil: the window's pixel size
    var showStats = true
    var statsLog = false // print the stats every second (measuring)
    var fullScreen = false
    var screen: String? // start on the screen whose name contains this
    var mock = false // measuring: local test frames, no host (Mock.swift)
    // Full screen with vsync off: macOS flips frames straight to the panel
    // (measured total 13.7 / 17 ms p50 / p90 against ~29 with vsync), but
    // late frames may tear. Only in full screen: in a window vsync off is
    // erratic.
    var immediateFullScreen = false
    // The stream's sRGB pixels shown as the display's own colors, without
    // macOS's color management: on a wide-gamut display (P3), more saturated
    // than the game intends, as on Windows (no color management) or the
    // Steam Deck's screen (Steam's color vibrance).
    var vividColors = false
    // A tiny datagram to the host 60 times a second, so this Mac's Wi-Fi
    // doesn't doze while only receiving (Connection.keepAwake).
    var keepWiFiAwake = true
    // Phase lock (Phase.swift): gamescope produces frames on this display's
    // refresh grid. Off: an empty grid, so gamescope paces itself and frames
    // are shown as they arrive (as on a variable-refresh display).
    var phaseLock = true
    // Hold each frame until a fixed delay after the host presented it
    // (Video.swift, Hold): the network's jitter traded for a constant delay.
    var holdFrames = false
    // Pacing chosen from the display (Session.pacing) instead of the two
    // settings above, Steam's VRR toggle included.
    var pacingAuto = true
    // iPad: fingers are a touchscreen on the host (Steam's touch input, as
    // on a Deck); off, a finger is the mouse with its left button held.
    var touchscreen = true
    // The cursor is always the client's in absolute mode: where the host shows
    // none (a game that hides it, or paints its own into the picture), the
    // system's arrow (Session.clientCursor).
    var forceClientCursor = false
    var layerKeys: [String: Double] = [:], drawableKeys: [String: Double] = [:] // measuring
    var bgra = false // decoder output BGRA instead of NV12 (measuring)
    var probe: Double?

    init(_ args: [String]) {
        let d = UserDefaults.standard
        if let v = d.string(forKey: "host"), !v.isEmpty { host = v }
        if d.integer(forKey: "port") > 0 { port = UInt16(clamping: d.integer(forKey: "port")) }
        if let c = d.object(forKey: "codec") as? Int { codec = UInt8(c) } else if d.bool(forKey: "hevc") { codec = 1 }
        if d.integer(forKey: "mbps") > 0 { mbps = UInt32(d.integer(forKey: "mbps")) }
        if d.double(forKey: "refresh") > 0 { refresh = d.double(forKey: "refresh") }
        size = Options.parseSize(d.string(forKey: "size") ?? "")
        if d.object(forKey: "showStats") != nil { showStats = d.bool(forKey: "showStats") }
        immediateFullScreen = d.bool(forKey: "immediateFullScreen")
        vividColors = d.bool(forKey: "vividColors")
        if d.object(forKey: "keepWiFiAwake") != nil { keepWiFiAwake = d.bool(forKey: "keepWiFiAwake") }
        if d.object(forKey: "phaseLock") != nil { phaseLock = d.bool(forKey: "phaseLock") }
        holdFrames = d.bool(forKey: "holdFrames")
        if d.object(forKey: "pacingAuto") != nil { pacingAuto = d.bool(forKey: "pacingAuto") }
        if d.object(forKey: "touchscreen") != nil { touchscreen = d.bool(forKey: "touchscreen") }
        forceClientCursor = d.bool(forKey: "forceClientCursor")

        var it = args.makeIterator()
        while let a = it.next() {
            switch a {
            case "--port": port = UInt16(it.next() ?? "") ?? port
            case "--hevc": codec = 1
            case "--av1": codec = 2
            case "--pyrowave": codec = 3
            case "--bitrate": mbps = UInt32(it.next() ?? "") ?? mbps
            case "--refresh": refresh = Double(it.next() ?? "")
            case "--size": size = Options.parseSize(it.next() ?? "")
            case "--probe": probe = Double(it.next() ?? "")
            case "--bgra": bgra = true
            case "--stats-log": statsLog = true
            case "--hide-stats": showStats = false
            case "--fullscreen": fullScreen = true
            case "--screen": screen = it.next()
            case "--mock": mock = true
            case "--no-vsync": immediateFullScreen = true
            case "--vivid": vividColors = true
            case "--no-keep-awake": keepWiFiAwake = false
            case "--no-phase": phaseLock = false
            case "--hold": holdFrames = true; pacingAuto = false
            case "--no-hold": holdFrames = false; pacingAuto = false
            case "--auto-pacing": pacingAuto = true
            case "--manual-pacing": pacingAuto = false
            case "--touch-mouse": touchscreen = false
            case "--force-client-cursor": forceClientCursor = true
            case "--layer": layerKeys = Options.parseKeys(it.next() ?? "")
            case "--drawable": drawableKeys = Options.parseKeys(it.next() ?? "")
            case let s where s.hasPrefix("-"): _ = it.next() // e.g. -NSDocumentRevisionsDebugMode YES
            default: host = a
            }
        }
    }

    static func parseSize(_ s: String) -> (Int, Int)? {
        let p = s.lowercased().split(separator: "x").compactMap { Int($0.trimmingCharacters(in: .whitespaces)) }
        return p.count == 2 && p[0] > 0 && p[1] > 0 ? (p[0] & ~1, p[1] & ~1) : nil
    }

    /// "a=1,b=0" → ["a": 1, "b": 0]
    static func parseKeys(_ s: String) -> [String: Double] {
        var r: [String: Double] = [:]
        for kv in s.split(separator: ",") {
            let p = kv.split(separator: "=")
            if p.count == 2, let v = Double(p[1]) { r[String(p[0])] = v }
        }
        return r
    }

    var sizeText: String { size.map { "\($0.0)x\($0.1)" } ?? "" }

    func save() {
        let d = UserDefaults.standard
        d.set(host, forKey: "host")
        d.set(Int(port), forKey: "port")
        d.set(Int(codec), forKey: "codec")
        d.set(Int(mbps), forKey: "mbps")
        d.set(refresh ?? 0, forKey: "refresh")
        d.set(sizeText, forKey: "size")
        d.set(showStats, forKey: "showStats")
        d.set(immediateFullScreen, forKey: "immediateFullScreen")
        d.set(vividColors, forKey: "vividColors")
        d.set(keepWiFiAwake, forKey: "keepWiFiAwake")
        d.set(phaseLock, forKey: "phaseLock")
        d.set(holdFrames, forKey: "holdFrames")
        d.set(pacingAuto, forKey: "pacingAuto")
        d.set(touchscreen, forKey: "touchscreen")
        d.set(forceClientCursor, forKey: "forceClientCursor")
    }
}

/// How frames are paced, from the settings or (Pacing: Auto) from the display.
/// The two models do not mix. A variable-refresh display (Adaptive-Sync, in
/// full screen: in a window macOS composites the desktop at a fixed rate)
/// shows a frame when it comes, so the game runs free on Steam's VRR and the
/// hold turns everything after the host into a constant delay. A
/// fixed-refresh one has its own clock: Steam's VRR off, so the game steps at
/// a fixed rate on gamescope's vblanks, and the phase lock makes those this
/// display's refreshes. The Dell U2725QE in full screen does VRR (test frames
/// at 100 Hz flipped 10.0 ms apart) as long as nothing redraws on another
/// display and the host stays under its top rate (PanelRate). An iPad's
/// ProMotion panel steps between fixed rates: the apps keep it at its top
/// rate, a fixed refresh.
struct Pacing {
    let vrr: Bool       // Auto: this display does VRR here
    let phaseLock: Bool
    let hold: Bool
    let steamVRR: UInt8 // protocol VrrPolicy: 0 Steam's own, 1 off, 2 on

    /// `vrr`: the display does VRR where the stream is (SessionShell.vrr).
    init(options: Options, vrr: Bool) {
        self.vrr = vrr
        if options.pacingAuto {
            (phaseLock, hold, steamVRR) = vrr ? (false, true, 2) : (true, false, 1)
        } else {
            (phaseLock, hold, steamVRR) = (options.phaseLock, options.holdFrames, 0)
        }
    }
}
