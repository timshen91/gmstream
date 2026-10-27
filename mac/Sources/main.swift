// gmstream native macOS client (reference): plain UDP to the host,
// VideoToolbox hardware decode, a Metal layer presented on the next refresh
// (per-stage latency down to the moment a frame is on screen), keyboard,
// mouse and game controllers (Bluetooth or USB) forwarded, gamescope's
// cursor as the native cursor, audio through Apple's Opus decoder.
//
// Stats show in the title bar and an overlay (Cmd+I hides it).
//
// Settings (Cmd+,) are saved; command-line options override them for one
// launch.
//
//   GMStream [HOST] [--port 8444] [--hevc | --pyrowave] [--bitrate MBPS] [--refresh HZ] [--size WxH]
//   GMStream [HOST] --probe SECONDS     (no window: receive and print stats)
//   Measuring: --stats-log (print the overlay every second), --hide-stats,
//   --fullscreen|3,
//   --inflight N (frames submitted but not on screen), --vsync,
//   --display-link FRAMES (draw from CAMetalDisplayLink, preferredFrameLatency),
//   --hide-stats, --fullscreen

import AppKit


/// Settings: saved ones (Settings window, Cmd+,), then command-line options
/// on top for this launch.
struct Options {
    var host = "steamtab.local"
    var port: UInt16 = 8444
    var codec: UInt8 = 0 // preferred: 0 H.264, 1 HEVC, 3 PyroWave
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

        var it = args.makeIterator()
        while let a = it.next() {
            switch a {
            case "--port": port = UInt16(it.next() ?? "") ?? port
            case "--hevc": codec = 1
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
    }
}

var options = Options(Array(CommandLine.arguments.dropFirst()))
let codecMask: UInt8 = 0b011 // H.264, HEVC (the probe also takes PyroWave, 1 << 3)

/// Per-second statistics, filled on the network thread.
final class Stats {
    private let lock = NSLock()
    private var frames = 0, bytes = 0, gaps = 0
    private var net: [Double] = [], total: [Double] = []

    func add(bytes n: Int, netMs: Double?, totalMs: Double?) {
        lock.lock(); defer { lock.unlock() }
        frames += 1; bytes += n
        if let netMs { net.append(netMs) }
        if let totalMs { total.append(totalMs) }
    }
    func gap() { lock.lock(); gaps += 1; lock.unlock() }
    func addBytes(_ n: Int) { lock.lock(); bytes += n; lock.unlock() }

    /// Returns a summary and resets.
    func take() -> (frames: Int, mbps: Double, gaps: Int, net: Double?, total: Double?, totalMax: Double?) {
        lock.lock(); defer { lock.unlock() }
        let avg: ([Double]) -> Double? = { $0.isEmpty ? nil : $0.reduce(0, +) / Double($0.count) }
        let r = (frames, Double(bytes) * 8 / 1e6, gaps, avg(net), avg(total), total.max())
        frames = 0; bytes = 0; net = []; total = []
        return r
    }
}

/// Decides which frames to show: in order, from a keyframe.
final class FrameGate {
    private var expect: UInt32?
    private var lastKeyRequest: UInt64 = 0

    /// Whether to decode this frame; asks for a keyframe after a gap.
    func admit(_ f: Frame, requestKeyframe: () -> Void, stats: Stats) -> Bool {
        if let e = expect, f.id == e {
            expect = e &+ 1
            return true
        }
        if f.key {
            expect = f.id &+ 1
            return true
        }
        if let e = expect, Int32(bitPattern: f.id &- e) < 0 { return false } // stale
        if expect != nil { stats.gap() }
        expect = nil
        let now = nowNs()
        if now - lastKeyRequest > 200_000_000 {
            lastKeyRequest = now
            requestKeyframe()
        }
        return false
    }
}

// MARK: - Probe mode

final class Probe: ConnectionDelegate {
    let conn: Connection
    let stats = Stats()
    let gate = FrameGate()
    var ack = false
    let opus = OpusDecoder()
    var audioPackets = 0, audioSamples = 0

    func audio(_ packet: Data) {
        audioPackets += 1
        audioSamples += opus?.decode(packet).count ?? 0
    }

    init(conn: Connection) { self.conn = conn }

    func received(frame f: Frame) {
        guard gate.admit(f, requestKeyframe: { conn.send(Msg.keyframe) }, stats: stats) else { return }
        let net = conn.toLocal(hostNs: f.encodedNs).map { Double(Int64(f.receivedNs) - Int64($0)) / 1e6 }
        let total = conn.toLocal(hostNs: f.slotNs).map { Double(Int64(nowNs()) - Int64($0)) / 1e6 }
        stats.add(bytes: f.bitstream.count, netMs: net, totalMs: total)
    }
    func pyrowave(_ p: PyroWavePacket) {
        if p.index == p.count - 1 { stats.add(bytes: p.payload.count, netMs: nil, totalMs: nil) } else { stats.addBytes(p.payload.count) }
    }
    func helloAck(codec: UInt8) { ack = true; print("connected, codec \(codec)") }
    func cursor(width: Int, height: Int, hotX: Int, hotY: Int, rgba: Data) {}
    func pointerLock(_ locked: Bool) {}
    func rumble(index: Int, strong: UInt16, weak: UInt16, ms: Int) {}
    func closed(reason: String) { print("closed: \(reason)"); exit(1) }
}

func fmt(_ v: Double?) -> String { v.map { String(format: "%.1f", $0) } ?? "-" }

if let seconds = options.probe {
    do {
        let conn = try Connection(host: options.host, port: options.port)
        let probe = Probe(conn: conn)
        conn.delegate = probe
        let (w, h) = options.size ?? (1920, 1080)
        conn.start(width: w, height: h, refreshMhz: UInt32((options.refresh ?? 60) * 1000), kbps: options.mbps * 1000,
                   codecMask: codecMask | 1 << 3, preferred: options.codec)
        let pinger = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in conn.ping() }
        let end = Date().addingTimeInterval(seconds)
        while Date() < end {
            RunLoop.main.run(until: min(end, Date().addingTimeInterval(1)))
            let s = probe.stats.take()
            print("\(s.frames) fps  \(String(format: "%.1f", s.mbps)) Mbps  gaps \(s.gaps)  " +
                  "encoded→received \(fmt(s.net)) ms  present→received \(fmt(s.total)) ms (max \(fmt(s.totalMax)))  " +
                  "rtt \(fmt(Double(conn.rttNs) / 1e6)) ms  audio \(probe.audioPackets) packets \(probe.audioSamples / 2) frames decoded")
            probe.audioPackets = 0; probe.audioSamples = 0
        }
        pinger.invalidate()
        conn.stop()
        exit(probe.ack ? 0 : 1)
    } catch {
        print("error: \(error.localizedDescription)")
        exit(1)
    }
}

// MARK: - Window

final class StreamView: NSView {
    let metalLayer: CAMetalLayer
    var streamSize = CGSize(width: 16, height: 9) {
        didSet { if streamSize != oldValue { controller?.rescaleCursor() } }
    }
    var cursor = NSCursor.arrow
    weak var controller: Controller?

    init(frame: NSRect, layer: CAMetalLayer) {
        metalLayer = layer
        super.init(frame: frame)
        wantsLayer = true
    }
    required init?(coder: NSCoder) { fatalError() }

    override func makeBackingLayer() -> CALayer { metalLayer }

    /// The drawable matches the view in device pixels.
    private func updateDrawableSize() {
        let scale = window?.backingScaleFactor ?? 2
        metalLayer.contentsScale = scale
        metalLayer.drawableSize = CGSize(width: max(1, bounds.width * scale), height: max(1, bounds.height * scale))
    }
    override func setFrameSize(_ size: NSSize) { super.setFrameSize(size); updateDrawableSize() }
    override func viewDidChangeBackingProperties() { super.viewDidChangeBackingProperties(); updateDrawableSize() }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    /// Where the video is drawn, in view points.
    var videoRect: CGRect {
        let s = min(bounds.width / streamSize.width, bounds.height / streamSize.height)
        let w = streamSize.width * s, h = streamSize.height * s
        return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect, .cursorUpdate],
                                       owner: self, userInfo: nil))
    }
    override func resetCursorRects() { addCursorRect(bounds, cursor: cursor) }
    override func cursorUpdate(with event: NSEvent) { cursor.set() }

    // Keys: Controller.captureKeyboard takes them before they get here.
    override func keyDown(with e: NSEvent) {}

    override func mouseMoved(with e: NSEvent) { controller?.moved(e) }
    override func mouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func rightMouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func otherMouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func mouseDown(with e: NSEvent) { controller?.button(0x110, down: true) }
    override func mouseUp(with e: NSEvent) { controller?.button(0x110, down: false) }
    override func rightMouseDown(with e: NSEvent) { controller?.button(0x111, down: true) }
    override func rightMouseUp(with e: NSEvent) { controller?.button(0x111, down: false) }
    override func otherMouseDown(with e: NSEvent) { controller?.otherButton(e.buttonNumber, down: true) }
    override func otherMouseUp(with e: NSEvent) { controller?.otherButton(e.buttonNumber, down: false) }
    override func scrollWheel(with e: NSEvent) { controller?.scroll(e) }
}

final class Controller: NSObject, NSApplicationDelegate, NSWindowDelegate, ConnectionDelegate {
    var window: NSWindow!
    var view: StreamView!
    var conn: Connection?
    let decoder = Decoder()
    let renderer = MetalRenderer(layerKeys: options.layerKeys, drawableKeys: options.drawableKeys)!
    let stages = StageStats()
    var netFirst: (id: UInt32, first: UInt64, last: UInt64, maxGap: UInt64, gapAt: Int, readLag: UInt64)? // measuring (GM_DEBUG_NET)
    lazy var pyro = PyroWaveDecoder(device: renderer.device, queue: renderer.queue)
    /// H.264, HEVC, and PyroWave where its Metal decoder runs.
    var clientCodecs: UInt8 { codecMask | (pyro != nil ? 1 << 3 : 0) }
    let chart = LatencyChart()
    let vsync = VsyncClock()
    lazy var phase = PhaseLock(refreshHz: Double(window.screen?.maximumFramesPerSecond ?? 60), vsync: vsync)
    let stats = Stats()
    let gate = FrameGate()
    var held = Set<UInt16>()
    let pads = Gamepads()
    let opus = OpusDecoder()
    var player: AudioPlayer?
    var locked = false, lockActive = false
    var keyMonitor: Any?
    var vrr = false // full screen on a variable-refresh display
    var codecName = "-"
    var timers: [Timer] = []
    var configureTimer: Timer?
    var closedReason: String?

    func applicationDidFinishLaunching(_ n: Notification) {
        let menu = NSMenu()
        let appItem = NSMenuItem()
        menu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Settings…", action: #selector(openSettings), keyEquivalent: ",")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Toggle Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f")
        appMenu.addItem(withTitle: "Reconnect", action: #selector(reconnect), keyEquivalent: "r")
        appMenu.addItem(withTitle: "Show/Hide Stats", action: #selector(toggleStats), keyEquivalent: "i")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit GMStream", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        NSApp.mainMenu = menu

        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 800),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "GMStream — \(options.host)"
        window.collectionBehavior = [.fullScreenPrimary]
        window.acceptsMouseMovedEvents = true
        window.delegate = self
        view = StreamView(frame: window.contentLayoutRect, layer: renderer.layer)
        view.controller = self
        window.contentView = view
        window.center()
        if let name = options.screen, let s = NSScreen.screens.first(where: { $0.localizedName.localizedCaseInsensitiveContains(name) }) {
            window.setFrameOrigin(NSPoint(x: s.frame.midX - window.frame.width / 2, y: s.frame.midY - window.frame.height / 2))
        }
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
        captureKeyboard()
        decoder.yuv = !options.bgra
        decoder.onDecoded = { [renderer] image, timing in renderer.submit(.buffer(image), timing) }
        pyro?.onDecoded = { [renderer] planes, timing in renderer.submit(.planes(planes), timing) }
        pyro?.onIncomplete = { [stats] in stats.gap() }
        renderer.onPresented = { [stages, chart, phase, vsync] t in
            stages.add(t)
            phase.presented(t)
            Trace.shared?.record(t: t.presented, aux: t.presented &- t.received, kind: 0xFE, host: t.presentLocal ?? 0)
            if getenv("GM_DEBUG_FLIP") != nil, let g = vsync.grid() {
                // Measuring: where in the refresh each frame was flipped.
                let ph = Double(Int64(t.presented) - Int64(g.vsync)).truncatingRemainder(dividingBy: g.interval)
                FileHandle.standardError.write(String(format: "flip %.3f %.3f\n", (ph < 0 ? ph + g.interval : ph) / 1e6,
                    Double(Int64(t.presented) - Int64(t.submitted)) / 1e6).data(using: .utf8)!)
            }
            if let p = t.presentLocal { chart.push(ms: Double(Int64(t.presented) - Int64(p)) / 1e6) }
        }
        _ = Trace.shared // its flush timer on the main run loop
        renderer.onSkipped = { [stages] in stages.skip() }
        renderer.onDropped = { [stages, phase] t in stages.drop(); phase.dropped(t) }
        pads.send = { [weak self] type, data in self?.conn?.send(type, data) }
        do { player = try AudioPlayer() } catch { NSLog("gmstream: audio output: \(error)") }
        NSApp.activate(ignoringOtherApps: true)

        if let screen = window.screen ?? NSScreen.main { vsync.start(on: screen) }
        applyPresentMode()
        if options.fullScreen { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.window.toggleFullScreen(nil) } }
        timers.append(Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.conn?.ping() })
        timers.append(Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.sendGrid() })
        timers.append(Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.updateTitle() })
        timers.append(Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            self?.updateOverlay()
        })
        connect()
    }

    /// This display's refresh grid, shifted by the phase lock, to the host.
    /// On a variable-refresh display, none: an empty grid, so gamescope paces
    /// itself and each frame is shown as it arrives.
    func sendGrid() {
        if let mock {
            let g = vrr || !options.phaseLock ? nil : phase.step()
            mock.setGrid(anchor: g?.anchor ?? 0, interval: g?.interval ?? 0)
            return
        }
        guard let conn else { return }
        var m = Writer()
        if vrr || !options.phaseLock || getenv("GM_NO_PHASE") != nil { // gamescope paces itself
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

    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ n: Notification) {
        releaseAll()
        setSystemHotKeys(enabled: true)
        conn?.stop()
    }

    // MARK: Session

    func wantedSize() -> (Int, Int) {
        if let s = options.size { return s }
        let scale = window.backingScaleFactor
        return (Int(view.bounds.width * scale) & ~1, Int(view.bounds.height * scale) & ~1)
    }
    func refreshMhz() -> UInt32 {
        UInt32((options.refresh ?? Double(window.screen?.maximumFramesPerSecond ?? 60)) * 1000)
    }

    var mock: MockSource?

    func connect() {
        closedReason = nil
        if options.mock {
            let (w, h) = options.size ?? (1920, 1080)
            let m = MockSource(device: renderer.device, width: w, height: h, refreshHz: options.refresh ?? 60)
            m.start { [renderer] image, timing in renderer.submit(image, timing) }
            mock = m
            DispatchQueue.main.async { self.view.streamSize = CGSize(width: w, height: h) }
            return
        }
        // Resolving the host can take seconds: not on the main thread. A
        // newer attempt (Reconnect, a settings change) supersedes this one.
        connectAttempt += 1
        let attempt = connectAttempt, host = options.host, port = options.port
        DispatchQueue.global(qos: .userInitiated).async {
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

    var connectAttempt = 0

    func start(_ c: Connection) {
        c.delegate = self
        conn = c
        let (w, h) = wantedSize()
        // Until the first frame says otherwise (the cursor's scale needs it).
        view.streamSize = CGSize(width: w, height: h)
        c.start(width: w, height: h, refreshMhz: refreshMhz(), kbps: options.mbps * 1000,
                codecMask: clientCodecs, preferred: options.codec)
        c.keepAwake(options.keepWiFiAwake)
    }

    @objc func reconnect() {
        conn?.stop()
        conn = nil
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
        if CGFloat(f.width) != view.streamSize.width || CGFloat(f.height) != view.streamSize.height {
            DispatchQueue.main.async { self.view.streamSize = CGSize(width: f.width, height: f.height) }
        }
    }

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
        if CGFloat(p.width) != view.streamSize.width || CGFloat(p.height) != view.streamSize.height {
            DispatchQueue.main.async { self.view.streamSize = CGSize(width: p.width, height: p.height) }
        }
    }

    func audio(_ packet: Data) {
        guard let player, let samples = opus?.decode(packet), !samples.isEmpty else { return }
        player.push(samples)
    }

    func helloAck(codec: UInt8) {
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
        codecName = ["H.264", "HEVC", "AV1", "PyroWave"][Int(Swift.min(codec, 3))]
        pads.resend()
    }

    func rumble(index: Int, strong: UInt16, weak: UInt16, ms: Int) {
        pads.rumble(index: index, strong: strong, weak: weak, ms: ms)
    }

    func closed(reason: String) {
        guard closedReason == nil else { return }
        closedReason = reason
        NSLog("gmstream: closed: %@", reason)
        releaseAll()
        setLock(false)
        conn?.stop()
        conn = nil
        updateTitle()
        if reason != "Another client connected" {
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
                if self?.conn == nil { self?.connect() }
            }
        }
    }

    /// Stats once a second: in the title bar and, unless hidden (Cmd+I), in
    /// an overlay that stays visible in full screen.
    func updateTitle() {
        if let r = closedReason {
            window.title = "GMStream — \(options.host) — \(r)"
            hudText = r
            updateOverlay()
            return
        }
        let s = stats.take()
        let st = stages.take()
        let pad = { (s: String, n: Int) in s.padding(toLength: n, withPad: " ", startingAt: 0) }
        let num = { (v: Double?) in pad(fmt(v), 6) }
        let total = st.rows.last!
        var lines = [
            "\(Int(view.streamSize.width))x\(Int(view.streamSize.height)) \(codecName) \(st.shown) fps shown " +
                "\(String(format: "%.0f", s.mbps)) Mbps",
            "\(pad("ms, 5 s", 9))\(pad("p50", 7))\(pad("p90", 7))max",
        ]
        for row in st.rows {
            if row.name == "total" { lines.append(String(repeating: "─", count: 27)) }
            if row.name == "pacing" { lines.append(String(repeating: "─", count: 27)) }
            lines.append("\(pad(row.name, 9))\(num(row.p50)) \(num(row.p90)) \(fmt(row.max))")
        }
        let phaseText = vrr ? "phase: off (variable refresh)" : !options.phaseLock ? "phase: off (setting)" : phase.summary()
        lines.append(phaseText + "  compositing \(renderer.compositingStatus)")
        lines += [
            "rtt \(fmt(conn.map { Double($0.rttNs) / 1e6 })) ms  skipped \(st.skipped)  dropped \(st.dropped)  gaps \(s.gaps)",
            "audio \(player.map { String(format: "%.0f ms buffered", $0.bufferedMs) } ?? (opus == nil ? "no Opus decoder" : "no output"))" +
                "  pads \(pads.connected)",
        ]
        window.title = "GMStream — \(lines[0]) · present→screen p90 \(fmt(total.p90)) ms (p50 \(fmt(total.p50)))"
        hudText = lines.joined(separator: "\n")
        if options.statsLog {
            print(lines.joined(separator: " | ").replacingOccurrences(of: "─", with: ""))
            fflush(stdout)
        }
        updateOverlay()
    }

    /// The stats overlay's text (updated once a second).
    var hudText = ""

    /// Redraws the stats overlay into the video (Latency.swift, renderOverlay).
    func updateOverlay() {
        guard options.showStats, !hudText.isEmpty else { return renderer.setOverlay(nil, inset: 0) }
        let scale = window.backingScaleFactor
        renderer.setOverlay(renderOverlay(text: hudText, chart: chart, scale: scale), inset: 8 * scale)
    }

    @objc func toggleStats() {
        options.showStats.toggle()
        options.save()
        updateOverlay()
        settings.load()
    }

    lazy var settings: SettingsWindow = {
        let s = SettingsWindow()
        s.onChange = { [weak self] change in self?.apply(change) }
        return s
    }()

    @objc func openSettings() { settings.show() }

    func apply(_ change: SettingsWindow.Change) {
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
        case .phaseLock: if options.phaseLock { phase.restart() } // sweeps for the current phase
        }
    }

    func sendConfigure() {
        guard let conn else { return }
        let (w, h) = wantedSize()
        var m = Writer()
        m.u16(UInt16(w)); m.u16(UInt16(h)); m.u32(refreshMhz())
        conn.send(Msg.configure, m.data)
    }

    func windowDidResize(_ n: Notification) {
        configureTimer?.invalidate()
        configureTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: false) { [weak self] _ in
            self?.sendConfigure()
        }
        rescaleCursor()
    }
    func windowDidChangeScreen(_ n: Notification) {
        if let s = window.screen {
            vsync.start(on: s)
            phase.restart(refreshHz: Double(s.maximumFramesPerSecond))
        }
        applyPresentMode()
        windowDidResize(n)
    }
    func windowDidEnterFullScreen(_ n: Notification) { applyPresentMode() }
    func windowDidExitFullScreen(_ n: Notification) { applyPresentMode() }

    /// Vsync on, except in full screen with "lowest latency" set. In full
    /// screen on a variable-refresh (Adaptive-Sync) display the panel refreshes
    /// when a frame arrives, so vsync costs nothing there: frames are shown
    /// tear-free as they come (measured display stage 3.3 ms, as with vsync
    /// off), and the phase lock is off.
    func applyPresentMode() {
        let fullScreen = window.styleMask.contains(.fullScreen)
        let screen = window.screen
        let wasVRR = vrr
        vrr = fullScreen && screen.map { $0.maximumRefreshInterval > $0.minimumRefreshInterval * 1.01 } ?? false
        let immediate = options.immediateFullScreen && fullScreen && !vrr
        renderer.setVsync(!immediate)
        phase.setImmediate(immediate)
        if wasVRR && !vrr { phase.restart() }
        // Tagged sRGB, macOS converts to the display; tagged as the display's
        // own color space, the pixels go to the panel unconverted.
        renderer.layer.colorspace = options.vividColors
            ? screen?.colorSpace?.cgColorSpace : CGColorSpace(name: CGColorSpace.sRGB)
    }

    func windowDidResignKey(_ n: Notification) {
        releaseAll()
        setLock(false)
        setSystemHotKeys(enabled: true)
    }
    func windowDidBecomeKey(_ n: Notification) {
        if locked { setLock(true) }
        setSystemHotKeys(enabled: false)
    }

    // MARK: Input

    /// The whole keyboard goes to the host while the stream window has focus:
    /// every key event is taken before AppKit's menus and key-view handling
    /// see it (so Command shortcuts, Ctrl+Tab, and the key-ups AppKit drops
    /// while Command is held all arrive), and the window server's own
    /// shortcuts (Cmd+Tab, Spotlight, Mission Control, screenshots) are off
    /// (setSystemHotKeys). The trackpad and mouse stay macOS's: the menu bar,
    /// another window or a Mission Control swipe gives the keyboard back.
    func captureKeyboard() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] e in
            guard let self, e.window === self.window, self.window.isKeyWindow else { return e }
            switch e.type {
            case .keyDown: key(e.keyCode, down: true, repeat: e.isARepeat)
            case .keyUp: key(e.keyCode, down: false, repeat: false)
            default: modifier(e.keyCode)
            }
            return nil
        }
    }

    /// Private (SkyLight; what VMware Fusion and Parallels use): turns the
    /// window server's global shortcuts off for this app's session, except
    /// accessibility ones. No permission needed; macOS restores them when the
    /// app exits.
    func setSystemHotKeys(enabled: Bool) {
        _ = CGSSetGlobalHotKeyOperatingMode(CGSMainConnectionID(), enabled ? 0 : 2)
    }

    func key(_ code: UInt16, down: Bool, repeat: Bool) {
        guard let k = evdevKeys[code], !`repeat` else { return } // gamescope repeats keys itself
        if down ? held.insert(k).inserted : held.remove(k) != nil { sendKey(k, down) }
    }
    func modifier(_ code: UInt16) {
        guard modifierKeys.contains(code), let k = evdevKeys[code] else { return }
        let down = !held.contains(k)
        if down { held.insert(k) } else { held.remove(k) }
        sendKey(k, down)
    }
    func sendKey(_ k: UInt16, _ down: Bool) {
        var m = Writer()
        m.u16(k); m.u8(down ? 1 : 0)
        conn?.send(Msg.key, m.data)
    }
    func releaseAll() {
        for k in held { sendKey(k, false) }
        held.removeAll()
    }

    func moved(_ e: NSEvent) {
        let r = view.videoRect
        if lockActive {
            // Relative motion in stream pixels.
            let s = view.streamSize.width / max(r.width, 1)
            var m = Writer()
            m.f32(Float(e.deltaX * s)); m.f32(Float(e.deltaY * s))
            conn?.send(Msg.motion, m.data)
            return
        }
        let p = view.convert(e.locationInWindow, from: nil)
        let x = min(1, max(0, (p.x - r.minX) / r.width))
        let y = min(1, max(0, (r.maxY - p.y) / r.height)) // AppKit's y points up
        var m = Writer()
        m.f32(Float(x)); m.f32(Float(y))
        conn?.send(Msg.position, m.data)
    }

    func button(_ code: UInt16, down: Bool) {
        if down && locked && !lockActive { setLock(true); return }
        var m = Writer()
        m.u16(code); m.u8(down ? 1 : 0)
        conn?.send(Msg.button, m.data)
    }
    func otherButton(_ n: Int, down: Bool) {
        let codes: [Int: UInt16] = [2: 0x112, 3: 0x113, 4: 0x114] // middle, back, forward
        if let c = codes[n] { button(c, down: down) }
    }

    func scroll(_ e: NSEvent) {
        // In browser wheel pixels (100 per notch), then as the web client does:
        // Wayland axis units 15 per notch, v120 120 per notch.
        let px = e.hasPreciseScrollingDeltas ? (-e.scrollingDeltaX, -e.scrollingDeltaY) : (-e.deltaX * 100, -e.deltaY * 100)
        var m = Writer()
        m.f32(Float(px.0 * 0.15)); m.f32(Float(px.1 * 0.15))
        m.i16(Int16(clamping: Int((px.0 * 1.2).rounded()))); m.i16(Int16(clamping: Int((px.1 * 1.2).rounded())))
        conn?.send(Msg.scroll, m.data)
    }

    // MARK: Cursor and pointer lock

    /// The host's cursor image, kept to rebuild at a new scale.
    var hostCursor: (width: Int, height: Int, hotX: Int, hotY: Int, rgba: Data)?

    func cursor(width: Int, height: Int, hotX: Int, hotY: Int, rgba: Data) {
        hostCursor = (width, height, hotX, hotY, rgba)
        makeCursor()
    }

    /// The stream or window size changed: the cursor's scale with it.
    func rescaleCursor() {
        if hostCursor != nil { makeCursor() } else { window.invalidateCursorRects(for: view) }
    }

    private func makeCursor() {
        guard let (width, height, hotX, hotY, rgba) = hostCursor else { return }
        guard width > 0, height > 0, rgba.count >= width * height * 4,
              let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height, bitsPerSample: 8,
                                         samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                                         bitmapFormat: [.alphaNonpremultiplied], bytesPerRow: width * 4, bitsPerPixel: 32),
              let dst = rep.bitmapData else {
            view.cursor = Self.hiddenCursor
            applyCursor()
            return
        }
        rgba.withUnsafeBytes { dst.update(from: $0.bindMemory(to: UInt8.self).baseAddress!, count: width * height * 4) }
        // Same size as in the stream: stream pixels → view points.
        let s = view.videoRect.width / max(view.streamSize.width, 1)
        let image = NSImage(size: NSSize(width: CGFloat(width) * s, height: CGFloat(height) * s))
        image.addRepresentation(rep)
        view.cursor = NSCursor(image: image, hotSpot: NSPoint(x: CGFloat(hotX) * s, y: CGFloat(hotY) * s))
        applyCursor()
    }

    static let hiddenCursor: NSCursor = {
        let image = NSImage(size: NSSize(width: 1, height: 1))
        return NSCursor(image: image, hotSpot: .zero)
    }()

    func applyCursor() {
        window.invalidateCursorRects(for: view)
        let p = view.convert(window.mouseLocationOutsideOfEventStream, from: nil)
        if view.bounds.contains(p) { view.cursor.set() }
    }

    /// The game wants relative mouse input: capture the pointer.
    func pointerLock(_ on: Bool) {
        locked = on
        setLock(on && window.isKeyWindow)
    }

    func setLock(_ on: Bool) {
        guard on != lockActive else { return }
        lockActive = on
        CGAssociateMouseAndMouseCursorPosition(on ? 0 : 1)
        if on { NSCursor.hide() } else { NSCursor.unhide() }
    }
}

@_silgen_name("CGSMainConnectionID") func CGSMainConnectionID() -> Int32
@_silgen_name("CGSSetGlobalHotKeyOperatingMode") func CGSSetGlobalHotKeyOperatingMode(_ connection: Int32, _ mode: Int32) -> Int32

let app = NSApplication.shared
let controller = Controller()
app.delegate = controller
app.setActivationPolicy(.regular)
app.run()
