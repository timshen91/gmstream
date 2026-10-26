// Phase lock: lines the host's frame timing up with this display's refreshes.
//
// With vsync, macOS shows a frame at the first refresh it can still make,
// plus a fixed queue depth; a frame that finishes decoding just after that
// cut-off waits almost a whole refresh more. We send gmstream this display's
// refresh grid (in the host's clock) shifted by `shift`; gmstream reports it
// to gamescope as its display's vblanks, and gamescope's own just-in-time
// scheduler then produces frames on that grid.
//
// To find the best shift, sweep it through one whole refresh (the phase is
// circular, so the cut-off is somewhere in it), recording the median
// `display` stage (decoded → on screen) at each step; then hold at the best
// point moved `margin` earlier, away from the cut-off. After that a servo
// holds the stage at its floor plus the margin: the two machines' clocks
// drift (tens of ppm), which would otherwise carry frames past the cut-off
// within a minute.

import Foundation

final class PhaseLock {
    private let lock = NSLock()
    private var nominal: Double            // refresh interval from the screen, ns
    private var first: UInt64?, last: UInt64? // presented refreshes
    private var samples: [Double] = []     // display stage, ns, since the last step
    private var since: UInt64 = 0          // ignore frames presented before this
    private(set) var shift = 0.0           // ns earlier than the display grid
    private var sweep: [(shift: Double, display: Double)] = []
    private var sweeping = true
    private var lockedBest: Double?        // the display stage's floor, from the sweep
    private var steps = 0
    // Distance kept from the cut-off. A frame that crosses it is dropped
    // (disableFIFO; with FIFO it would deepen the queue for good). A late
    // frame waits less, so the spread of the display stage over the last 10 s
    // (median minus 0.5th percentile) is how far the latest frames run behind:
    // the margin covers that plus 0.5 ms, and each drop adds 0.5 ms.
    private var margin = 3e6
    private var drops = 0
    private var recent: [Double] = []      // display stage, last ~10 s

    static let sweepStep = 1e6, settleSteps = 3, minMargin = 1e6

    // Vsync off (full screen): macOS flips frames straight to the panel, so
    // there is no refresh cut-off to steer around (a sweep is flat, and where
    // flips land does not depend on the phase). The lock then only keeps
    // gamescope on the panel's grid, from `vsync`, with the shift held.
    private let vsync: VsyncClock
    private var immediate = false

    init(refreshHz: Double, vsync: VsyncClock) {
        nominal = 1e9 / refreshHz
        self.vsync = vsync
    }

    /// Switches between the two modes; back to vsync, it sweeps again.
    func setImmediate(_ on: Bool) {
        lock.lock(); defer { lock.unlock() }
        guard on != immediate else { return }
        immediate = on
        if !on {
            sweeping = true
            sweep.removeAll()
            lockedBest = nil
            recent.removeAll()
            steps = 0
        }
    }

    /// Starts over with a sweep (e.g. after a stretch without a grid, or on
    /// another screen: `refreshHz` is then its refresh rate).
    func restart(refreshHz: Double? = nil) {
        lock.lock(); defer { lock.unlock() }
        if let refreshHz {
            nominal = 1e9 / refreshHz
            first = nil
            last = nil
        }
        sweeping = true
        sweep.removeAll()
        lockedBest = nil
        recent.removeAll()
        steps = 0
        samples.removeAll()
    }

    /// A frame macOS never showed (any thread).
    func dropped() { lock.lock(); drops += 1; lock.unlock() }

    /// A frame reached the screen (any thread).
    func presented(_ t: FrameTiming) {
        lock.lock(); defer { lock.unlock() }
        if first == nil { first = t.presented }
        last = t.presented
        if t.presented > since && t.submitted > 0 {
            samples.append(Double(Int64(t.presented) - Int64(t.submitted)))
        }
    }

    /// The refresh interval, measured once enough refreshes have passed.
    private var interval: Double {
        guard let first, let last else { return nominal }
        let n = (Double(last - first) / nominal).rounded()
        return n >= 120 ? Double(last - first) / n : nominal
    }

    /// Every half second: adjusts `shift`, returns the grid to send (a recent
    /// refresh in this Mac's clock, shifted, and the interval).
    func step() -> (anchor: UInt64, interval: UInt64)? {
        lock.lock(); defer { lock.unlock() }
        guard let last else { return nil }
        if immediate, let g = vsync.grid() {
            return (UInt64(Int64(g.vsync) - Int64(shift)), UInt64(g.interval))
        }
        let t = interval
        let d = samples.sorted()
        samples.removeAll()
        defer { drops = 0 }
        steps += 1
        // Settle first (gamescope switching from VRR to the grid), and skip
        // steps with too few frames (a static screen sends none).
        if steps > Self.settleSteps && d.count >= 10 {
            let display = d[d.count / 2]
            if sweeping {
                sweep.append((shift, display))
                if Double(sweep.count) * Self.sweepStep >= t {
                    let best = sweep.min { $0.display < $1.display }!
                    if getenv("GM_DEBUG_PHASE") != nil {
                        let table = sweep.map { String(format: "%+.0f:%.1f", $0.shift / 1e6, $0.display / 1e6) }.joined(separator: " ")
                        FileHandle.standardError.write("sweep \(table)\n".data(using: .utf8)!)
                    }
                    shift = best.shift + margin
                    lockedBest = best.display - 0.5e6 // the cut-off is within a step
                    sweeping = false
                } else {
                    shift -= Self.sweepStep // frames later
                }
            } else if let floor = lockedBest {
                recent += d
                if recent.count > 600 { recent.removeFirst(recent.count - 600) }
                if recent.count >= 120 {
                    let r = recent.sorted()
                    let spread = r[r.count / 2] - r[r.count / 200]
                    margin = 0.8 * margin + 0.2 * (spread + 0.5e6)
                }
                margin = min(t / 2, max(Self.minMargin, margin + Double(drops) * 0.5e6))
                // Hold the stage at floor + margin, following the drift between
                // the two clocks. About a refresh above it, frames drifted past
                // the cut-off: move them earlier by the overshoot.
                let target = floor + margin
                let error = display - target
                if error > t / 2 {
                    shift += t - error
                } else {
                    shift -= error / 2
                }
                if display < floor - 1e6 {
                    lockedBest = display // the floor is lower than the sweep saw
                }
                if getenv("GM_DEBUG_PHASE") != nil {
                    FileHandle.standardError.write(String(format: "locked shift %+.1f display %.1f (target %.1f, margin %.2f)\n",
                                                          shift / 1e6, display / 1e6, target / 1e6, margin / 1e6).data(using: .utf8)!)
                }
            }
            since = nowNs() + 100_000_000 // let the change settle
        }
        return (UInt64(Int64(last) - Int64(shift)), UInt64(t))
    }

    func summary() -> String {
        lock.lock(); defer { lock.unlock() }
        guard last != nil else { return "phase: waiting for the display" }
        if immediate {
            return String(format: "phase: immediate flips (vsync off), refresh %.3f ms", (vsync.grid()?.interval ?? 0) / 1e6)
        }
        let state = sweeping ? "sweeping \(sweep.count)/\(Int((interval / Self.sweepStep).rounded(.up)))" : "locked"
        return String(format: "phase: %@, shift %+.1f ms, margin %.2f ms, refresh %.3f ms", state, shift / 1e6, margin / 1e6, interval / 1e6)
    }
}
