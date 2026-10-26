// Game controllers (Bluetooth or USB) through Apple's GameController
// framework, sent as the same Gamepad messages the web client sends (W3C
// standard mapping). State goes out on every change, not on a poll. Rumble
// from the host plays on the controller's haptics.

import CoreHaptics
import GameController

final class Gamepads {
    /// Sends a message (type, payload) to the host.
    var send: (UInt8, Data) -> Void = { _, _ in }

    private var slots: [GCController?] = [nil, nil, nil, nil]
    private var last: [Data?] = [nil, nil, nil, nil]
    private var haptics: [ObjectIdentifier: CHHapticEngine] = [:]
    private var players: [ObjectIdentifier: CHHapticAdvancedPatternPlayer] = [:]

    init() {
        // Keep receiving controller input while another app is in front.
        GCController.shouldMonitorBackgroundEvents = true
        let nc = NotificationCenter.default
        nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.attach(c) }
        }
        nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.detach(c) }
        }
        GCController.controllers().forEach(attach)
    }

    /// After (re)connecting to the host: send every pad's current state.
    func resend() {
        last = [nil, nil, nil, nil]
        for (i, c) in slots.enumerated() { if let c { update(i, c) } }
    }

    var connected: Int { slots.compactMap { $0 }.count }

    private func attach(_ c: GCController) {
        guard c.extendedGamepad != nil, !slots.contains(where: { $0 === c }),
              let i = slots.firstIndex(where: { $0 == nil }) else { return }
        slots[i] = c
        c.playerIndex = GCControllerPlayerIndex(rawValue: i) ?? .indexUnset
        // macOS keeps the Home (Xbox/PS) button and the View/Options button
        // for its own gestures unless the app asks for them; Steam needs Home.
        if let g = c.extendedGamepad {
            for e in [g.buttonHome, g.buttonOptions, g.buttonMenu] as [GCControllerElement?] {
                e?.preferredSystemGestureState = .disabled
            }
        }
        c.extendedGamepad?.valueChangedHandler = { [weak self, weak c] _, _ in
            guard let self, let c, let i = self.slots.firstIndex(where: { $0 === c }) else { return }
            self.update(i, c)
        }
        update(i, c)
    }

    private func detach(_ c: GCController) {
        guard let i = slots.firstIndex(where: { $0 === c }) else { return }
        slots[i] = nil
        last[i] = nil
        haptics[ObjectIdentifier(c)]?.stop()
        haptics[ObjectIdentifier(c)] = nil
        players[ObjectIdentifier(c)] = nil
        send(Msg.gamepadRemove, Data([UInt8(i)]))
    }

    private func update(_ i: Int, _ c: GCController) {
        guard let g = c.extendedGamepad else { return }
        // W3C standard gamepad: bit i = buttons[i].
        let pressed: [Bool] = [
            g.buttonA.isPressed, g.buttonB.isPressed, g.buttonX.isPressed, g.buttonY.isPressed,
            g.leftShoulder.isPressed, g.rightShoulder.isPressed, g.leftTrigger.isPressed, g.rightTrigger.isPressed,
            g.buttonOptions?.isPressed ?? false, g.buttonMenu.isPressed,
            g.leftThumbstickButton?.isPressed ?? false, g.rightThumbstickButton?.isPressed ?? false,
            g.dpad.up.isPressed, g.dpad.down.isPressed, g.dpad.left.isPressed, g.dpad.right.isPressed,
            g.buttonHome?.isPressed ?? false,
        ]
        var buttons: UInt32 = 0
        for (bit, on) in pressed.enumerated() where on { buttons |= 1 << UInt32(bit) }
        let axis = { (v: Float) in Int16(clamping: Int((v * 32767).rounded())) }
        var w = Writer()
        w.u8(UInt8(i))
        w.u32(buttons)
        // GameController's y points up; the protocol's points down.
        w.i16(axis(g.leftThumbstick.xAxis.value)); w.i16(axis(-g.leftThumbstick.yAxis.value))
        w.i16(axis(g.rightThumbstick.xAxis.value)); w.i16(axis(-g.rightThumbstick.yAxis.value))
        w.u8(UInt8((g.leftTrigger.value * 255).rounded())); w.u8(UInt8((g.rightTrigger.value * 255).rounded()))
        guard w.data != last[i] else { return }
        last[i] = w.data
        send(Msg.gamepad, w.data)
    }

    /// Rumble from the host: the stronger motor's strength for `ms`, or stop.
    func rumble(index: Int, strong: UInt16, weak: UInt16, ms: Int) {
        guard index < slots.count, let c = slots[index] else { return }
        let id = ObjectIdentifier(c)
        let intensity = Float(max(strong, weak)) / 65535
        if ms == 0 || intensity == 0 {
            try? players[id]?.stop(atTime: CHHapticTimeImmediate)
            return
        }
        if haptics[id] == nil, let engine = c.haptics?.createEngine(withLocality: .default) {
            engine.isAutoShutdownEnabled = true
            try? engine.start()
            haptics[id] = engine
        }
        guard let engine = haptics[id] else { return }
        let event = CHHapticEvent(eventType: .hapticContinuous,
                                  parameters: [CHHapticEventParameter(parameterID: .hapticIntensity, value: intensity),
                                               CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.5)],
                                  relativeTime: 0, duration: Double(ms) / 1000)
        do {
            try? players[id]?.stop(atTime: CHHapticTimeImmediate)
            try engine.start()
            let player = try engine.makeAdvancedPlayer(with: CHHapticPattern(events: [event], parameters: []))
            players[id] = player
            try player.start(atTime: CHHapticTimeImmediate)
        } catch {
            haptics[id] = nil
        }
    }
}
