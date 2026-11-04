// The client's settings, and the pacing they (or the display) choose.

import Foundation
#if os(macOS)
import AppKit
#else
import UIKit
#endif

/// Whether this device can show HDR10, and the user has not turned it off.
/// Both platforms report the headroom the panel could give beyond SDR white;
/// 1.0 means it has none. Any attached screen counts: the window can be
/// moved, and the host only needs to know whether HDR is worth sending.
///
/// Asked once, on the main thread (`primeHDRCapable`), because the Hello that
/// carries it is written on the connection's thread and the screen list is
/// AppKit's.
private var hdrCapable: Bool?

/// Asks the screens again. The headroom a display reports follows the
/// system's HDR setting, not just the panel, so this is read fresh for each
/// connection rather than remembered.
func primeHDRCapable() {
    #if os(macOS)
    let best = NSScreen.screens
        .map { $0.maximumPotentialExtendedDynamicRangeColorComponentValue }.max() ?? 1
    NSLog("gmstream: HDR %@ (most headroom any screen offers: %.2f)",
          best > 1.05 ? "available" : "off", best)
    hdrCapable = best > 1.05
    #else
    if #available(iOS 16.0, *) {
        hdrCapable = UIScreen.main.potentialEDRHeadroom > 1.05
    } else {
        hdrCapable = false
    }
    #endif
}

func canShowHDR() -> Bool {
    options.hdr && hdrCapable == true
}

/// This launch's settings; the Settings window (or sheet) changes and saves them.
var options = Options(Array(CommandLine.arguments.dropFirst()))

/// How frames reach the screen. Auto: chosen from the display (Pacing). Low
/// latency: as they come, no phase lock, no hold, vsync off. Manual: the three
/// settings as they are.
enum PacingMode: Int, CaseIterable {
    case auto, lowLatency, manual
    var title: String { ["Auto (from the display)", "Low latency", "Manual"][rawValue] }
}

/// Manual pacing: the host's VRR. A display that can't do VRR has it off.
/// Auto: on where it really happens, in full screen on a capable display (the
/// host then runs just under the panel's top rate, PanelRate), else off. On: on,
/// whatever Steam's toggle shows. Steam: left to Steam's toggle.
enum VRRMode: Int, CaseIterable {
    case auto, off, on, steam
    var title: String { ["Auto", "Off", "On (capable display)", "Steam (capable display)"][rawValue] }
    var name: String { ["auto", "off", "on", "steam"][rawValue] }
}

/// Whether the client draws its cursor. Auto: when gamescope has a cursor
/// image, not when it has none (a game that hid its cursor, or draws its own
/// into the picture). Show: always, with an arrow if there is no image.
enum CursorMode: Int, CaseIterable {
    case auto, show, hide
    var title: String { ["Auto (as gamescope shows it)", "Show", "Hide"][rawValue] }
    var name: String { ["auto", "show", "hide"][rawValue] }
}

/// What the game gets. Auto: motion while it asks for a lock, else positions.
/// Manual settings stand, whatever the game asks.
enum MouseMode: Int, CaseIterable {
    case auto, absolute, relative
    var title: String { ["Auto (as the game asks)", "Absolute", "Relative"][rawValue] }
    var name: String { ["auto", "absolute", "relative"][rawValue] }
}

/// Whether the cursor keeps to the game's clip. Auto: it does, as far as the
/// game has one and has the mouse. None: it goes anywhere in the view, whatever
/// the game asks. Either way it only applies while the mouse is captured (full
/// screen): windowed the pointer is the system's and goes where it likes, in
/// and out of the window (Session.wantsCapture, Session.confined).
enum Confinement: Int, CaseIterable {
    case auto, none
    var title: String { ["Auto (as the game asks)", "None"][rawValue] }
    var name: String { ["auto", "none"][rawValue] }
}

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
    // Manual pacing: vsync off in full screen. macOS flips frames straight to
    // the panel (measured total 13.7 / 17 ms p50 / p90 against ~29 with vsync),
    // but late frames may tear. Only in full screen: in a window vsync off is
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
    // PyroWave: keep the GPU's clock up while frames arrive, so each stripe's
    // decode runs at full speed (GPUWarmer).
    var keepGPUAwake = true
    // Take each frame in pieces as the host's encoder writes it, so it is on
    // the wire before it is finished (AV1 and HEVC; core/vkencode.c). Off: the
    // host sends whole frames, as before.
    var streamFrames = true
    // PyroWave with inter frames: each frame predicted from the last one, the
    // client's losses reported back so the host follows (PROTOCOL.md). Off:
    // every frame stands alone, as before.
    var pyrowaveInter = true
    // Each only means something for its own codecs, so each is asked for only
    // with them (and its setting is greyed out otherwise).
    static func takesFramesInPieces(_ codec: UInt8) -> Bool { codec == 1 || codec == 2 }
    static func takesInterFrames(_ codec: UInt8) -> Bool { codec == 3 }
    // Manual pacing. Phase lock (Phase.swift): gamescope produces frames on this display's
    // refresh grid. Off: an empty grid, so gamescope paces itself and frames
    // are shown as they arrive (as on a variable-refresh display).
    var phaseLock = true
    // Manual pacing. Hold each frame until a fixed delay after the host presented it
    // (Video.swift, Hold): the network's jitter traded for a constant delay.
    var holdFrames = false
    var pacingMode = PacingMode.auto
    var vrrMode = VRRMode.auto
    // iPad: fingers are a touchscreen on the host (Steam's touch input, as
    // on a Deck); off, a finger is the mouse with its left button held.
    var touchscreen = true
    var cursorMode = CursorMode.auto
    var mouseMode = MouseMode.auto
    var confinement = Confinement.auto
    // How many parts the host cuts each frame into, so that the encode and the
    // network overlap: AV1 tiles or HEVC slice segments. 0 asks for the
    // codec's own default (AV1 as many as the hardware allows, HEVC one, which
    // is what measured best -- see DESIGN.md 12.6). Each part costs bits, so
    // this is worth turning down as well as up.
    var parallelism = 0
    var layerKeys: [String: Double] = [:], drawableKeys: [String: Double] = [:] // measuring
    var bgra = false // decoder output BGRA instead of NV12 (measuring)
    // Tell the host this client can show HDR10, when the display it is on can
    // (--no-hdr says it cannot, for measuring or to keep a stream SDR).
    var hdr = true
    var probe: Double?

    init(_ args: [String]) {
        let d = UserDefaults.standard
        if let v = d.string(forKey: "host"), !v.isEmpty { host = v }
        if d.integer(forKey: "port") > 0 { port = UInt16(clamping: d.integer(forKey: "port")) }
        if let c = d.object(forKey: "codec") as? Int { codec = UInt8(c) } else if d.bool(forKey: "hevc") { codec = 1 }
        if d.integer(forKey: "mbps") > 0 { mbps = UInt32(d.integer(forKey: "mbps")) }
        if d.double(forKey: "refresh") > 0 { refresh = d.double(forKey: "refresh") }
        size = Options.parseSize(d.string(forKey: "size") ?? "")
        vrrMode = VRRMode(rawValue: d.integer(forKey: "vrrMode")) ?? .auto
        if d.object(forKey: "showStats") != nil { showStats = d.bool(forKey: "showStats") }
        immediateFullScreen = d.bool(forKey: "immediateFullScreen")
        vividColors = d.bool(forKey: "vividColors")
        if d.object(forKey: "keepWiFiAwake") != nil { keepWiFiAwake = d.bool(forKey: "keepWiFiAwake") }
        if d.object(forKey: "keepGPUAwake") != nil { keepGPUAwake = d.bool(forKey: "keepGPUAwake") }
        if d.object(forKey: "streamFrames") != nil { streamFrames = d.bool(forKey: "streamFrames") }
        if d.object(forKey: "pyrowaveInter") != nil { pyrowaveInter = d.bool(forKey: "pyrowaveInter") }
        if d.object(forKey: "hdr") != nil { hdr = d.bool(forKey: "hdr") }
        if d.object(forKey: "phaseLock") != nil { phaseLock = d.bool(forKey: "phaseLock") }
        holdFrames = d.bool(forKey: "holdFrames")
        if d.object(forKey: "pacingMode") != nil {
            pacingMode = PacingMode(rawValue: d.integer(forKey: "pacingMode")) ?? .auto
        } else if d.object(forKey: "pacingAuto") != nil { // saved by an older version
            pacingMode = d.bool(forKey: "pacingAuto") ? .auto : .manual
        }
        if d.object(forKey: "touchscreen") != nil { touchscreen = d.bool(forKey: "touchscreen") }
        cursorMode = CursorMode(rawValue: d.integer(forKey: "cursorSetting")) ?? .auto
        mouseMode = MouseMode(rawValue: d.integer(forKey: "mouseMode")) ?? .auto
        confinement = Confinement(rawValue: d.integer(forKey: "cursorConfinement")) ?? .auto
        parallelism = d.integer(forKey: "parallelism")

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
            case "--no-hdr": hdr = false
            case "--stats-log": statsLog = true
            case "--hide-stats": showStats = false
            case "--fullscreen": fullScreen = true
            case "--screen": screen = it.next()
            case "--mock": mock = true
            case "--no-vsync": immediateFullScreen = true; pacingMode = .manual
            case "--vivid": vividColors = true
            case "--no-keep-awake": keepWiFiAwake = false
            case "--no-gpu-awake": keepGPUAwake = false
            case "--whole-frames": streamFrames = false
            case "--intra-only": pyrowaveInter = false
            case "--no-phase": phaseLock = false
            case "--hold": holdFrames = true; pacingMode = .manual
            case "--no-hold": holdFrames = false; pacingMode = .manual
            case "--auto-pacing": pacingMode = .auto
            case "--low-latency": pacingMode = .lowLatency
            case "--manual-pacing": pacingMode = .manual
            case "--vrr":
                let n = it.next()?.lowercased()
                if let m = VRRMode.allCases.first(where: { $0.name == n }) { vrrMode = m; pacingMode = .manual }
            case "--touch-mouse": touchscreen = false
            case "--force-client-cursor": cursorMode = .show
            case "--cursor":
                let n = it.next()?.lowercased()
                cursorMode = CursorMode.allCases.first { $0.name == n } ?? cursorMode
            case "--mouse":
                let n = it.next()?.lowercased()
                mouseMode = MouseMode.allCases.first { $0.name == n } ?? mouseMode
            case "--confine":
                let n = it.next()?.lowercased()
                confinement = Confinement.allCases.first { $0.name == n } ?? confinement
            case "--parallelism":
                let n = it.next()?.lowercased()
                parallelism = n == "auto" ? 0 : max(0, Int(n ?? "") ?? 0)
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
        d.set(keepGPUAwake, forKey: "keepGPUAwake")
        d.set(streamFrames, forKey: "streamFrames")
        d.set(pyrowaveInter, forKey: "pyrowaveInter")
        d.set(phaseLock, forKey: "phaseLock")
        d.set(holdFrames, forKey: "holdFrames")
        d.set(pacingMode.rawValue, forKey: "pacingMode")
        d.set(vrrMode.rawValue, forKey: "vrrMode")
        d.set(touchscreen, forKey: "touchscreen")
        d.set(cursorMode.rawValue, forKey: "cursorSetting")
        d.set(mouseMode.rawValue, forKey: "mouseMode")
        d.set(confinement.rawValue, forKey: "cursorConfinement")
        d.set(parallelism, forKey: "parallelism")
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
    let vsyncOff: Bool  // in full screen, where the platform can
    let vrrMode: VRRMode
    let steamVRR: UInt8 // protocol VrrPolicy: 0 Steam's own, 1 off, 2 on

    /// `vrr`: the display does VRR where the stream is (SessionShell.vrr).
    /// `capable`: it can, in a window too (SessionShell.vrrCapable).
    init(options: Options, vrr: Bool, capable: Bool) {
        self.vrr = vrr
        let mode: VRRMode
        var lock: Bool
        switch options.pacingMode {
        case .auto:
            (lock, hold, vsyncOff) = vrr ? (false, true, false) : (true, false, false)
            mode = .auto
        case .lowLatency:
            (lock, hold, vsyncOff) = (false, false, true)
            mode = .on
        case .manual:
            (lock, hold, vsyncOff) = (options.phaseLock, options.holdFrames, options.immediateFullScreen)
            mode = options.vrrMode
        }
        vrrMode = mode
        switch mode {
        case .auto: steamVRR = vrr ? 2 : 1
        case .off: steamVRR = 1
        // Free-running on the host, which is what Low latency is, whatever the display.
        case .on: steamVRR = capable || options.pacingMode == .lowLatency ? 2 : 1
        case .steam: steamVRR = capable ? 0 : 1
        }
        // The host never times itself: frames go by the game or by the client's
        // grid. With neither, gamescope steps on a 60 Hz timer of its own.
        if steamVRR != 2 { lock = true }
        phaseLock = lock
    }
}
