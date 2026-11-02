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
import GameController

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
    func relative(_ on: Bool) {}
    func cursorClip(_ clip: CGRect?) {}
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

    // Keys: Controller.takeKeyboard takes them before they get here.
    override func keyDown(with e: NSEvent) {}

    override func mouseMoved(with e: NSEvent) { controller?.moved(e) }
    override func mouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func rightMouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func otherMouseDragged(with e: NSEvent) { controller?.moved(e) }
    override func mouseDown(with e: NSEvent) {
        logCursor("click view")
        controller?.session.button(Session.leftButton, down: true)
    }
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
    var cursorHidden = false
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
        // Outside the view too, for coming back in. A tracking area as well would
        // deliver every event inside the view twice.
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
        takeKeyboard()
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
        session.probe?.sweep(how: AXIsProcessTrusted() ? "events" : "warp") { [weak self] x in self?.placeMouse(x) }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ n: Notification) {
        session.stop()
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
    var vrr: Bool { fullScreen && vrrCapable }
    var vrrCapable: Bool { window.screen?.variableRefresh == true }
    var displayColorSpace: CGColorSpace? { window.screen?.colorSpace?.cgColorSpace }
    var focused: Bool { window.isKeyWindow }
    func status(_ line: String) { window.title = "GMStream — \(line)" }
    func cursorChanged() { makeCursor() }
    var capturesMouse: Bool { true }
    func pacingChanged() { settings.load() }

    func setPointerLock(_ on: Bool) {
        guard on != session.lockActive else { return }
        // The mouse outside the view is the system's (the title bar, dragging
        // the window), until it comes in (moved).
        let mouse = view.convert(window.mouseLocationOutsideOfEventStream, from: nil)
        if on && !view.bounds.contains(mouse) { return }
        session.lockActive = on
        logCursor(String(format: "capture %d mouse %.1f %.1f view %.0f %.0f", on ? 1 : 0, mouse.x, view.bounds.height - mouse.y, view.bounds.width, view.bounds.height))
        if on {
            // Our cursor goes on from where the mouse is. The system's waits
            // in the middle of the view, hidden: macOS sends clicks to where it
            // really is, and at an edge that can be the title bar.
            capturedPoint.value = session.confined ? clampedToArea(mouse) : mouse
            warpMouse(to: CGPoint(x: view.bounds.midX, y: view.bounds.midY))
            CGAssociateMouseAndMouseCursorPosition(0)
        } else {
            // And the system's goes on from where ours went.
            CGAssociateMouseAndMouseCursorPosition(1)
            warpMouse(to: capturedPoint.value)
            CGAssociateMouseAndMouseCursorPosition(1) // or macOS ignores the mouse for 0.25 s after a warp
        }
        updateCursor()
    }

    private func clampedToArea(_ p: CGPoint) -> CGPoint {
        var area = session.cursorArea(in: view.bounds.size)
        area.origin.y = view.bounds.height - area.maxY // AppKit's y points up
        return clamped(p, to: area)
    }

    private func clamped(_ p: CGPoint, to r: CGRect) -> CGPoint {
        CGPoint(x: min(r.maxX, max(r.minX, p.x)), y: min(r.maxY, max(r.minY, p.y)))
    }

    /// The system cursor to `p` in the view. Mouse events that are already on
    /// their way still say where it was, and one after it can carry the whole
    /// jump as its delta: until the warp has settled, none of them count.
    private func warpMouse(to p: CGPoint) {
        let s = window.convertPoint(toScreen: view.convert(p, to: nil))
        let g = CGPoint(x: s.x, y: (NSScreen.screens.first?.frame.maxY ?? 0) - s.y)
        logCursor(String(format: "warp %.1f %.1f", p.x, view.bounds.height - p.y))
        CGWarpMouseCursorPosition(g)
        settling = (g, ProcessInfo.processInfo.systemUptime + 0.1)
    }

    /// Whether this mouse event is from before a warp settled.
    private func stillSettling(_ e: NSEvent) -> Bool {
        guard let s = settling else { return false }
        let at = e.cgEvent?.location ?? .zero
        if hypot(at.x - s.at.x, at.y - s.at.y) < 30 || ProcessInfo.processInfo.systemUptime > s.until { settling = nil }
        return true
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
        s.pacingNow = { [weak self] in self?.session.pacingNow() ?? (options.phaseLock, options.holdFrames, options.immediateFullScreen, options.vrrMode, "") }
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
    }
    func windowDidBecomeKey(_ n: Notification) {
        session.updateCapture()
    }

    // MARK: Input

    /// macOS has the keyboard first, then the host, while the stream window
    /// has focus. The window server's shortcuts (Cmd+Tab, Spotlight, Mission
    /// Control, screenshots) never get here; this app's menu goes next; any
    /// other key goes to the host. Events are taken before AppKit's key-view
    /// handling, so Ctrl+Tab and the key-ups AppKit drops while Command is
    /// held still arrive.
    func takeKeyboard() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] e in
            guard let self, e.window === self.window, self.window.isKeyWindow else { return e }
            switch e.type {
            case .keyDown:
                if NSApp.mainMenu?.performKeyEquivalent(with: e) != true { key(e.keyCode, down: true, repeat: e.isARepeat) }
            case .keyUp: key(e.keyCode, down: false, repeat: false)
            default: modifier(e)
            }
            return nil
        }
    }

    func key(_ code: UInt16, down: Bool, repeat: Bool) {
        guard let k = evdevKeys[code], !`repeat` else { return } // gamescope repeats keys itself
        session.key(k, down: down)
    }
    /// From the event's flags, not by toggling: focus can come back with a
    /// modifier held (Cmd+Tab), and its release must not count as a press.
    func modifier(_ e: NSEvent) {
        guard modifierKeys.contains(e.keyCode), let k = evdevKeys[e.keyCode] else { return }
        let down = modifierMasks[e.keyCode].map { e.modifierFlags.rawValue & $0 != 0 } ?? !session.held.contains(k)
        session.key(k, down: down)
    }

    /// Measuring (MouseProbe): the real pointer to x across the video, as a
    /// mouse would put it there; without Accessibility, a warp and what the
    /// event would have sent.
    private func placeMouse(_ x: Double) {
        let r = session.videoRect(in: view.bounds.size)
        let p = window.convertPoint(toScreen: view.convert(NSPoint(x: r.minX + x * r.width, y: r.midY), to: nil))
        let g = CGPoint(x: p.x, y: NSScreen.screens[0].frame.maxY - p.y)
        if AXIsProcessTrusted() {
            CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: g, mouseButton: .left)?.post(tap: .cghidEventTap)
        } else {
            CGWarpMouseCursorPosition(g)
            session.position(x: x, y: 0.5)
        }
    }

    func moved(_ e: NSEvent) {
        let at = view.convert(e.locationInWindow, from: nil), was = capturedPoint.value, wasCaptured = session.lockActive
        defer {
            let p = capturedPoint.value, h = view.bounds.height
            logCursor(String(format: "move loc %.1f %.1f delta %.1f %.1f captured %d cursor_before %.1f %.1f cursor %.1f %.1f in_view %d",
                             at.x, h - at.y, e.deltaX, e.deltaY, wasCaptured ? 1 : 0, was.x, h - was.y, p.x, h - p.y, view.bounds.contains(at) ? 1 : 0))
        }
        let r = session.videoRect(in: view.bounds.size)
        var p = view.convert(e.locationInWindow, from: nil)
        if stillSettling(e) { return }
        if session.lockActive {
            // Captured: the system cursor doesn't move, so the cursor is the
            // sum of the motion (AppKit's y points up, a delta's down).
            let c = capturedPoint.value
            let target = CGPoint(x: c.x + e.deltaX, y: c.y - e.deltaY)
            p = session.confined ? clampedToArea(target) : clamped(target, to: view.bounds)
            capturedPoint.value = p
            if session.sendsMotion {
                // Relative motion in stream pixels.
                let s = session.streamSize.width / max(r.width, 1)
                session.motion(dx: e.deltaX * s, dy: e.deltaY * s)
                renderer.cursorMoved()
                return
            }
        }
        if !session.lockActive && session.wantsCapture && window.isKeyWindow {
            // The system cursor is out of the view; in it, it's ours.
            if view.bounds.contains(p) { session.updateCapture() }
            return
        }
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

    /// Where a captured mouse is, in the view: the main thread adds to it, the
    /// render thread reads it.
    private let capturedPoint = LockedPoint()
    private var settling: (at: CGPoint, until: TimeInterval)? // a warp, see warpMouse

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

    /// After any change to the cursor, the capture, focus or the window's
    /// place. The system cursor is hidden whenever the mouse is captured and
    /// never used otherwise; ours is drawn into every frame. NSCursor.hide()
    /// nests, so one flag owns it.
    func updateCursor() {
        let captured = session.lockActive && window.isKeyWindow
        if captured != cursorHidden {
            cursorHidden = captured
            if captured { NSCursor.hide() } else { NSCursor.unhide() }
        }
        session.cursorMode = localCursor == nil ? "hidden" : "drawn"
        guard captured, let texture = localCursor?.texture else {
            renderer.setCursor(nil)
            return
        }
        // Read on the render thread as each frame is drawn: drawable pixels
        // from the top left, the view's y points up.
        let scale = window.backingScaleFactor, height = view.bounds.height
        let hot = localCursor!.cursor.hotSpot, point = capturedPoint
        renderer.setCursor(texture, hotSpot: CGPoint(x: hot.x * scale, y: hot.y * scale), refresh: shortestRefresh, local: true) {
            let p = point.value
            return CGPoint(x: p.x * scale, y: (height - p.y) * scale)
        }
    }
}

/// Measuring (GM_DEBUG_CURSOR=<file>): every mouse event as macOS delivered it,
/// and what the app did about it, for mac/tools/cursorcheck.py to judge. The
/// raw device motion from GCMouse doesn't go through the window server, so
/// warps can't touch it: it says what the hand really did.
private let cursorLog = getenv("GM_DEBUG_CURSOR").flatMap { fopen($0, "w") }
private let cursorLogLock = NSLock()
func logCursor(_ line: @autoclosure () -> String) {
    guard let cursorLog else { return }
    let text = String(format: "%.6f ", ProcessInfo.processInfo.systemUptime) + line() + "\n"
    cursorLogLock.lock()
    fputs(text, cursorLog)
    fflush(cursorLog)
    cursorLogLock.unlock()
}
func logRawMouse() {
    guard cursorLog != nil else { return }
    GCController.shouldMonitorBackgroundEvents = true
    let watch: (GCMouse?) -> Void = { mouse in
        mouse?.mouseInput?.mouseMovedHandler = { _, dx, dy in logCursor(String(format: "hid %.1f %.1f", dx, -dy)) }
        mouse?.mouseInput?.leftButton.pressedChangedHandler = { _, _, down in if down { logCursor("hidclick") } }
    }
    GCMouse.mice().forEach { watch($0) }
    NotificationCenter.default.addObserver(forName: .GCMouseDidConnect, object: nil, queue: .main) { watch($0.object as? GCMouse) }
}

final class LockedPoint {
    private let lock = NSLock()
    private var point = CGPoint.zero
    var value: CGPoint {
        get { lock.lock(); defer { lock.unlock() }; return point }
        set { lock.lock(); point = newValue; lock.unlock() }
    }
}

let app = NSApplication.shared
let controller = Controller()
app.delegate = controller
app.setActivationPolicy(.regular)
logRawMouse()
app.run()
