// The Settings window (Cmd+,). Every change is saved and applied at once:
// bitrate live, refresh and size by reconfiguring the stream, host, port and
// codec by reconnecting.

import AppKit

final class SettingsWindow: NSObject {
    typealias Change = SettingsChange

    let window: NSWindow
    /// Called after `options` has been updated and saved.
    var onChange: (Change) -> Void = { _ in }
    /// What pacing is in effect (Auto picks it from the display): phase lock,
    /// hold, and a line on what Auto chose and did to Steam's VRR toggle.
    var pacingNow: () -> (phaseLock: Bool, hold: Bool, vsyncOff: Bool, vrr: VRRMode, note: String) = { (options.phaseLock, options.holdFrames, options.immediateFullScreen, options.vrrMode, "") }

    private let host = NSTextField()
    private let port = NSTextField()
    private let codec = NSPopUpButton()
    private let bitrate = NSTextField()
    private let refresh = NSPopUpButton()
    private let size = NSTextField()
    private let stats = NSButton(checkboxWithTitle: "Show stats overlay (Cmd+I)", target: nil, action: nil)
    private let immediate = NSButton(checkboxWithTitle: "Vsync off in full screen (may tear)", target: nil, action: nil)
    private let vivid = NSButton(checkboxWithTitle: "Vivid colors (display's full gamut, as on Windows)", target: nil, action: nil)
    private let awake = NSButton(checkboxWithTitle: "Keep Wi-Fi awake (60 tiny packets/s to the host)", target: nil, action: nil)
    private let streamFrames = NSButton(checkboxWithTitle: "Take AV1 frames as the host encodes them", target: nil, action: nil)
    private let cursorMode = NSPopUpButton()
    private let mouseMode = NSPopUpButton()
    private let confinement = NSPopUpButton()
    private let pacing = NSButton(checkboxWithTitle: "Pace the host to this display's refreshes (phase lock)", target: nil, action: nil)
    private let hold = NSButton(checkboxWithTitle: "Even out network jitter (fixed delay after the host)", target: nil, action: nil)
    private let pacingMode = NSPopUpButton()
    private let vrrMode = NSPopUpButton()
    private let copyKey = NSButton(title: "Copy line for ~/.ssh/authorized_keys", target: nil, action: nil)
    private let forgetHost = NSButton(title: "Forget host's key", target: nil, action: nil)
    private let pacingNote = NSTextField(wrappingLabelWithString: "")
    private static let refreshRates = [30, 60, 90, 120, 144, 165, 240]
    private static let codecs: [(name: String, id: UInt8)] =
        [("H.264", 0), ("HEVC", 1)] + (Decoder.hasAV1 ? [("AV1", 2)] : []) + [("PyroWave", 3)]

    override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 420, height: 320),
                          styleMask: [.titled, .closable], backing: .buffered, defer: true)
        super.init()
        window.title = "GMStream Settings"
        window.isReleasedWhenClosed = false

        copyKey.target = self
        copyKey.action = #selector(copyDeviceKey)
        forgetHost.target = self
        forgetHost.action = #selector(forgetHostKey)
        codec.addItems(withTitles: Self.codecs.map(\.name))
        cursorMode.addItems(withTitles: CursorMode.allCases.map(\.title))
        mouseMode.addItems(withTitles: MouseMode.allCases.map(\.title))
        confinement.addItems(withTitles: Confinement.allCases.map(\.title))
        pacingMode.addItems(withTitles: PacingMode.allCases.map(\.title))
        vrrMode.addItems(withTitles: VRRMode.allCases.map(\.title))
        pacingNote.textColor = .secondaryLabelColor
        pacingNote.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        pacingNote.preferredMaxLayoutWidth = 300
        refresh.addItem(withTitle: "Match display")
        refresh.addItems(withTitles: Self.refreshRates.map { "\($0) Hz" })
        size.placeholderString = "window size, e.g. 2560x1600"
        for field in [host, port, bitrate, size] {
            field.target = self
            field.action = #selector(changed(_:))
        }
        for control in [codec, refresh, stats, immediate, pacingMode, vrrMode, pacing, hold, vivid, awake, streamFrames, cursorMode, mouseMode, confinement] as [NSControl] {
            control.target = self
            control.action = #selector(changed(_:))
        }
        let row = { (label: String, control: NSView, note: String?) -> [NSView] in
            let l = NSTextField(labelWithString: label)
            l.alignment = .right
            guard let note else { return [l, control] }
            let n = NSTextField(labelWithString: note)
            n.textColor = .secondaryLabelColor
            n.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            let stack = NSStackView(views: [control, n])
            stack.orientation = .vertical
            stack.alignment = .leading
            stack.spacing = 2
            return [l, stack]
        }
        let grid = NSGridView(views: [
            row("Host:", host, nil),
            row("Port:", port, "Plain UDP; gmstream listens on 8444"),
            row("Codec:", codec, nil),
            row("Bitrate (Mb/s):", bitrate, nil),
            row("Refresh rate:", refresh, "The host's frame-rate cap"),
            row("Fixed size:", size, "Empty: follow the window"),
            [NSGridCell.emptyContentView, stats],
            [NSTextField(labelWithString: "Pacing:"), pacingMode],
            [NSGridCell.emptyContentView, pacingNote],
            [NSGridCell.emptyContentView, pacing],
            [NSGridCell.emptyContentView, hold],
            [NSGridCell.emptyContentView, immediate],
            row("VRR:", vrrMode, nil),
            [NSGridCell.emptyContentView, vivid],
            [NSGridCell.emptyContentView, awake],
            [NSGridCell.emptyContentView, streamFrames],
            row("Cursor:", cursorMode, nil),
            row("Mouse:", mouseMode, nil),
            row("Confinement:", confinement, nil),
            [NSTextField(labelWithString: "This device:"), copyKey],
            [NSGridCell.emptyContentView, forgetHost],
        ])
        grid.rowSpacing = 10
        grid.columnSpacing = 8
        grid.column(at: 0).xPlacement = .trailing
        grid.rowAlignment = .firstBaseline
        for f in [host, size] { f.widthAnchor.constraint(equalToConstant: 240).isActive = true }
        for f in [port, bitrate] { f.widthAnchor.constraint(equalToConstant: 80).isActive = true }
        grid.translatesAutoresizingMaskIntoConstraints = false
        let content = NSView()
        content.addSubview(grid)
        NSLayoutConstraint.activate([
            grid.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            grid.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -20),
            grid.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
            grid.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -20),
        ])
        window.contentView = content
    }

    func show() {
        load()
        if !window.isVisible { window.center() }
        window.makeKeyAndOrderFront(nil)
    }

    /// Shows the current values (also after Cmd+I toggles the overlay).
    func load() {
        host.stringValue = options.host
        port.stringValue = String(options.port)
        codec.selectItem(at: Self.codecs.firstIndex { $0.id == options.codec } ?? 0)
        bitrate.stringValue = String(options.mbps)
        if let r = options.refresh, let i = Self.refreshRates.firstIndex(of: Int(r)) {
            refresh.selectItem(at: i + 1)
        } else {
            refresh.selectItem(at: 0)
        }
        size.stringValue = options.sizeText
        stats.state = options.showStats ? .on : .off
        vivid.state = options.vividColors ? .on : .off
        awake.state = options.keepWiFiAwake ? .on : .off
        streamFrames.state = options.streamFrames ? .on : .off
        cursorMode.selectItem(at: options.cursorMode.rawValue)
        mouseMode.selectItem(at: options.mouseMode.rawValue)
        confinement.selectItem(at: options.confinement.rawValue)
        let now = pacingNow()
        pacingMode.selectItem(at: options.pacingMode.rawValue)
        pacing.state = now.phaseLock ? .on : .off
        hold.state = now.hold ? .on : .off
        immediate.state = now.vsyncOff ? .on : .off
        vrrMode.selectItem(at: now.vrr.rawValue)
        for control in [pacing, hold, immediate, vrrMode] as [NSControl] { control.isEnabled = options.pacingMode == .manual }
        pacingNote.stringValue = now.note
    }

    @objc private func copyDeviceKey() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(Identity.shared.authorizedLine, forType: .string)
    }

    @objc private func forgetHostKey() {
        UserDefaults.standard.removeObject(forKey: "hostKeys")
        onChange(.connection)
    }

    @objc private func changed(_ sender: NSControl) {
        let change: Change
        switch sender {
        case host, port, codec:
            let h = host.stringValue.trimmingCharacters(in: .whitespaces)
            let p = UInt16(port.stringValue) ?? options.port
            let c = Self.codecs[max(0, codec.indexOfSelectedItem)].id
            guard !h.isEmpty, h != options.host || p != options.port || c != options.codec else { return load() }
            options.host = h
            options.port = p
            options.codec = c
            change = .connection
        case bitrate:
            guard let m = UInt32(bitrate.stringValue), (1...500).contains(m) else { return load() }
            guard m != options.mbps else { return }
            options.mbps = m
            change = .bitrate
        case refresh:
            let i = refresh.indexOfSelectedItem
            options.refresh = i > 0 ? Double(Self.refreshRates[i - 1]) : nil
            change = .display
        case size:
            let text = size.stringValue.trimmingCharacters(in: .whitespaces)
            let s = Options.parseSize(text)
            if !text.isEmpty && s == nil { return load() }
            options.size = s
            change = .display
        case immediate:
            options.immediateFullScreen = immediate.state == .on
            change = .present
        case vivid:
            options.vividColors = vivid.state == .on
            change = .present
        case pacing:
            options.phaseLock = pacing.state == .on
            change = .phaseLock
        case hold:
            options.holdFrames = hold.state == .on
            change = .phaseLock
        case pacingMode:
            options.pacingMode = PacingMode(rawValue: pacingMode.indexOfSelectedItem) ?? .auto
            change = .phaseLock
        case vrrMode:
            options.vrrMode = VRRMode(rawValue: vrrMode.indexOfSelectedItem) ?? .auto
            change = .phaseLock
        case awake:
            options.keepWiFiAwake = awake.state == .on
            change = .keepAwake
        case streamFrames:
            // The host is told in Hello, so the stream starts over.
            options.streamFrames = streamFrames.state == .on
            change = .connection
        case cursorMode:
            options.cursorMode = CursorMode(rawValue: cursorMode.indexOfSelectedItem) ?? .auto
            change = .cursor
        case mouseMode:
            options.mouseMode = MouseMode(rawValue: mouseMode.indexOfSelectedItem) ?? .auto
            change = .cursor
        case confinement:
            options.confinement = Confinement(rawValue: confinement.indexOfSelectedItem) ?? .auto
            change = .cursor
        default:
            options.showStats = stats.state == .on
            change = .stats
        }
        options.save()
        load()
        onChange(change)
    }
}
