// Measuring (GM_DEBUG_MOUSE=<file>): mouse to pixel latency with
// tools/mockgame. Moves the pointer a little a few times a second and watches
// decoded frames for the game's strip to flip. Needs H.264, HEVC or AV1.
//
// One line per step, all in this Mac's ns:
//   sent presentLocal received decoded presented (presentHost - presentLocal)
//
// With GM_DEBUG_MOUSE_SWEEP=<widths per second> instead, the shell sweeps the
// real pointer left and right at that speed, and each frame's orange block is
// matched to when the pointer was at that spot: the gap between the local
// cursor and the block, in time. Same columns; `sent` is that moment.
//
// With GM_DEBUG_MOUSE_GRID, for a game with no strip: "s sent" per step and
// "f presentLocal decoded presented hostOffset cell:change ..." per frame,
// the luma change since the last frame in 64x40 cells, to find the cursor
// afterwards. GM_DEBUG_MOUSE_STEP sets the step, in stream pixels.

import CoreVideo
import Foundation

final class MouseProbe {
    private let out: FileHandle
    private let lock = NSLock()
    private var sent: UInt64?              // waiting for the flip
    private var bright: Bool?
    private var result: (UInt64, FrameTiming)? // logged once it has been presented
    private var started = false
    private let speed = getenv("GM_DEBUG_MOUSE_SWEEP").flatMap { Double(String(cString: $0)) }
    private var path: [(t: UInt64, x: Double)] = [] // where the pointer was, last second
    private var blockX: Double?
    private var matched: [(UInt64, FrameTiming)] = []
    private var timer: DispatchSourceTimer?
    private let grid = getenv("GM_DEBUG_MOUSE_GRID") != nil
    private let size = getenv("GM_DEBUG_MOUSE_STEP").flatMap { Double(String(cString: $0)) } ?? 8
    private var previous: [UInt8] = []
    private var frames: [(FrameTiming, String)] = []

    init?() {
        guard let path = getenv("GM_DEBUG_MOUSE").map({ String(cString: $0) }),
              FileManager.default.createFile(atPath: path, contents: nil),
              let out = FileHandle(forWritingAtPath: path) else { return nil }
        self.out = out
    }

    /// `move`: moves the pointer by dx, on a background queue.
    func start(move: @escaping (Double) -> Void) {
        guard !started, speed == nil else { return }
        started = true
        step(sign: 1, move: move)
    }

    /// `place`: puts the pointer at x, 0...1 across the video, mid height.
    func sweep(how: String, place: @escaping (Double) -> Void) {
        guard let speed, timer == nil else { return }
        out.write("# \(how)\n".data(using: .utf8)!)
        let begin = nowNs()
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now(), repeating: .milliseconds(4))
        t.setEventHandler { [weak self] in
            guard let self else { return }
            let now = nowNs()
            let d = Double(now - begin) / 1e9 * speed
            let leg = (d / 0.7).truncatingRemainder(dividingBy: 2) // 0.15...0.85 and back
            let x = 0.15 + 0.7 * (leg < 1 ? leg : 2 - leg)
            self.lock.lock()
            self.path.append((now, x))
            self.path.removeAll { now - $0.t > 1_000_000_000 }
            self.flush()
            self.lock.unlock()
            place(x)
        }
        timer = t
        t.resume()
    }

    private func flush() {
        for (t, f) in matched where f.presented != 0 {
            let host = Int64(truncatingIfNeeded: f.presentHost) &- Int64(truncatingIfNeeded: f.presentLocal ?? 0)
            out.write("\(t) \(f.presentLocal ?? 0) \(f.received) \(f.decoded) \(f.presented) \(host)\n".data(using: .utf8)!)
        }
        matched.removeAll { $0.1.presented != 0 }
    }

    private func cells(_ luma: UnsafePointer<UInt8>, _ w: Int, _ h: Int, _ stride: Int, _ timing: FrameTiming) {
        let (cols, rows, step) = (64, 40, 3)
        var now = [UInt8](); now.reserveCapacity(w * h / (step * step))
        var sums = [Int](repeating: 0, count: cols * rows), n = 0
        lock.lock(); defer { lock.unlock() }
        let have = previous.count == (h / step + (h % step > 0 ? 1 : 0)) * (w / step + (w % step > 0 ? 1 : 0))
        for y in Swift.stride(from: 0, to: h, by: step) {
            let row = luma + y * stride
            for x in Swift.stride(from: 0, to: w, by: step) {
                now.append(row[x])
                if have { sums[y * rows / h * cols + x * cols / w] += abs(Int(row[x]) - Int(previous[n])) }
                n += 1
            }
        }
        previous = now
        let area = (w / cols / step) * (h / rows / step)
        let changed = sums.enumerated().compactMap { $1 / max(area, 1) >= 2 ? "\($0):\($1 / area)" : nil }
        frames.append((timing, changed.joined(separator: " ")))
        for (f, line) in frames where f.presented != 0 {
            let host = Int64(truncatingIfNeeded: f.presentHost) &- Int64(truncatingIfNeeded: f.presentLocal ?? 0)
            out.write("f \(f.presentLocal ?? 0) \(f.decoded) \(f.presented) \(host) \(line)\n".data(using: .utf8)!)
        }
        frames.removeAll { $0.0.presented != 0 }
    }

    /// The block's centre on the row the pointer sweeps along, 0...1.
    private func block(_ row: UnsafePointer<UInt8>, width: Int) -> Double? {
        var first = -1, last = -1
        for x in 0..<width where row[x] > 110 {
            if first < 0 { first = x }
            last = x
        }
        return first < 0 ? nil : (Double(first + last) / 2 + 0.5) / Double(width)
    }

    /// The latest moment the pointer crossed `x` going the same way.
    private func crossing(_ x: Double, rising: Bool) -> UInt64? {
        guard path.count > 1 else { return nil }
        for i in stride(from: path.count - 1, to: 0, by: -1) {
            let (a, b) = (path[i - 1], path[i])
            guard (b.x > a.x) == rising, min(a.x, b.x) <= x, x <= max(a.x, b.x), a.x != b.x else { continue }
            return a.t + UInt64(Double(b.t - a.t) * (x - a.x) / (b.x - a.x))
        }
        return nil
    }

    private func step(sign: Double, move: @escaping (Double) -> Void) {
        lock.lock()
        if let (t, f) = result, f.presented != 0 {
            let host = Int64(truncatingIfNeeded: f.presentHost) &- Int64(truncatingIfNeeded: f.presentLocal ?? 0)
            out.write("\(t) \(f.presentLocal ?? 0) \(f.received) \(f.decoded) \(f.presented) \(host)\n".data(using: .utf8)!)
        }
        result = nil
        sent = nowNs()
        if grid { out.write("s \(sent ?? 0)\n".data(using: .utf8)!) }
        lock.unlock()
        move(size * sign)
        // Off any 60 Hz beat, so steps land all over the frame. Not on the main
        // queue: full screen, its timers fall in step with the frames, and
        // every step would then sample one phase of the frame.
        DispatchQueue.global(qos: .userInteractive).asyncAfter(deadline: .now() + .microseconds(Int.random(in: 300_000...500_000))) { [weak self] in
            self?.step(sign: -sign, move: move)
        }
    }

    /// On the decoder's thread: reads the strip, near the top in the middle.
    func decoded(_ image: CVPixelBuffer, _ timing: FrameTiming) {
        guard CVPixelBufferGetPlaneCount(image) > 1 else { return }
        CVPixelBufferLockBaseAddress(image, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(image, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(image, 0) else { return }
        if grid {
            cells(base.assumingMemoryBound(to: UInt8.self), CVPixelBufferGetWidthOfPlane(image, 0),
                  CVPixelBufferGetHeightOfPlane(image, 0), CVPixelBufferGetBytesPerRowOfPlane(image, 0), timing)
            return
        }
        if speed != nil {
            let y = CVPixelBufferGetHeightOfPlane(image, 0) / 2
            let row = base.advanced(by: y * CVPixelBufferGetBytesPerRowOfPlane(image, 0)).assumingMemoryBound(to: UInt8.self)
            guard let u = block(row, width: CVPixelBufferGetWidthOfPlane(image, 0)) else { return }
            lock.lock(); defer { lock.unlock() }
            if let prev = blockX, abs(u - prev) > 0.0005, u > 0.25, u < 0.75, let t = crossing(u, rising: u > prev) {
                matched.append((t, timing))
            }
            blockX = u
            return
        }
        let y = CVPixelBufferGetHeightOfPlane(image, 0) / 16, x = CVPixelBufferGetWidthOfPlane(image, 0) / 2
        let row = base.advanced(by: y * CVPixelBufferGetBytesPerRowOfPlane(image, 0)).assumingMemoryBound(to: UInt8.self)
        let now = (0..<16).reduce(0) { $0 + Int(row[x + $1]) } / 16 > 128
        lock.lock(); defer { lock.unlock() }
        guard now != bright else { return }
        let first = bright == nil
        bright = now
        if !first, let s = sent {
            result = (s, timing)
            sent = nil
        }
    }
}
