// The stream, full screen: the Metal layer, and touch, hardware keyboard,
// mouse and trackpad input turned into the host's. A three-finger tap opens
// the settings.

import GameController
import SwiftUI
import UIKit

final class StreamViewController: UIViewController, SessionShell, UIPointerInteractionDelegate {
    lazy var session = Session(shell: self)
    var renderer: MetalRenderer { session.renderer }
    private var configureTimer: Timer?
    private var keepRate: CADisplayLink?
    private var wantLock = false
    /// Where the pointer (or the finger) is, in view points; nil: not over
    /// the stream. The cursor is drawn there.
    private var pointer: CGPoint?
    private let pointerPixels = PointerBox()
    private var pointerButtons: UIEvent.ButtonMask = []
    private var touch: UITouch?             // the finger that is the left button (touch as mouse)
    private var touchIds: [ObjectIdentifier: Int] = [:] // fingers down, as the host's touch ids
    private var scrolled = CGPoint.zero     // the scroll gesture's translation so far

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        view.layer.addSublayer(renderer.layer)
        view.isMultipleTouchEnabled = true
        view.addInteraction(UIPointerInteraction(delegate: self))
        let hover = UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:)))
        view.addGestureRecognizer(hover)
        let scroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        scroll.allowedScrollTypesMask = .all
        scroll.allowedTouchTypes = [] // trackpad and wheel scrolling only, not touches
        view.addGestureRecognizer(scroll)
        // The touches it began are cancelled on the host when it fires.
        let settings = UITapGestureRecognizer(target: self, action: #selector(openSettings))
        settings.numberOfTouchesRequired = 3
        view.addGestureRecognizer(settings)
        NotificationCenter.default.addObserver(forName: .GCMouseDidConnect, object: nil, queue: .main) { [weak self] n in
            if let m = n.object as? GCMouse { self?.attach(m) }
        }
        GCMouse.mice().forEach(attach)
        NotificationCenter.default.addObserver(forName: UIPointerLockState.didChangeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.lockChanged()
        }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        becomeFirstResponder()
        start()
    }

    /// Streams while the app is in front.
    func start() {
        guard view.window != nil else { return }
        // ProMotion lowers the refresh rate for content that changes less
        // often; held at the top rate, the panel has a fixed grid for the
        // phase lock.
        if keepRate == nil {
            let link = CADisplayLink(target: self, selector: #selector(tick))
            let hz = Float(maxFPS)
            link.preferredFrameRateRange = CAFrameRateRange(minimum: hz, maximum: hz, preferred: hz)
            link.add(to: .main, forMode: .common)
            keepRate = link
        }
        session.start()
    }

    func stop() {
        session.stop()
        keepRate?.invalidate()
        keepRate = nil
    }

    @objc private func tick() {}

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let scale = view.traitCollection.displayScale
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        renderer.layer.frame = view.bounds
        renderer.layer.contentsScale = scale
        renderer.layer.drawableSize = CGSize(width: max(1, view.bounds.width * scale), height: max(1, view.bounds.height * scale))
        CATransaction.commit()
        configureTimer?.invalidate()
        configureTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: false) { [weak self] _ in
            self?.session.sendConfigure()
            self?.session.applyPresentMode() // full screen or a window
        }
        updateCursor()
    }

    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }
    override var prefersPointerLocked: Bool { wantLock }
    override var canBecomeFirstResponder: Bool { true }

    // MARK: SessionShell

    private var screen: UIScreen? { view.window?.windowScene?.screen }
    var pixelSize: CGSize { CGSize(width: view.bounds.width * scale, height: view.bounds.height * scale) }
    var scale: Double { view.traitCollection.displayScale }
    var maxFPS: Double { Double(screen?.maximumFramesPerSecond ?? 60) }
    var shortestRefresh: Double { 1 / maxFPS }
    var fullScreen: Bool { screen.map { view.window?.bounds.size == $0.bounds.size } ?? false }
    var vrr: Bool { false } // held at the top rate (start)
    var displayColorSpace: CGColorSpace? { CGColorSpace(name: CGColorSpace.displayP3) }
    var focused: Bool { view.window?.windowScene?.activationState == .foregroundActive && presentedViewController == nil }
    func status(_ line: String) {}
    func cursorChanged() { makeCursor() }
    func pacingChanged() {}

    func setPointerLock(_ on: Bool) {
        wantLock = on
        setNeedsUpdateOfPrefersPointerLocked()
        lockChanged()
    }

    /// iPadOS locks the pointer only in full screen, and when it decides to.
    private func lockChanged() {
        let active = wantLock && view.window?.windowScene?.pointerLockState?.isLocked == true
        guard active != session.lockActive else { return }
        session.lockActive = active
        updateCursor()
    }

    // MARK: Settings

    @objc func openSettings() {
        guard presentedViewController == nil else { return }
        session.releaseAll()
        let sheet = UIHostingController(rootView: SettingsView(pacing: session.pacingNow()) { [weak self] changes in
            self?.dismiss(animated: true)
            changes.forEach { self?.session.apply($0) }
        })
        present(sheet, animated: true)
    }

    // MARK: Keyboard

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = keys(presses, down: true)
        if !rest.isEmpty { super.pressesBegan(rest, with: event) }
    }
    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = keys(presses, down: false)
        if !rest.isEmpty { super.pressesEnded(rest, with: event) }
    }
    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = keys(presses, down: false)
        if !rest.isEmpty { super.pressesCancelled(rest, with: event) }
    }

    /// Sends the keys the host knows; returns the presses left for UIKit.
    private func keys(_ presses: Set<UIPress>, down: Bool) -> Set<UIPress> {
        guard presentedViewController == nil else { return presses }
        var rest = Set<UIPress>()
        for p in presses {
            if let code = p.key?.keyCode.rawValue, let k = evdevKeys[code] {
                session.key(k, down: down)
            } else {
                rest.insert(p)
            }
        }
        return rest
    }

    // MARK: Pointer and touch
    //
    // A mouse or trackpad (UIApplicationSupportsIndirectInputEvents): hover
    // moves the pointer, clicks come as indirect-pointer touches with their
    // buttons. Fingers are the host's touchscreen (Session.touch), or with
    // the setting off, a finger is the pointer with the left button held.
    // With the pointer locked, motion and buttons come from GameController's
    // GCMouse.

    /// The position in the video, 0...1 from its top left, of `p` (view points).
    private func send(_ p: CGPoint) {
        pointer = p
        pointerPixels.set(CGPoint(x: p.x * scale, y: p.y * scale))
        let r = session.videoRect(in: view.bounds.size)
        session.position(x: (p.x - r.minX) / r.width, y: (p.y - r.minY) / r.height)
    }

    @objc private func hovered(_ g: UIHoverGestureRecognizer) {
        switch g.state {
        case .began, .changed: if !session.lockActive { send(g.location(in: view)) }
        default:
            pointer = nil
            pointerPixels.set(nil)
            renderer.cursorMoved()
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { touched(touches, event, ended: false) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { touched(touches, event, ended: false) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { touched(touches, event, ended: true) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        // A gesture took them (the settings): the host ends every touch.
        if !touchIds.isEmpty && touches.contains(where: { touchIds[ObjectIdentifier($0)] != nil }) {
            touchIds = [:]
            session.touch(0, .cancel)
        }
        touched(touches, event, ended: true)
    }

    private func touched(_ touches: Set<UITouch>, _ event: UIEvent?, ended: Bool) {
        for t in touches {
            if t.type != .indirectPointer && options.touchscreen {
                finger(t, ended: ended)
            } else if t.type == .indirectPointer {
                guard !session.lockActive else { continue }
                send(t.location(in: view))
                setButtons(ended ? [] : event?.buttonMask ?? .primary)
            } else if touch == nil || touch === t {
                // The first finger down is the pointer, its left button held.
                send(t.location(in: view))
                if touch == nil { touch = t; session.button(Session.leftButton, down: true) }
                if ended { touch = nil; session.button(Session.leftButton, down: false) }
            }
        }
    }

    /// A finger on the host's touchscreen, with the lowest free id.
    private func finger(_ t: UITouch, ended: Bool) {
        let key = ObjectIdentifier(t)
        let r = session.videoRect(in: view.bounds.size)
        let p = t.location(in: view)
        let (x, y) = ((p.x - r.minX) / r.width, (p.y - r.minY) / r.height)
        if let id = touchIds[key] {
            if ended {
                touchIds[key] = nil
                session.touch(id, .up)
            } else {
                session.touch(id, .moved, x: x, y: y)
            }
        } else if !ended, t.phase == .began, let id = (0..<10).first(where: { !touchIds.values.contains($0) }) {
            touchIds[key] = id
            session.touch(id, .down, x: x, y: y)
        }
    }

    /// Sends the buttons that changed (left, right, middle, back, forward).
    private func setButtons(_ mask: UIEvent.ButtonMask) {
        let codes: [UInt16] = [Session.leftButton, Session.rightButton, Session.middleButton, 0x113, 0x114]
        for (i, code) in codes.enumerated() {
            let b = UIEvent.ButtonMask.button(i + 1)
            if mask.contains(b) != pointerButtons.contains(b) { session.button(code, down: mask.contains(b)) }
        }
        pointerButtons = mask
    }

    @objc private func scrolled(_ g: UIPanGestureRecognizer) {
        if g.state == .began { scrolled = .zero }
        let t = g.translation(in: view)
        // Content dragged up (fingers up) is a wheel turned down.
        session.scroll(dx: -(t.x - scrolled.x), dy: -(t.y - scrolled.y))
        scrolled = t
    }

    private func attach(_ mouse: GCMouse) {
        guard let input = mouse.mouseInput else { return }
        input.mouseMovedHandler = { [weak self] _, dx, dy in
            guard let self, self.session.lockActive else { return }
            // Counts, taken as points; GameController's y points up.
            let s = self.session.streamSize.width / max(self.session.videoRect(in: self.view.bounds.size).width, 1)
            self.session.motion(dx: Double(dx) * s, dy: -Double(dy) * s)
        }
        let buttons: [(GCControllerButtonInput?, UInt16)] = [(input.leftButton, Session.leftButton), (input.rightButton, Session.rightButton),
                                                              (input.middleButton, Session.middleButton)]
        for (button, code) in buttons {
            button?.pressedChangedHandler = { [weak self] _, _, pressed in
                guard let self, self.session.lockActive else { return }
                self.session.button(code, down: pressed)
            }
        }
    }

    // MARK: Cursor
    //
    // iPadOS's pointer can take a shape but not an image: it is hidden over
    // the stream, and the cursor (Session.clientCursor) is drawn into our frames at the
    // pointer's (or the finger's) position.

    func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? { .hidden() }

    private var cursorTexture: (texture: MTLTexture, hotSpot: CGPoint)?

    private func makeCursor() {
        defer { updateCursor() }
        guard let host = session.clientCursor else {
            cursorTexture = nil
            return
        }
        // Same size as in the stream: stream pixels → device pixels.
        let s = session.videoRect(in: view.bounds.size).width / max(session.streamSize.width, 1) * scale
        if let image = host.image,
           let t = renderer.texture(from: image, width: Int((Double(host.width) * s).rounded(.up)),
                                    height: Int((Double(host.height) * s).rounded(.up))) {
            cursorTexture = (t, CGPoint(x: Double(host.hotX) * s, y: Double(host.hotY) * s))
        } else if let t = arrow() {
            cursorTexture = (t, .zero) // shown, but no image we can use
        }
    }

    private func updateCursor() {
        let show = cursorTexture != nil && !session.lockActive
        session.cursorMode = show ? "frame" : "host"
        guard show, let (texture, hot) = cursorTexture else { return renderer.setCursor(nil) }
        renderer.setCursor(texture, hotSpot: hot, refresh: shortestRefresh) { [pointerPixels] in pointerPixels.get() }
    }

    /// A plain arrow, 12x19 points, for a host cursor with no image.
    private func arrow() -> MTLTexture? {
        let s = scale, w = Int(12 * s), h = Int(19 * s)
        guard let g = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        g.translateBy(x: 0, y: Double(h))
        g.scaleBy(x: s, y: -s)
        let points = [(1, 1), (1, 16), (4.5, 12.8), (7, 18), (9.2, 17), (6.8, 12), (11.5, 12)].map { CGPoint(x: $0.0, y: $0.1) }
        g.addLines(between: points)
        g.closePath()
        g.setFillColor(gray: 0, alpha: 1)
        g.setStrokeColor(gray: 1, alpha: 1)
        g.setLineWidth(1)
        g.setLineJoin(.round)
        g.drawPath(using: .fillStroke)
        return g.makeImage().flatMap { renderer.texture(from: $0, width: w, height: h) }
    }
}

/// The pointer in drawable pixels, read on the render thread.
final class PointerBox {
    private let lock = NSLock()
    private var p: CGPoint?
    func set(_ p: CGPoint?) { lock.lock(); self.p = p; lock.unlock() }
    func get() -> CGPoint? { lock.lock(); defer { lock.unlock() }; return p }
}
