// Phase lock: lines the host's frame timing up with this display's refreshes.
//
// With vsync, macOS shows a frame at the first refresh it can still make,
// plus a fixed queue depth; a frame that finishes decoding just after that
// cut-off waits almost a whole refresh more. We send gmstream this display's
// refresh grid (in the host's clock) shifted by `shift`; gmstream reports it
// to gamescope as its display's vblanks, and gamescope's own just-in-time
// scheduler then produces frames on that grid.
//
// Each frame is placed by its lead `a`: the time from its submit to the next
// point in the refresh where flips land (0..refresh). An on-time frame is
// shown a refresh after that point (display stage = a + refresh); one with a
// lead under the cut-off misses its refresh and is not shown later but
// replaced by the next frame (disableFIFO), so the screen stutters. The lead
// belongs to the display, not to `shift`: the cut-off is a fixed lead, learned
// from the leads of the frames that missed it (the second highest of the
// last 20 near-misses, so one odd drop does not move it). Frames that never
// show give no display stage, which is why the lead has to come from the
// submit time: a cut-off taken from the shown frames' display stage alone
// came out ~1.5 ms too optimistic and parked frames on the edge (~0.75
// drops/s).
//
// macOS can also start showing every frame a refresh late for good (the
// window server's queue one deeper after a hiccup), with nothing dropped and
// every lead fine. Moving frames does not undo that; showing one frame fewer
// does: the lock asks the renderer to skip one (`takeDrain`). One repeated
// frame instead of a refresh more latency from then on.
//
// To find a starting point, sweep `shift` through one whole refresh (the phase
// is circular, so the cut-off is somewhere in it), recording the median
// display stage at each step; lock at the lowest step without drops, moved
// `margin` earlier. A servo then holds the late tail (1st percentile) of the
// leads a guard above the cut-off. That also follows the drift between the
// two machines' clocks (tens of ppm), which would otherwise carry frames past
// the cut-off within a minute.

import Foundation

final class PhaseLock {
    private let lock = NSLock()
    private var nominal: Double            // refresh interval from the screen, ns
    private var first: UInt64?, last: UInt64? // presented refreshes
    private var samples: [Double] = []     // display stage, ns, since the last step
    private var leads: [(a: Double, ok: Bool)] = [] // since the last step; ok: not dropped
    private var lateShown = 0, shown = 0   // since the last step: shown a refresh late, all shown
    private var drain = false              // the renderer should skip one frame
    private var since: UInt64 = 0          // ignore frames submitted before this
    private(set) var shift = 0.0           // ns earlier than the display grid
    private var sweep: [(shift: Double, display: Double, safe: Bool)] = []
    private var sweeping = true
    private var steps = 0
    private var cutoff: Double?            // lead below which frames miss their refresh
    private var misses: [Double] = []      // recent near-misses' leads
    private var recent: [Double] = []      // leads, last ~10 s, as if at the current shift
    private var margin = 3e6               // median lead above the cut-off (after the sweep: start)
    private var drops = 0

    static let sweepStep = 1e6, settleSteps = 3, guardNs = 0.4e6, maxMisses = 20

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
        if !on { startSweep() }
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
        startSweep()
    }

    private func startSweep() {
        sweeping = true
        sweep.removeAll()
        cutoff = nil
        misses.removeAll()
        recent.removeAll()
        margin = 3e6
        steps = 0
        samples.removeAll()
        leads.removeAll()
    }

    /// The time from `submitted` to the next point in the refresh where flips
    /// land (`last` is a flip).
    private func lead(_ submitted: UInt64) -> Double? {
        guard let last else { return nil }
        let iv = interval
        let r = Double(Int64(last) - Int64(submitted)).truncatingRemainder(dividingBy: iv)
        return r < 0 ? r + iv : r
    }

    /// A frame macOS never showed (any thread).
    func dropped(_ t: FrameTiming) {
        lock.lock(); defer { lock.unlock() }
        drops += 1
        if t.submitted > since, let a = lead(t.submitted) { leads.append((a, false)) }
    }

    /// A frame reached the screen (any thread).
    func presented(_ t: FrameTiming) {
        lock.lock(); defer { lock.unlock() }
        if first == nil { first = t.presented }
        last = t.presented
        guard t.submitted > since, t.submitted > 0 else { return }
        let display = Double(Int64(t.presented) - Int64(t.submitted))
        samples.append(display)
        guard let a = lead(t.submitted) else { return }
        leads.append((a, true))
        shown += 1
        if display > a + interval * 1.5 { lateShown += 1 }
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
        let step = leads
        let (late, all) = (lateShown, shown)
        samples.removeAll()
        leads.removeAll()
        lateShown = 0
        shown = 0
        defer { drops = 0 }
        steps += 1
        // Settle first (gamescope switching from VRR to the grid), and skip
        // steps with too few frames (a static screen sends none).
        guard steps > Self.settleSteps && d.count >= 10 else { return (UInt64(Int64(last) - Int64(shift)), UInt64(t)) }
        let before = shift
        if sweeping {
            // A step whose frames straddle the cut-off shows only the lucky
            // ones (low display stage): not a place to lock.
            let display = d[d.count / 2]
            sweep.append((shift, display, drops * 30 < d.count))
            if Double(sweep.count) * Self.sweepStep >= t {
                let best = sweep.filter(\.safe).min { $0.display < $1.display } ?? sweep.min { $0.display < $1.display }!
                if getenv("GM_DEBUG_PHASE") != nil {
                    let table = sweep.map { String(format: "%+.0f:%.1f%@", $0.shift / 1e6, $0.display / 1e6, $0.safe ? "" : "!") }.joined(separator: " ")
                    FileHandle.standardError.write("sweep \(table)\n".data(using: .utf8)!)
                }
                shift = best.shift + margin
                // Its lead is ~display - refresh; the cut-off a little below
                // until misses show where it is.
                cutoff = max(0.3e6, best.display - t - 1e6)
                sweeping = false
            } else {
                shift -= Self.sweepStep // frames later
            }
        } else if all >= 10 && late * 5 > all * 4 {
            // Shown a refresh late across the board: drain, do not steer.
            drain = true
            recent.removeAll()
            if getenv("GM_DEBUG_PHASE") != nil {
                FileHandle.standardError.write("locked: \(late)/\(all) frames a refresh late, skipping one\n".data(using: .utf8)!)
            }
        } else if var cut = cutoff, !step.isEmpty {
            // Leads unwrapped around this step's median: a frame later than the
            // flip point reads as a small or negative lead, not as an early one.
            let mid = step.map(\.a).sorted()[step.count / 2]
            let unwrap = { (a: Double) -> Double in
                var x = (a - mid).truncatingRemainder(dividingBy: t)
                if x > t / 2 { x -= t } else if x <= -t / 2 { x += t }
                return mid + x
            }
            let a = step.map { (unwrap($0.a), $0.ok) }
            recent += a.map(\.0)
            if recent.count > 600 { recent.removeFirst(recent.count - 600) }
            // Dropped near-misses (not frames far behind for other reasons) set
            // the cut-off.
            let missed = a.filter { !$0.1 && $0.0 > 0 && $0.0 < mid }.map(\.0)
            misses += missed
            if misses.count > Self.maxMisses { misses.removeFirst(misses.count - Self.maxMisses) }
            let m = misses.sorted(by: >)
            if m.count >= 2 { cut = max(cut, m[1]) }
            cut = min(cut, t / 2)
            cutoff = cut
            // Hold the late tail (1st percentile over the last ~10 s; this
            // step's until there are enough) a guard above it.
            let r = (recent.count >= 120 ? recent : a.map(\.0)).sorted()
            let tail = r[r.count / 100]
            margin = mid - cut
            shift -= (tail - (cut + Self.guardNs)) / 2 // later lowers every lead
            if a.contains(where: { !$0.1 }) { shift += 0.25e6 } // react before the next step
            if getenv("GM_DEBUG_PHASE") != nil {
                FileHandle.standardError.write(String(format: "locked shift %+.1f lead p50 %.2f tail %.2f cut-off %.2f misses %d\n",
                                                      shift / 1e6, mid / 1e6, tail / 1e6, cut / 1e6, missed.count).data(using: .utf8)!)
            }
        }
        // Moving frames earlier lengthens every lead by as much.
        let moved = shift - before
        if moved != 0 { recent = recent.map { $0 + moved } }
        since = nowNs() + 100_000_000 // let the change settle
        return (UInt64(Int64(last) - Int64(shift)), UInt64(t))
    }

    /// Whether the renderer should skip one frame (once).
    func takeDrain() -> Bool {
        lock.lock(); defer { lock.unlock() }
        defer { drain = false }
        return drain
    }

    func summary() -> String {
        lock.lock(); defer { lock.unlock() }
        guard last != nil else { return "phase: waiting for the display" }
        if immediate {
            return String(format: "phase: immediate flips (vsync off), refresh %.3f ms", (vsync.grid()?.interval ?? 0) / 1e6)
        }
        let state = sweeping ? "sweeping \(sweep.count)/\(Int((interval / Self.sweepStep).rounded(.up)))" : "locked"
        return String(format: "phase: %@, shift %+.1f ms, margin %.2f ms, refresh %.3f ms", state, shift / 1e6, margin / 1e6, interval / 1e6)
            + (cutoff.map { String(format: ", cut-off %.2f ms", $0 / 1e6) } ?? "")
    }
}
