// The settings sheet (a three-finger tap on the stream). Done saves them and
// applies what changed: bitrate live, refresh and size by reconfiguring the
// stream, host, port and codec by reconnecting.

import SwiftUI

struct SettingsView: View {
    /// What pacing is in effect (Session.pacingNow).
    let pacing: (phaseLock: Bool, hold: Bool, vsyncOff: Bool, vrr: VRRMode, note: String)
    /// Called with what changed, after `options` was updated and saved.
    let done: ([SettingsChange]) -> Void

    @State private var o = options
    @State private var port = String(options.port)
    @State private var bitrate = String(options.mbps)
    @State private var size = options.sizeText
    private static let refreshRates = [30, 60, 90, 120, 144, 165, 240]

    var body: some View {
        NavigationStack {
            Form {
                Section("Host") {
                    LabeledContent("Host") {
                        TextField("steamdeck.local", text: $o.host).multilineTextAlignment(.trailing)
                            .textInputAutocapitalization(.never).autocorrectionDisabled()
                    }
                    LabeledContent("Port") { TextField("8444", text: $port).multilineTextAlignment(.trailing).keyboardType(.numberPad) }
                }
                Section {
                    Picker("Codec", selection: $o.codec) {
                        Text("H.264").tag(UInt8(0))
                        Text("HEVC").tag(UInt8(1))
                        if Decoder.hasAV1 { Text("AV1").tag(UInt8(2)) }
                        Text("PyroWave").tag(UInt8(3))
                    }
                    LabeledContent("Bitrate (Mb/s)") { TextField("40", text: $bitrate).multilineTextAlignment(.trailing).keyboardType(.numberPad) }
                    Picker("Refresh rate", selection: $o.refresh) {
                        Text("Match display").tag(Double?.none)
                        ForEach(Self.refreshRates, id: \.self) { Text("\($0) Hz").tag(Optional(Double($0))) }
                    }
                    LabeledContent("Fixed size") {
                        TextField("screen size, e.g. 2560x1600", text: $size).multilineTextAlignment(.trailing)
                            .textInputAutocapitalization(.never).autocorrectionDisabled()
                    }
                } header: { Text("Stream") } footer: { Text("Refresh rate is the host's frame-rate cap.") }
                Section {
                    Toggle("Show stats overlay", isOn: $o.showStats)
                    Toggle("Vivid colors (the display's full gamut)", isOn: $o.vividColors)
                    Toggle("Keep Wi-Fi awake (60 tiny packets/s to the host)", isOn: $o.keepWiFiAwake)
                    Toggle("Take AV1 frames as the host encodes them", isOn: $o.streamFrames)
                }
                Section {
                    Picker("Cursor", selection: $o.cursorMode) {
                        ForEach(CursorMode.allCases, id: \.self) { Text($0.title).tag($0) }
                    }
                    Picker("Mouse", selection: $o.mouseMode) {
                        ForEach(MouseMode.allCases, id: \.self) { Text($0.title).tag($0) }
                    }
                } footer: { Text("Show: a mouse or trackpad always shows a cursor here, also where the host hides it.") }
                Section {
                    Toggle("Touch as a touchscreen", isOn: $o.touchscreen)
                } footer: {
                    Text(o.touchscreen ? "Fingers are the host's touchscreen, as on a Steam Deck."
                         : "A finger is the mouse, its left button held.")
                }
                Section {
                    Picker("Pacing", selection: $o.pacingMode) {
                        ForEach(PacingMode.allCases, id: \.self) { Text($0.title).tag($0) }
                    }
                    Toggle("Pace the host to this display's refreshes (phase lock)",
                           isOn: o.pacingMode == .manual ? $o.phaseLock : .constant(pacing.phaseLock)).disabled(o.pacingMode != .manual)
                    Toggle("Even out network jitter (fixed delay after the host)",
                           isOn: o.pacingMode == .manual ? $o.holdFrames : .constant(pacing.hold)).disabled(o.pacingMode != .manual)
                } footer: { Text(o.pacingMode == options.pacingMode ? pacing.note : "") }
                Section {
                    Button("Copy line for ~/.ssh/authorized_keys") { UIPasteboard.general.string = Identity.shared.authorizedLine }
                    Button("Forget host's key") { UserDefaults.standard.removeObject(forKey: "hostKeys") }
                } header: { Text("This device") }
                Section {
                    Button("Reconnect") { finish(reconnect: true) }
                }
            }
            .navigationTitle("GMStream")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { finish(reconnect: false) } } }
        }
    }

    private func finish(reconnect: Bool) {
        var n = o
        n.host = n.host.trimmingCharacters(in: .whitespaces)
        if n.host.isEmpty { n.host = options.host }
        n.port = UInt16(port) ?? options.port
        if let m = UInt32(bitrate), (1...500).contains(m) { n.mbps = m }
        let text = size.trimmingCharacters(in: .whitespaces)
        if text.isEmpty { n.size = nil } else if let s = Options.parseSize(text) { n.size = s }
        let old = options
        var changes: [SettingsChange] = []
        if reconnect || n.host != old.host || n.port != old.port || n.codec != old.codec { changes.append(.connection) }
        if n.mbps != old.mbps { changes.append(.bitrate) }
        if n.refresh != old.refresh || n.sizeText != old.sizeText { changes.append(.display) }
        if n.showStats != old.showStats { changes.append(.stats) }
        if n.vividColors != old.vividColors { changes.append(.present) }
        if n.keepWiFiAwake != old.keepWiFiAwake { changes.append(.keepAwake) }
        // The host is told in Hello, so the stream starts over.
        if n.streamFrames != old.streamFrames { changes.append(.connection) }
        if n.cursorMode != old.cursorMode || n.mouseMode != old.mouseMode { changes.append(.cursor) }
        if n.pacingMode != old.pacingMode || n.phaseLock != old.phaseLock || n.holdFrames != old.holdFrames { changes.append(.phaseLock) }
        options = n
        options.save()
        done(changes)
    }
}
