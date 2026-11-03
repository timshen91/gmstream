// Per-stage latency for frames that reached the screen, and the overlay's
// chart of the last 10 s (the same chart as the web client's, but ending on
// the screen rather than at the canvas draw).

#if os(macOS)
import AppKit
typealias PlatformColor = NSColor
typealias PlatformFont = NSFont
#else
import UIKit
typealias PlatformColor = UIColor
typealias PlatformFont = UIFont
#endif

/// Runs `body` with `g` as the current context for AppKit's or UIKit's text
/// drawing; `g` is already flipped (y down).
func withTextContext(_ g: CGContext, _ body: () -> Void) {
    #if os(macOS)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(cgContext: g, flipped: true)
    body()
    NSGraphicsContext.restoreGraphicsState()
    #else
    UIGraphicsPushContext(g)
    body()
    UIGraphicsPopContext()
    #endif
}

/// The OS that shows the frames, for the stats.
let systemName: String = {
    #if os(macOS)
    return "macOS"
    #else
    return "iPadOS"
    #endif
}()

/// p50, p99 and max per stage over the last 5 s (300 frames at 60 fps, 600
/// at 120: p99 is the 3rd or 6th worst). The target to optimize is the
/// total's p99: one frame in ten is too many to leave out.
final class StageStats {
    // "to macOS" (or iPadOS): gamescope present → handed to the system (total − display).
    // "spacing": per frame, |interval between presentations − interval
    // between gamescope presents|; 0 keeps the host's frame spacing exactly.
    // It measures the result whatever the pacing mode, phase lock or not.
    // "network": from the host sending the frame's first byte to its last
    // byte arriving here, so it is the whole of the network's share however
    // the frame was sent.
    // "read": from the last byte arriving in the kernel to the frame being
    // handed to the decoder — this client's own lag, which used to hide
    // inside "network" because the frame was stamped when we got round to
    // it rather than when it arrived.
    // "overlap": the part of that which happened while the frame was still
    // being encoded, as a credit — a frame sent in pieces is already on the
    // wire before the encoder has finished, and that time is counted once,
    // in "encode". It is 0 for a frame sent whole, which cannot start
    // arriving until it is finished. The column adds up to "to \(systemName)".
    static let stages = ["encode", "network", "overlap", "read", "decode", "hold", "render", "to \(systemName)", "display", "total", "spacing"]
    static let window: UInt64 = 5_000_000_000

    struct Row {
        let name: String
        let p50: Double?, p99: Double?, max: Double?
    }

    private let lock = NSLock()
    private var frames: [(at: UInt64, values: [Double?])] = []
    private var shown = 0, skipped = 0, dropped = 0
    private var lastPresented: UInt64?, lastPresent: UInt64?

    /// A frame on screen; stages it cannot know (no clock offset yet) are
    /// left out.
    func add(_ t: FrameTiming) {
        let ms = { (a: UInt64?, b: UInt64) -> Double? in a.map { Double(Int64(b) - Int64($0)) / 1e6 } }
        let overlap = Double(t.overlapNs) / 1e6
        var values: [Double?] = [
            Double(t.encodeNs) / 1e6,
            // From the first byte out, which is `overlap` before the encode
            // ended, to the last byte in.
            ms(t.encodedLocal, t.received).map { $0 + overlap },
            -overlap,
            t.decodeStart > 0 ? ms(t.received, t.decodeStart) : nil,
            ms(t.decodeStart > 0 ? t.decodeStart : t.received, t.decoded),
            ms(t.decoded, t.released),
            ms(t.released, t.submitted),
            ms(t.presentLocal, t.submitted),
            ms(t.submitted, t.presented),
            ms(t.presentLocal, t.presented),
            nil,
        ]
        lock.lock(); defer { lock.unlock() }
        if let lp = lastPresented, let lc = lastPresent, let c = t.presentLocal, c > lc, c - lc < 100_000_000 {
            values[Self.stages.count - 1] = abs(Double(Int64(t.presented) - Int64(lp)) - Double(c - lc)) / 1e6
        }
        lastPresented = t.presented
        lastPresent = t.presentLocal
        shown += 1
        frames.append((t.presented, values))
    }

    func skip() { lock.lock(); skipped += 1; lock.unlock() }
    func drop() { lock.lock(); dropped += 1; lock.unlock() }

    /// Percentile rows over the window; frames shown and skipped in the last
    /// second (those reset).
    func take() -> (rows: [Row], shown: Int, skipped: Int, dropped: Int) {
        lock.lock(); defer { lock.unlock() }
        let cutoff = nowNs() &- Self.window
        frames.removeAll { $0.at < cutoff }
        let rows = Self.stages.enumerated().map { i, name -> Row in
            let s = frames.compactMap { $0.values[i] }.sorted()
            let pct = { (p: Double) in s.isEmpty ? nil : s[min(s.count - 1, Int(Double(s.count) * p))] }
            return Row(name: name, p50: pct(0.5), p99: pct(0.99), max: s.last)
        }
        let r = (rows, shown, skipped, dropped)
        shown = 0; skipped = 0; dropped = 0
        return r
    }
}

/// Latency per frame, last 10 s: gamescope present → handed to macOS as a
/// line, with macOS's display stage stacked above it up to the total. The
/// scale grows in steps to fit spikes (never below 20 ms); pauses in the
/// stream (a static screen sends nothing) break the line.
final class LatencyChart {
    private let window_: Double = 10, gap: Double = 0.25
    private let lock = NSLock()
    private var points: [(t: Double, base: Double, ms: Double)] = []

    /// `base`: to macOS; `ms`: total (base + display).
    func push(base: Double, ms: Double) {
        let t = CACurrentMediaTime()
        lock.lock()
        points.append((t, base, ms))
        while let first = points.first, first.t < t - window_ { points.removeFirst() }
        lock.unlock()
    }

    /// Draws into `g` (y down, the current text context) at the origin.
    func draw(in g: CGContext, width W: Double, height H: Double) {
        let now = CACurrentMediaTime()
        lock.lock()
        let pts = points.filter { $0.t >= now - window_ }
        lock.unlock()
        let peak = pts.map(\.ms).max() ?? 0
        let top = [20.0, 40, 80, 160, 320, 640, 1280].first { $0 >= peak } ?? (peak / 1000).rounded(.up) * 1000
        let y = { (ms: Double) in H - 1 - min(ms, top) / top * (H - 12) }
        let x = { (t: Double) in (t - (now - self.window_)) / self.window_ * W }

        // Recessive grid: the top of the scale and its midpoint, labelled.
        g.setStrokeColor(PlatformColor(white: 1, alpha: 0.13).cgColor)
        g.setLineWidth(1)
        for v in [top, top / 2] {
            g.move(to: CGPoint(x: 0, y: y(v).rounded() + 0.5))
            g.addLine(to: CGPoint(x: W, y: y(v).rounded() + 0.5))
        }
        g.strokePath()
        let attrs: [NSAttributedString.Key: Any] = [
            .font: PlatformFont.monospacedSystemFont(ofSize: 10, weight: .regular),
            .foregroundColor: PlatformColor(red: 0xc3 / 255.0, green: 0xc2 / 255.0, blue: 0xb7 / 255.0, alpha: 1),
        ]
        ("\(Int(top)) ms" as NSString).draw(at: CGPoint(x: 2, y: -1), withAttributes: attrs)
        ("\(Int(top / 2))" as NSString).draw(at: CGPoint(x: 2, y: y(top / 2) - 13), withAttributes: attrs)
        if !pts.isEmpty {
            let label = String(format: "max %.1f ms", peak) as NSString
            label.draw(at: CGPoint(x: W - label.size(withAttributes: attrs).width - 2, y: -1), withAttributes: attrs)
        }

        // Unbroken runs of points; each is drawn as the display band (base to
        // total, filled) under the to-macOS line.
        var runs: [[(t: Double, base: Double, ms: Double)]] = []
        var last = -Double.infinity
        for p in pts {
            if p.t - last > gap { runs.append([]) }
            runs[runs.count - 1].append(p)
            last = p.t
        }
        g.setFillColor(PlatformColor(red: 0xe8 / 255.0, green: 0xa3 / 255.0, blue: 0x3d / 255.0, alpha: 0.55).cgColor)
        for run in runs where run.count > 1 {
            g.move(to: CGPoint(x: x(run[0].t), y: y(run[0].base)))
            for p in run.dropFirst() { g.addLine(to: CGPoint(x: x(p.t), y: y(p.base))) }
            for p in run.reversed() { g.addLine(to: CGPoint(x: x(p.t), y: y(p.ms))) }
            g.closePath()
        }
        g.fillPath()
        g.setStrokeColor(PlatformColor(red: 0x39 / 255.0, green: 0x87 / 255.0, blue: 0xe5 / 255.0, alpha: 1).cgColor)
        g.setLineWidth(1.5)
        g.setLineJoin(.round)
        for run in runs {
            g.move(to: CGPoint(x: x(run[0].t), y: y(run[0].base)))
            for p in run.dropFirst() { g.addLine(to: CGPoint(x: x(p.t), y: y(p.base))) }
        }
        g.strokePath()
    }
}

/// The stats overlay as an image: text, then the latency chart, on a
/// translucent box, `scale` pixels per point. It is drawn into the video
/// frame (MetalRenderer.setOverlay), not as views over it: a view that
/// changes makes macOS composite the window for that frame instead of
/// flipping it straight to the display. Measured full screen on a VRR
/// display, with the chart redrawn 10 times a second: display stage p90
/// 6.9 ms as views, 3.6 without.
func renderOverlay(text: String, chart: LatencyChart, scale: Double) -> CGContext? {
    let attrs: [NSAttributedString.Key: Any] = [
        .font: PlatformFont.monospacedSystemFont(ofSize: 11, weight: .regular),
        .foregroundColor: PlatformColor(red: 0.6, green: 1, blue: 0.6, alpha: 1),
    ]
    let str = NSAttributedString(string: text, attributes: attrs)
    let tb = str.boundingRect(with: CGSize(width: 4000, height: 4000), options: [.usesLineFragmentOrigin], context: nil)
    let chartW = 240.0, chartH = 56.0, padX = 8.0, top = 4.0, gap = 4.0, bottom = 6.0
    let w = max(tb.width, chartW) + 2 * padX, h = top + tb.height + gap + chartH + bottom
    let pw = Int((w * scale).rounded(.up)), ph = Int((h * scale).rounded(.up))
    guard let g = CGContext(data: nil, width: pw, height: ph, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
    else { return nil }
    g.translateBy(x: 0, y: Double(ph))
    g.scaleBy(x: scale, y: -scale)
    withTextContext(g) {
        g.setFillColor(PlatformColor(white: 0, alpha: 0.65).cgColor)
        g.addPath(CGPath(roundedRect: CGRect(x: 0, y: 0, width: w, height: h), cornerWidth: 4, cornerHeight: 4, transform: nil))
        g.fillPath()
        str.draw(with: CGRect(x: padX, y: top, width: tb.width, height: tb.height), options: [.usesLineFragmentOrigin], context: nil)
        g.saveGState()
        g.translateBy(x: padX, y: top + tb.height + gap)
        chart.draw(in: g, width: chartW, height: chartH)
        g.restoreGState()
    }
    return g
}
