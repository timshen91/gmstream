// The Settings window (Cmd+,). Every change is saved and applied at once:
// bitrate live, refresh and size by reconfiguring the stream, host, port and
// codec by reconnecting.

import AppKit

final class SettingsWindow: NSObject {
    enum Change { case connection, bitrate, display, stats, present }

    let window: NSWindow
    /// Called after `options` has been updated and saved.
    var onChange: (Change) -> Void = { _ in }

    private let host = NSTextField()
    private let port = NSTextField()
    private let codec = NSPopUpButton()
    private let bitrate = NSTextField()
    private let refresh = NSPopUpButton()
    private let size = NSTextField()
    private let stats = NSButton(checkboxWithTitle: "Show stats overlay (Cmd+I)", target: nil, action: nil)
    private let immediate = NSButton(checkboxWithTitle: "Lowest latency in full screen (vsync off, may tear)", target: nil, action: nil)
    private let vivid = NSButton(checkboxWithTitle: "Vivid colors (display's full gamut, as on Windows)", target: nil, action: nil)
    private static let refreshRates = [30, 60, 90, 120, 144, 165, 240]
    private static let codecs: [(name: String, id: UInt8)] = [("H.264", 0), ("HEVC", 1), ("PyroWave", 3)]

    override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 420, height: 260),
                          styleMask: [.titled, .closable], backing: .buffered, defer: true)
        super.init()
        window.title = "GMStream Settings"
        window.isReleasedWhenClosed = false

        codec.addItems(withTitles: Self.codecs.map(\.name))
        refresh.addItem(withTitle: "Match display")
        refresh.addItems(withTitles: Self.refreshRates.map { "\($0) Hz" })
        size.placeholderString = "window size, e.g. 2560x1600"
        for field in [host, port, bitrate, size] {
            field.target = self
            field.action = #selector(changed(_:))
        }
        for control in [codec, refresh, stats, immediate, vivid] as [NSControl] {
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
            [NSGridCell.emptyContentView, immediate],
            [NSGridCell.emptyContentView, vivid],
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
        immediate.state = options.immediateFullScreen ? .on : .off
        vivid.state = options.vividColors ? .on : .off
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
        default:
            options.showStats = stats.state == .on
            change = .stats
        }
        options.save()
        load()
        onChange(change)
    }
}
