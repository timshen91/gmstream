// One stream from the host, shared by the macOS and iPadOS apps: the
// connection, decoders, renderer, pacing (phase lock, hold, panel rate),
// audio, game controllers, stats, and the input the app sends. The app
// around it (SessionShell) owns the window, its screen, the system cursor
// and the pointer lock.

import Foundation
import Metal
import QuartzCore

let codecMask: UInt8 = 0b011 | (Decoder.hasAV1 ? 1 << 2 : 0) // H.264, HEVC, AV1 (the probe also takes PyroWave, 1 << 3)

/// What a session needs from the app around it. Everything here is called
/// on the main thread.
protocol SessionShell: AnyObject {
    /// The stream view in device pixels, and device pixels per point.
    var pixelSize: CGSize { get }
    var scale: Double { get }
    /// The screen's top refresh rate (Hz) and its shortest refresh (s).
    var maxFPS: Double { get }
    var shortestRefresh: Double { get }
    /// The stream fills the screen (frames can be flipped straight to it).
    var fullScreen: Bool { get }
    /// The display refreshes when a frame comes, where the stream is (VRR).
    var vrr: Bool { get }
    var vrrCapable: Bool { get }
    /// The display's own color space ("Vivid colors").
    var displayColorSpace: CGColorSpace? { get }
    /// The stream has the keyboard and the pointer.
    var focused: Bool { get }
    /// The status in one line (macOS: the title bar).
    func status(_ line: String)
    /// The cursor to draw may have changed: the host's cursor, the pointer
    /// lock, the stream's size or the pacing did.
    func cursorChanged()
    /// Whether the mouse is captured all through a stream, not only for
    /// relative motion.
    var capturesMouse: Bool { get }
    /// Captures the pointer, or frees it. The shell sets
    /// `lockActive` when it has (iPadOS decides later, or not at all).
    func setPointerLock(_ on: Bool)
    /// The pacing in effect changed (the settings show it).
    func pacingChanged()
}

final class Session: ConnectionDelegate {
    weak var shell: SessionShell?
    var conn: Connection?
    let decoder = Decoder()
    let renderer = MetalRenderer(layerKeys: options.layerKeys, drawableKeys: options.drawableKeys)!
    let stages = StageStats()
    var netFirst: (id: UInt32, first: UInt64, last: UInt64, maxGap: UInt64, gapAt: Int, readLag: UInt64)? // measuring (GM_DEBUG_NET)
    lazy var pyro = PyroWaveDecoder(device: renderer.device, queue: renderer.queue)
    /// H.264, HEVC, AV1 in hardware, and PyroWave where its Metal decoder runs.
    var clientCodecs: UInt8 { codecMask | (pyro != nil ? 1 << 3 : 0) }
    let chart = LatencyChart()
    let panel = PanelRate()
    // The host's frame-rate cap when the panel's real top rate is below its
    // nominal one (PanelRate): its shortest refresh plus 0.2 ms, so every
    // frame comes after the panel could refresh again (PanelRate).
    var capHz: Double?
    let phase = PhaseLock(refreshHz: 60) // restarted at the screen's rate in start()
    let stats = Stats()
    let gate = FrameGate()
    var held = Set<UInt16>()
    let pads = Gamepads()
    let opus = OpusDecoder()
    var player: AudioPlayer?
    var gameRelative = false // the game wants motion (Mouse Auto follows it)
    var lockActive = false // the pointer is captured (set by the shell)
    var gameClip: CGRect?  // where the game keeps the cursor, in stream pixels, while it has the mouse
    var fullScreen = false // as the present mode was last applied
    var immediate = false  // vsync off, as last applied
    var pacing = Pacing(options: options, vrr: false, capable: false)
    var codecName = "-"
    var timers: [Timer] = []
    var closedReason: String?
    var mock: MockSource?
    var connectAttempt = 0
    /// The stream's size in pixels (until the first frame, what was asked for).
    var streamSize = CGSize(width: 16, height: 9) {
        didSet { if streamSize != oldValue { shell?.cursorChanged() } }
    }
    /// The host's cursor (nil: it shows none).
    var hostCursor: HostCursor?
    /// The cursor to draw (nil: none).
    var clientCursor: HostCursor? {
        switch options.cursorMode {
        case .auto: return hostCursor
        case .show: return hostCursor ?? .noImage
        case .hide: return nil
        }
    }

    /// Stats: "hardware" while gamescope has a cursor image. "remote" otherwise:
    /// whatever cursor there is, if any, is pixels in the video.
    var cursorSource: String { hostCursor != nil ? "hardware" : "remote" }

    /// Whether the cursor keeps to the game's clip right now.
    var confined: Bool { options.confinement == .auto && gameClip != nil }
    /// Motion instead of positions: the setting, or Auto's answer to the game.
    var sendsMotion: Bool { options.mouseMode == .auto ? gameRelative : options.mouseMode == .relative }

    /// While there is a stream the mouse is ours: the system cursor stays still
    /// and hidden in the view and the app adds up the motion, so it can't reach
    /// a screen's edge (no Dock, menu bar, hot corner or other display). That
    /// is the shell's way (capturesMouse), or relative alone where it can't.
    var wantsCapture: Bool {
        (shell?.capturesMouse == true || sendsMotion) && (conn != nil || options.mock)
    }

    /// The area the cursor stays in, in a view of `size`, y down as in the stream.
    func cursorArea(in size: CGSize) -> CGRect {
        let v = videoRect(in: size)
        guard confined, let clip = gameClip else { return v }
        let scale = v.width / streamSize.width
        return CGRect(x: v.minX + clip.minX * scale, y: v.minY + clip.minY * scale,
                      width: clip.width * scale, height: clip.height * scale)
    }

    /// Captures the pointer, or frees it, as the settings and focus now want.
    func updateCapture() {
        shell?.setPointerLock(wantsCapture && shell?.focused == true)
        shell?.cursorChanged()
    }
    /// Stopped while the app is in the background (no GPU work allowed there).
    private(set) var running = false
    let probe = MouseProbe()

    init(shell: SessionShell) {
        self.shell = shell
        decoder.yuv = !options.bgra
        decoder.onDecoded = { [renderer, probe, decoder] image, height, timing in
            probe?.decoded(image, timing)
            // The stream says whether it is HDR10; the renderer switches its
            // layer on its own thread.
            renderer.wantsHDR = decoder.hdr
            renderer.submit(.buffer(image, height: height), timing)
        }
        pyro?.onDecoded = { [renderer] planes, timing in renderer.submit(.planes(planes), timing) }
        pyro?.onIncomplete = { [stats] in stats.gap() }
        renderer.onPresented = { [stages, chart, phase, panel] t in
            stages.add(t)
            phase.presented(t)
            panel.presented(t.presented)
            Trace.shared?.record(t: t.presented, aux: t.presented &- t.received, kind: 0xFE, host: t.presentLocal ?? 0)
            if FrameTiming.debugFlip, let g = phase.grid() {
                // Measuring: where in the refresh each frame was flipped.
                let phase = { (ns: UInt64) -> Double in
                    let ph = Double(Int64(ns) - Int64(g.vblank)).truncatingRemainder(dividingBy: g.interval)
                    return (ph < 0 ? ph + g.interval : ph) / 1e6
                }
                FileHandle.standardError.write(String(format: "flip %.3f %.3f made %.3f submitted %.3f drawn %.3f\n", phase(t.presented),
                    Double(Int64(t.presented) - Int64(t.submitted)) / 1e6, phase(t.presentLocal ?? 0), phase(t.submitted),
                    Double(Int64(t.drawn) - Int64(t.submitted)) / 1e6).data(using: .utf8)!)
            }
            if let p = t.presentLocal {
                chart.push(base: Double(Int64(t.submitted) - Int64(p)) / 1e6, ms: Double(Int64(t.presented) - Int64(p)) / 1e6)
            }
        }
        _ = Trace.shared // its flush timer on the main run loop
        renderer.onSkipped = { [stages] in stages.skip() }
        renderer.onDropped = { [stages, phase] t in stages.drop(); phase.dropped(t) }
        pads.send = { [weak self] type, data in self?.conn?.send(type, data) }
        do { player = try AudioPlayer() } catch { NSLog("gmstream: audio output: \(error)") }
    }

    /// Connects, and keeps the stream going (reconnecting after a drop)
    /// until `stop`.
    func start() {
        guard !running else { return }
        running = true
        if let shell { phase.restart(refreshHz: shell.maxFPS) }
        applyPresentMode()
        timers.append(Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.conn?.ping() })
        timers.append(Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.sendGrid() })
        timers.append(Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            self?.updateStats()
            self?.checkPanelRate()
        })
        timers.append(Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            self?.updateOverlay()
        })
        connect()
    }

    func stop() {
        guard running else { return }
        running = false
        timers.forEach { $0.invalidate() }
        timers = []
        releaseAll()
        shell?.setPointerLock(false)
        connectAttempt += 1
        conn?.stop()
        conn = nil
        mock?.stop()
        mock = nil
    }

    /// This display's refresh grid, shifted by the phase lock, to the host.
    /// With pacing off (the setting alone decides, on a variable-refresh
    /// display too), none: an empty grid, so gamescope paces itself and each
    /// frame is shown as it arrives.
    func sendGrid() {
        if !immediate && MetalRenderer.canTurnVsyncOff && pacing.vsyncOff && fullScreen && phase.grid() != nil {
            applyPresentMode()
        }
        if let mock {
            let g = !pacing.phaseLock ? nil : phase.step()
            mock.setGrid(anchor: g?.anchor ?? 0, interval: g?.interval ?? 0)
            return
        }
        guard let conn else { return }
        var m = Writer()
        if !pacing.phaseLock || getenv("GM_NO_PHASE") != nil { // gamescope paces itself
            m.u64(0)
            m.u32(0)
        } else {
            guard let g = phase.step(), let anchor = conn.toHost(localNs: g.anchor) else { return }
            if phase.takeDrain() { renderer.skipOne() }
            m.u64(anchor)
            m.u32(UInt32(g.interval))
        }
        conn.send(Msg.displayGrid, m.data)
    }

    // MARK: Connection

    func wantedSize() -> (Int, Int) {
        if let s = options.size { return s }
        let p = shell?.pixelSize ?? CGSize(width: 1920, height: 1080)
        return (Int(p.width) & ~1, Int(p.height) & ~1)
    }
    func refreshMhz() -> UInt32 {
        UInt32(((options.refresh ?? capHz) ?? shell?.maxFPS ?? 60) * 1000)
    }

    /// Once a second: cap the host just under the panel's real top rate when
    /// it is below the nominal one (only measurable before the cap is set).
    func checkPanelRate() {
        guard capHz == nil, !pacing.phaseLock, options.refresh == nil, let shell,
              let top = panel.top(nominalHz: shell.maxFPS) else { return }
        capHz = 1e9 / (1e9 / top + 200_000)
        // Shorter than the host's spacing on average, or one frame pushed
        // back would push every later one too (it drains 0.1 ms a frame).
        renderer.setHoldSpacing(1e9 / top + 100_000)
        NSLog("gmstream: panel tops out at %.3f Hz; capping the host at %.3f Hz", top, capHz!)
        sendConfigure()
    }

    func connect() {
        closedReason = nil
        if options.mock {
            let (w, h) = options.size ?? (1920, 1080)
            let m = MockSource(device: renderer.device, width: w, height: h, refreshHz: options.refresh ?? 60)
            m.start { [renderer] image, timing in renderer.submit(image, timing) }
            mock = m
            DispatchQueue.main.async {
                self.streamSize = CGSize(width: w, height: h)
                // As if the host showed a cursor with no usable image: the system's arrow.
                self.hostCursor = .noImage
                self.shell?.cursorChanged()
            }
            return
        }
        // Resolving the host can take seconds: not on the main thread. A
        // newer attempt (Reconnect, a settings change) supersedes this one.
        connectAttempt += 1
        let attempt = connectAttempt, host = options.host, port = options.port
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let result = Result { try Connection(host: host, port: port) }
            DispatchQueue.main.async { [weak self] in
                guard let self, attempt == self.connectAttempt else { return }
                switch result {
                case .success(let c): self.start(c)
                case .failure(let e): self.closed(reason: e.localizedDescription)
                }
            }
        }
    }

    private func start(_ c: Connection) {
        c.delegate = self
        // The screen list, on the main thread and late enough for AppKit to
        // have one: the Hello that carries it goes out just below.
        primeHDRCapable()
        hostSize = (0, 0)
        conn = c
        let (w, h) = wantedSize()
        // Until the first frame says otherwise (the cursor's scale needs it).
        streamSize = CGSize(width: w, height: h)
        c.start(width: w, height: h, refreshMhz: refreshMhz(), kbps: options.mbps * 1000,
                codecMask: clientCodecs, preferred: options.codec)
        c.keepAwake(options.keepWiFiAwake)
        c.send(Msg.vrrPolicy, Data([pacing.steamVRR]))
    }

    func reconnect() {
        guard running else { return }
        conn?.stop()
        conn = nil
        mock?.stop()
        mock = nil
        connect()
    }

    func received(frame f: Frame) {
        guard let conn, gate.admit(f, requestKeyframe: { conn.send(Msg.keyframe) }, stats: stats) else { return }
        let timing = FrameTiming(frame: f) { conn.toLocal(hostNs: $0) }
        if !decoder.decode(f, timing: timing) {
            conn.send(Msg.keyframe)
            return
        }
        stats.add(bytes: f.bitstream.count, netMs: nil, totalMs: nil)
        sizeFromHost(width: f.width, height: f.height)
    }

    /// On the connection's thread: the stream's size as frames carry it.
    private func sizeFromHost(width: Int, height: Int) {
        guard width != hostSize.0 || height != hostSize.1 else { return }
        hostSize = (width, height)
        DispatchQueue.main.async { self.streamSize = CGSize(width: width, height: height) }
    }
    private var hostSize = (0, 0) // the connection thread's

    func pyrowave(_ p: PyroWavePacket) {
        guard let conn, let pyro else { return }
        if getenv("GM_DEBUG_NET") != nil, let enc = conn.toLocal(hostNs: p.encodedNs) {
            // Measuring: per frame, when its first packet arrived (after the
            // host finished encoding), how long the rest took, and the
            // largest gap between two of its packets.
            if p.index == 0 { netFirst = (p.frameId, p.receivedNs, p.receivedNs, 0, 0, 0) }
            if var n = netFirst, n.id == p.frameId {
                // How long after arriving the app read a packet (app stalls).
                n.readLag = max(n.readLag, p.readNs &- p.receivedNs)
                if p.index > 0 {
                    let gap = p.receivedNs &- n.last
                    if gap > n.maxGap { n.maxGap = gap; n.gapAt = Int(p.index) }
                    n.last = p.receivedNs
                }
                netFirst = n
                if p.index == p.count - 1 {
                    FileHandle.standardError.write(String(format: "net t %.3f first %.2f burst %.2f maxgap %.2f at %d/%d readlag %.2f\n",
                        Double(n.first) / 1e6, Double(Int64(n.first) - Int64(enc)) / 1e6, Double(p.receivedNs - n.first) / 1e6,
                        Double(n.maxGap) / 1e6, n.gapAt, p.count, Double(n.readLag) / 1e6).data(using: .utf8)!)
                }
            }
        }
        stats.addBytes(p.payload.count)
        pyro.packet(p) {
            FrameTiming(slotNs: p.slotNs, encodedNs: p.encodedNs, received: nowNs()) { conn.toLocal(hostNs: $0) }
        }
        sizeFromHost(width: Int(p.width), height: Int(p.height))
    }

    func audio(_ packet: Data) {
        guard let player, let samples = opus?.decode(packet), !samples.isEmpty else { return }
        player.push(samples)
    }

    func helloAck(codec: UInt8) {
        updateCapture()
        if let seq = getenv("GM_TEST_KEYS").map({ String(cString: $0) }) {
            // Measuring: after 4 s, sends evdev key events "29+,42+,15+,15-,42-,29-".
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
                for ev in seq.split(separator: ",") {
                    guard let k = UInt16(ev.dropLast()) else { continue }
                    self.sendKey(k, ev.hasSuffix("+"))
                    usleep(30_000)
                }
            }
        }
        probe?.start { [weak self] dx in self?.motion(dx: dx, dy: 0) }
        codecName = ["H.264", "HEVC", "AV1", "PyroWave"][Int(Swift.min(codec, 3))]
        pads.resend()
    }

    func rumble(index: Int, strong: UInt16, weak: UInt16, ms: Int) {
        pads.rumble(index: index, strong: strong, weak: weak, ms: ms)
    }

    /// What the host's encoder takes in (its own words), for the stats.
    func encodeFormat(_ text: String) {
        hostFormat = text
    }

    func closed(reason: String) {
        guard closedReason == nil else { return }
        closedReason = reason
        NSLog("gmstream: closed: %@", reason)
        releaseAll()
        shell?.setPointerLock(false)
        conn?.stop()
        conn = nil
        updateStats()
        if reason != "Another client connected" {
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
                if let self, self.running, self.conn == nil { self.connect() }
            }
        }
    }

    // MARK: Stats

    /// Stats once a second: a one-line status (macOS: the title bar) and,
    /// unless hidden, an overlay drawn into the video.
    func updateStats() {
        if let r = closedReason {
            shell?.status("\(options.host) — \(r)")
            hudText = r
            updateOverlay()
            return
        }
        let s = stats.take()
        let st = stages.take()
        let now = ProcessInfo.processInfo.systemUptime
        let cursorFPS = Double(renderer.takeCursorUpdates()) / max(now - cursorFPSSince, 0.001)
        cursorFPSSince = now
        let pad = { (s: String, n: Int) in s.padding(toLength: n, withPad: " ", startingAt: 0) }
        let num = { (v: Double?) in pad(fmt(v), 6) }
        let total = st.rows.first { $0.name == "total" }!
        var lines = [
            "\(Int(streamSize.width))x\(Int(streamSize.height)) \(codecName) \(st.shown) fps shown " +
                "\(String(format: "%.0f", s.mbps)) Mbps",
            "\(pad("ms, 5 s", 10))\(pad("p50", 7))\(pad("p99", 7))max",
        ]
        for row in st.rows {
            if ["to \(systemName)", "total", "spacing"].contains(row.name) { lines.append(String(repeating: "─", count: 28)) }
            lines.append("\(pad(row.name, 10))\(num(row.p50)) \(num(row.p99)) \(fmt(row.max))")
        }
        let mode = switch options.pacingMode {
        case .auto: "pacing auto (\(pacing.vrr ? "VRR: hold" : "fixed refresh: phase lock")), "
        case .lowLatency: "pacing low latency, "
        case .manual: ""
        }
        let phaseText = mode + (!pacing.phaseLock ? "phase: off" : phase.summary())
        lines.append(phaseText + "  vsync \(immediate ? "off" : "on")  VRR \(["Steam", "off", "on"][Int(pacing.steamVRR)])  compositing \(renderer.compositingStatus)")
        lines.append("cursor \(cursorSource), \(cursorMode) \(String(format: "%.0f", cursorFPS)) fps  game mouse \(gameRelative ? "relative" : "absolute")  clip \(gameClip != nil ? "yes" : "no")  mouse \(sendsMotion ? "relative" : "absolute")  confined \(confined ? "yes" : "no")")
        // What goes into each encoder and each decoder, since a mismatch at
        // either end costs a conversion pass nobody asked for.
        if !hostFormat.isEmpty {
            lines.append("host in  " + hostFormat)
        }
        lines.append("here in  " + decoder.inputSummary)
        let hold = renderer.holdStatus(offset: conn?.clockOffsetNs)
        lines += [
            "rtt \(fmt(conn.map { Double($0.rttNs) / 1e6 })) ms  skipped \(st.skipped)  dropped \(st.dropped)  gaps \(s.gaps)" +
                (pacing.hold ? "  hold \(fmt(hold.delayNs.map { $0 / 1e6 })) ms, late \(hold.late)" : ""),
            "audio \(player.map { String(format: "%.0f ms buffered", $0.bufferedMs) } ?? (opus == nil ? "no Opus decoder" : "no output"))" +
                "  pads \(pads.connected)",
        ]
        shell?.status("\(lines[0]) · present→screen p99 \(fmt(total.p99)) ms (p50 \(fmt(total.p50)))")
        hudText = lines.joined(separator: "\n")
        if options.statsLog {
            print(lines.joined(separator: " | ").replacingOccurrences(of: "─", with: ""))
            fflush(stdout)
        }
        updateOverlay()
    }

    /// What the host's encoder takes in, as it described it.
    var hostFormat = ""
    /// The stats overlay's text (updated once a second).
    var hudText = ""
    /// Who draws the cursor, for the stats (set by the shell): "host",
    /// "frame" (into ours) or "macOS".
    var cursorMode = "host"
    private var cursorFPSSince = ProcessInfo.processInfo.systemUptime

    /// Redraws the stats overlay into the video (Latency.swift, renderOverlay).
    func updateOverlay() {
        guard running else { return }
        // Shown when disconnected, too: why.
        guard options.showStats || closedReason != nil, !hudText.isEmpty else { return renderer.setOverlay(nil, inset: 0) }
        let scale = shell?.scale ?? 2
        renderer.setOverlay(renderOverlay(text: hudText, chart: chart, scale: scale), inset: 8 * scale)
    }

    // MARK: Settings and display

    /// What pacing is in effect, for the settings: phase lock, hold, vsync off,
    /// and a line on what the mode chose and did to Steam's VRR toggle.
    func pacingNow() -> (phaseLock: Bool, hold: Bool, vsyncOff: Bool, vrr: VRRMode, note: String) {
        let note = switch options.pacingMode {
        case .manual: switch pacing.steamVRR {
            case 0: "Steam's VRR toggle is left to you."
            case 2: "VRR is on, whatever Steam's toggle shows."
            default: "VRR is off."
            }
        case .lowLatency: "Frames show as they come: no phase lock, no fixed delay"
            + (MetalRenderer.canTurnVsyncOff ? ", vsync off in full screen (may tear)" : "")
            + "; the game runs free on the host, whatever Steam's toggle shows."
        case .auto: pacing.vrr ? "Variable-refresh display in full screen: fixed delay, no phase lock; Steam's VRR is forced on (the game runs free)."
            : "Fixed refresh (or a window): phase lock, no fixed delay; Steam's VRR is off (its toggle greyed)."
        }
        return (pacing.phaseLock, pacing.hold, pacing.vsyncOff, pacing.vrrMode, note)
    }

    /// `options` changed (and was saved) in the settings.
    func apply(_ change: SettingsChange) {
        switch change {
        case .connection: reconnect()
        case .bitrate:
            var m = Writer()
            m.u32(options.mbps * 1000)
            conn?.send(Msg.bitrate, m.data)
        case .display: sendConfigure()
        case .stats: updateOverlay()
        case .present: applyPresentMode()
        case .keepAwake: conn?.keepAwake(options.keepWiFiAwake)
        case .cursor: updateCapture()
        case .phaseLock: applyPresentMode() // pacing settings: phase lock, hold, auto
        }
    }

    /// The stream's size or refresh to the host (after the view or the
    /// settings changed).
    func sendConfigure() {
        guard let conn else { return }
        let (w, h) = wantedSize()
        var m = Writer()
        m.u16(UInt16(w)); m.u16(UInt16(h)); m.u32(refreshMhz())
        conn.send(Msg.configure, m.data)
    }

    /// Another screen, or this one woken or reconfigured: its refresh grid
    /// and top rate are measured again (vsync stays on until then).
    func screenChanged() {
        if let shell { phase.restart(refreshHz: shell.maxFPS) }
        capHz = nil
        renderer.setHoldSpacing(nil)
        panel.reset()
        applyPresentMode()
    }

    /// Vsync on, except in full screen with "lowest latency" set (macOS
    /// only). The settings alone decide, on a variable-refresh
    /// (Adaptive-Sync) display too (there, with vsync on and pacing off, the
    /// panel refreshes when a frame arrives: measured display stage 3.3 ms).
    /// With the phase lock, vsync stays on until the panel's refresh grid is
    /// measured (a second of frames; then sendGrid comes back here), since
    /// the lock steers vsync-off flips on it. Entering or leaving full screen
    /// changes how frames reach the panel (flipped straight to it or
    /// composited), so the phase lock starts over.
    func applyPresentMode() {
        guard let shell else { return }
        let wasFullScreen = fullScreen, was = pacing
        fullScreen = shell.fullScreen
        pacing = Pacing(options: options, vrr: shell.vrr, capable: shell.vrrCapable)
        immediate = MetalRenderer.canTurnVsyncOff && pacing.vsyncOff && fullScreen
            && (!pacing.phaseLock || phase.grid() != nil)
        renderer.setVsync(!immediate)
        renderer.holdFrames = pacing.hold
        phase.setImmediate(immediate)
        if (fullScreen != wasFullScreen || pacing.phaseLock != was.phaseLock) && !immediate { phase.restart() }
        if pacing.steamVRR != was.steamVRR { conn?.send(Msg.vrrPolicy, Data([pacing.steamVRR])) }
        // Tagged sRGB, the system converts to the display; tagged as the
        // display's own color space, the pixels go to the panel unconverted
        // (on the iPad too: an 8-bit layer tagged Display P3 shows P3's
        // primaries, checked by eye with GM_MOCK_COLORS=gamut; iPadOS's
        // screenshots clip to sRGB, so they cannot show it).
        renderer.layer.colorspace = options.vividColors ? shell.displayColorSpace : CGColorSpace(name: CGColorSpace.sRGB)
        shell.cursorChanged()
        shell.pacingChanged()
    }

    // MARK: Input
    //
    // The apps turn their events into these (keys as Linux evdev codes,
    // positions in the video, motion in stream pixels).

    /// Where the video is drawn in a view of `size` (any unit), letterboxed.
    func videoRect(in size: CGSize) -> CGRect {
        let s = min(size.width / streamSize.width, size.height / streamSize.height)
        let w = streamSize.width * s, h = streamSize.height * s
        return CGRect(x: (size.width - w) / 2, y: (size.height - h) / 2, width: w, height: h)
    }

    /// A key went down or up. gamescope repeats keys itself.
    func key(_ k: UInt16, down: Bool) {
        if down ? held.insert(k).inserted : held.remove(k) != nil { sendKey(k, down) }
    }
    func sendKey(_ k: UInt16, _ down: Bool) {
        var m = Writer()
        m.u16(k); m.u8(down ? 1 : 0)
        conn?.send(Msg.key, m.data)
    }
    /// Every key held goes up (focus lost, disconnected).
    func releaseAll() {
        for k in held { sendKey(k, false) }
        held.removeAll()
    }

    private var lastPosition: (x: Float, y: Float)?

    /// The pointer at (x, y) in the video, 0...1 from its top left
    /// (clamped). With the cursor drawn into frames it needs drawing anew.
    func position(x: Double, y: Double) {
        renderer.cursorMoved()
        let at = (x: Float(min(1, max(0, x))), y: Float(min(1, max(0, y))))
        lastPosition = at
        conn?.position(x: at.x, y: at.y)
    }

    /// Relative motion in stream pixels (pointer locked).
    func motion(dx: Double, dy: Double) {
        conn?.motion(dx: dx, dy: dy)
    }

    /// A mouse button (evdev BTN_*). A click while the host wants the
    /// pointer but it is not captured (focus came back) captures it instead.
    func button(_ code: UInt16, down: Bool) {
        if down && wantsCapture && !lockActive { shell?.setPointerLock(true); return }
        // Buttons are ordered and the mouse isn't: click where the mouse is.
        if !sendsMotion, let at = lastPosition {
            var p = Writer()
            p.f32(at.x); p.f32(at.y)
            conn?.send(Msg.position, p.data)
        }
        var m = Writer()
        m.u16(code); m.u8(down ? 1 : 0)
        conn?.send(Msg.button, m.data)
    }
    static let leftButton: UInt16 = 0x110, rightButton: UInt16 = 0x111, middleButton: UInt16 = 0x112

    /// A touchscreen: touch `id` down, moved or up at (x, y) in the video,
    /// 0...1 from its top left; cancel ends every touch.
    enum TouchPhase: UInt8 { case down = 0, moved = 1, up = 2, cancel = 3 }
    func touch(_ id: Int, _ phase: TouchPhase, x: Double = 0, y: Double = 0) {
        var m = Writer()
        m.u8(UInt8(id)); m.u8(phase.rawValue)
        m.f32(Float(min(1, max(0, x)))); m.f32(Float(min(1, max(0, y))))
        conn?.send(Msg.touch, m.data)
    }

    /// Scrolling, in browser wheel pixels (100 per notch, positive: down or
    /// right), sent as the web client does: Wayland axis units 15 per notch,
    /// v120 120 per notch.
    func scroll(dx: Double, dy: Double) {
        var m = Writer()
        m.f32(Float(dx * 0.15)); m.f32(Float(dy * 0.15))
        m.i16(Int16(clamping: Int((dx * 1.2).rounded()))); m.i16(Int16(clamping: Int((dy * 1.2).rounded())))
        conn?.send(Msg.scroll, m.data)
    }

    // MARK: The cursor
    //
    // The cursor is the client's: its position is added up here and it is
    // drawn after decoding, at the display's rate. gamescope says what it looks
    // like (Cursor), whether the game wants motion (Relative) and where the game
    // keeps it (CursorClip), each on its own; none carries its position, and no
    // cursor can mean hidden or a game drawing its own, which to us are the same.

    func cursor(width: Int, height: Int, hotX: Int, hotY: Int, rgba: Data) {
        hostCursor = width > 0 && height > 0 ? HostCursor(width: width, height: height, hotX: hotX, hotY: hotY, rgba: rgba) : nil
        shell?.cursorChanged()
    }

    /// The focused window's size: its shape is the game's in the stream, centered.
    func cursorClip(_ clip: CGRect?) {
        gameClip = clip
        shell?.cursorChanged()
    }

    /// The game wants motion: Mouse on Auto follows it, a manual setting doesn't.
    func relative(_ on: Bool) {
        gameRelative = on
        updateCapture()
    }
}

/// The host's cursor image: RGBA, not premultiplied, top row first.
struct HostCursor {
    let width: Int, height: Int, hotX: Int, hotY: Int
    let rgba: Data

    /// A cursor to show with no usable image: the app's arrow.
    static let noImage = HostCursor(width: 1, height: 1, hotX: 0, hotY: 0, rgba: Data())

    /// nil when the host shows a cursor but sent no usable image.
    var image: CGImage? {
        guard rgba.count >= width * height * 4, let provider = CGDataProvider(data: rgba as CFData) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }
}

/// What a settings change touches (the apps' settings UIs).
enum SettingsChange { case connection, bitrate, display, stats, present, keepAwake, phaseLock, cursor }

func fmt(_ v: Double?) -> String { v.map { String(format: "%.1f", $0) } ?? "-" }
