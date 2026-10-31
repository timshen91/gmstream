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

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow,
                                                              .inVisibleRect, .cursorUpdate],
                                       owner: self, userInfo: nil))
    }
    override func mouseEntered(with e: NSEvent) { controller?.pointerInView = true; controller?.updateCursor() }
    override func mouseExited(with e: NSEvent) { controller?.pointerInView = false; controller?.updateCursor() }
    override func resetCursorRects() { addCursorRect(bounds, cursor: cursor) }
    override func cursorUpdate(with event: NSEvent) { cursor.set() }

    // Keys: Controller.captureKeyboard takes them before they get here.
    override func keyDown(with e: NSEvent) {}

    override func mouseMoved(with e: NSEvent) { controller?.moved(e) }
    override func mouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func rightMouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func otherMouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func mouseDown(with e: NSEvent) { controller?.session.button(Session.leftButton, down: true) }
    override func mouseUp(with e: NSEvent) { controller?.session.button(Session.leftButton, down: false) }
    override func rightMouseDown(with e: NSEvent) { controller?.session.button(Session.rightButton, down: true) }
    override func rightMouseUp(with e: NSEvent) { controller?.session.button(Session.rightButton, down: false) }
    override func otherMouseDown(with e: NSEvent) { controller?.otherButton(e.buttonNumber, down: true) }
    override func otherMouseUp(with e: NSEvent) { controller?.otherButton(e.buttonNumber, down: false) }
    override func scrollWheel(with e: NSEvent) { controller?.scroll(e) }
}

/// The app: its menu, the stream window, keyboard capture and the cursor,
/// around one Session.
final class Controller: NSObject, NSApplicationDelegate, NSWindowDelegate, SessionShell {
    var window: NSWindow!
    var view: StreamView!
    lazy var session = Session(shell: self)
    var renderer: MetalRenderer { session.renderer }
    var pointerInView = false, cursorHidden = false
    var keyMonitor: Any?
    var configureTimer: Timer?

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
        NSApp.activate(ignoringOtherApps: true)

        // A panel that slept or was reconfigured may refresh on a new grid.
        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.screensDidWakeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.session.screenChanged()
        }
        NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            self?.session.screenChanged()
        }
        if options.fullScreen { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.window.toggleFullScreen(nil) } }
        session.start()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ n: Notification) {
        session.stop()
        setSystemHotKeys(enabled: true)
    }

    // MARK: SessionShell

    var pixelSize: CGSize {
        let scale = window.backingScaleFactor
        return CGSize(width: view.bounds.width * scale, height: view.bounds.height * scale)
    }
    var scale: Double { window.backingScaleFactor }
    var maxFPS: Double { Double(window.screen?.maximumFramesPerSecond ?? 60) }
    var shortestRefresh: Double { window.screen?.minimumRefreshInterval ?? 1.0 / 120 }
    var fullScreen: Bool { window.styleMask.contains(.fullScreen) }
    var vrr: Bool { fullScreen && window.screen?.variableRefresh == true }
    var displayColorSpace: CGColorSpace? { window.screen?.colorSpace?.cgColorSpace }
    var focused: Bool { window.isKeyWindow }
    func status(_ line: String) { window.title = "GMStream — \(line)" }
    func cursorChanged() { makeCursor() }
    func pacingChanged() { settings.load() }

    func setPointerLock(_ on: Bool) {
        guard on != session.lockActive else { return }
        session.lockActive = on
        CGAssociateMouseAndMouseCursorPosition(on ? 0 : 1)
        updateCursor()
    }

    // MARK: Menu and settings

    @objc func reconnect() { session.reconnect() }

    @objc func toggleStats() {
        options.showStats.toggle()
        options.save()
        session.updateOverlay()
        settings.load()
    }

    lazy var settings: SettingsWindow = {
        let s = SettingsWindow()
        s.onChange = { [weak self] change in self?.session.apply(change) }
        s.pacingNow = { [weak self] in self?.session.pacingNow() ?? (options.phaseLock, options.holdFrames, "") }
        return s
    }()

    @objc func openSettings() { settings.show() }

    // MARK: Window

    func windowDidResize(_ n: Notification) {
        configureTimer?.invalidate()
        configureTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: false) { [weak self] _ in
            self?.session.sendConfigure()
        }
        makeCursor()
    }
    func windowDidMove(_ n: Notification) { updateCursor() }
    func windowDidChangeScreen(_ n: Notification) {
        session.screenChanged()
        windowDidResize(n)
    }
    func windowDidEnterFullScreen(_ n: Notification) { session.applyPresentMode() }
    func windowDidExitFullScreen(_ n: Notification) { session.applyPresentMode() }

    func windowDidResignKey(_ n: Notification) {
        session.releaseAll()
        setPointerLock(false)
        updateCursor()
        setSystemHotKeys(enabled: true)
    }
    func windowDidBecomeKey(_ n: Notification) {
        if session.locked { setPointerLock(true) }
        pointerInView = view.bounds.contains(view.convert(window.mouseLocationOutsideOfEventStream, from: nil))
        updateCursor()
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
        session.key(k, down: down)
    }
    func modifier(_ code: UInt16) {
        guard modifierKeys.contains(code), let k = evdevKeys[code] else { return }
        session.key(k, down: !session.held.contains(k))
    }

    func moved(_ e: NSEvent) {
        let r = session.videoRect(in: view.bounds.size)
        if session.lockActive {
            // Relative motion in stream pixels.
            let s = session.streamSize.width / max(r.width, 1)
            session.motion(dx: e.deltaX * s, dy: e.deltaY * s)
            return
        }
        let p = view.convert(e.locationInWindow, from: nil)
        session.position(x: (p.x - r.minX) / r.width, y: (r.maxY - p.y) / r.height) // AppKit's y points up
    }

    func otherButton(_ n: Int, down: Bool) {
        let codes: [Int: UInt16] = [2: 0x112, 3: 0x113, 4: 0x114] // middle, back, forward
        if let c = codes[n] { session.button(c, down: down) }
    }

    func scroll(_ e: NSEvent) {
        // In browser wheel pixels (100 per notch).
        let px = e.hasPreciseScrollingDeltas ? (-e.scrollingDeltaX, -e.scrollingDeltaY) : (-e.deltaX * 100, -e.deltaY * 100)
        session.scroll(dx: px.0, dy: px.1)
    }

    // MARK: Cursor
    //
    // In full screen on a variable-refresh display the host's cursor is drawn
    // into the frame: any cursor sprite the window server moves there (a
    // transparent one too) makes the panel refresh at its fixed top rate
    // while the mouse moves, and frames wait for that grid (display stage
    // 3.2 -> 7.3 ms p50 in --mock). Elsewhere the display refreshes at a
    // fixed rate anyway: the macOS cursor.

    /// The cursor to show, at the current scale, and its image as a texture.
    private var localCursor: (cursor: NSCursor, texture: MTLTexture?)?

    /// The cursor to show at the current scale (after it, the stream or the
    /// window size changed).
    private func makeCursor() {
        defer { updateCursor() }
        guard let host = session.clientCursor else {
            localCursor = nil
            return
        }
        var cursor = NSCursor.arrow // shown, but no image we can use
        if let image = host.image {
            // Same size as in the stream: stream pixels → view points.
            let s = session.videoRect(in: view.bounds.size).width / max(session.streamSize.width, 1)
            cursor = NSCursor(image: NSImage(cgImage: image, size: NSSize(width: CGFloat(host.width) * s, height: CGFloat(host.height) * s)),
                              hotSpot: NSPoint(x: CGFloat(host.hotX) * s, y: CGFloat(host.hotY) * s))
        }
        let scale = window.backingScaleFactor
        let texture = cursor.image.cgImage(forProposedRect: nil, context: nil, hints: nil).flatMap {
            renderer.texture(from: $0, width: Int((cursor.image.size.width * scale).rounded(.up)),
                             height: Int((cursor.image.size.height * scale).rounded(.up)))
        }
        localCursor = (cursor, texture)
    }

    /// Applies who draws the cursor (above) after any change to the host's
    /// cursor, the pointer lock, focus, where the pointer is, the window's
    /// place, or the present mode. NSCursor.hide() nests, so one flag owns it.
    func updateCursor() {
        let vrr = session.pacing.vrr, lockActive = session.lockActive
        let over = pointerInView && window.isKeyWindow
        let show = localCursor != nil && !lockActive
        let hide = lockActive || (over && (!show || vrr))
        session.cursorMode = !show ? "host" : vrr ? "frame" : "macOS"
        if hide != cursorHidden {
            cursorHidden = hide
            if hide { NSCursor.hide() } else { NSCursor.unhide() }
        }
        view.cursor = localCursor?.cursor ?? .arrow
        window.invalidateCursorRects(for: view)
        if over && show && !vrr { view.cursor.set() }
        guard over && show && vrr, let texture = localCursor?.texture else {
            renderer.setCursor(nil)
            return
        }
        // The drawable's top left in global display coordinates (points, y
        // down from the top of the main display), as CGEvent reports the
        // mouse; read on the render thread as each frame is drawn.
        let scale = window.backingScaleFactor
        let r = window.convertToScreen(view.convert(view.bounds, to: nil))
        let origin = CGPoint(x: r.minX, y: (NSScreen.screens.first?.frame.maxY ?? 0) - r.maxY)
        let hot = localCursor!.cursor.hotSpot
        renderer.setCursor(texture, hotSpot: CGPoint(x: hot.x * scale, y: hot.y * scale), refresh: shortestRefresh) {
            CGEvent(source: nil).map { CGPoint(x: ($0.location.x - origin.x) * scale, y: ($0.location.y - origin.y) * scale) }
        }
    }
}

@_silgen_name("CGSMainConnectionID") func CGSMainConnectionID() -> Int32
@_silgen_name("CGSSetGlobalHotKeyOperatingMode") func CGSSetGlobalHotKeyOperatingMode(_ connection: Int32, _ mode: Int32) -> Int32

let app = NSApplication.shared
let controller = Controller()
app.delegate = controller
app.setActivationPolicy(.regular)
app.run()
