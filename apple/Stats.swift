// Per-second statistics and which received frames get decoded.

import Foundation

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
