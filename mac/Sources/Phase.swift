// Phase lock: lines the host's frame timing up with this display's refreshes.
//
// With vsync, macOS shows a frame at the first refresh it can still make,
// plus a fixed queue depth; a frame that finishes decoding just after that
// cut-off waits almost a whole refresh more. We send gmstream this display's
// refresh grid (in the host's clock) shifted by `shift`; gmstream reports it
// to gamescope as its display's vblanks, and gamescope's own just-in-time
// scheduler then produces frames on that grid.
//
// Each frame's lead `a` is the time from its submit to the next point in the
// refresh where flips land (0..refresh). An on-time frame is shown a refresh
// after that point; a late one is replaced by the next frame (disableFIFO),
// and so is an early one whose successor comes in before the window server
// takes it: the screen stutters. Which leads make it is not a sharp cut-off
// (Dell at 120 Hz, in a window: about half the frames with leads under
// 1.5 ms dropped, 3% from 2.25 to 4.75 ms, ~60% from 5 ms up), so it is
// learned: per lead bin, the share of frames shown over the last ~25 s.
//
// The lock steers the host present's lead (from its present to the next
// flip point), the same for every frame up to the host's sub-ms jitter; a
// frame's own lead is that minus its lateness (submit − host present:
// network, decode, render). Latenesses have no wrap and do not move with
// the phase, so the lock picks the host lead under which the last ~10 s of
// them would have been shown most often, and moves toward it the short way
// round. (Submit leads alone are known only modulo the refresh; unwrapped
// around their median, every frame more than half a refresh late read as
// early, and at 120 Hz over Wi-Fi, lateness p50 9 / p99 17 ms, the lock went
// a whole refresh round every few seconds: 12 drops/s, total p90 25.5 ms.
// This way: 10.5 drops/s, the rest late arrivals, total p90 20.5 ms.)
//
// macOS can also start showing every frame a refresh late for good (the
// window server's queue one deeper after a hiccup), with nothing dropped and
// every lead fine. Moving frames does not undo that; showing one frame fewer
// does: the lock asks the renderer to skip one (`takeDrain`). One repeated
// frame instead of a refresh more latency from then on.
//
// To find a starting point, sweep `shift` through one whole refresh,
// recording the median display stage at each step, and start at the lowest
// step without drops, moved `margin` earlier; the sweep also fills the lead
// bins. Steering every step follows the drift between the two machines'
// clocks (tens of ppm), which would otherwise carry frames off within a
// minute.

import Foundation

final class PhaseLock {
    private let lock = NSLock()
    private var nominal: Double            // refresh interval from the screen, ns
    private var first: UInt64?, last: UInt64? // presented refreshes
    private var period: Double?            // measured refresh interval, ns
    private var previous: UInt64?          // the flip before `last`
    private var gaps: [Double] = []        // recent flip-to-flip gaps, per refresh
    private var samples: [Double] = []     // display stage, ns, since the last step
    // Since the last step; ok: not dropped; late: submit − the host's present
    // (this Mac's clock), once the clock offset is known.
    private var leads: [(a: Double, ok: Bool, late: Double?)] = []
    private var lateShown = 0, shown = 0   // since the last step: shown a refresh late, all shown
    private var drain = false              // the renderer should skip one frame
    private var since: UInt64 = 0          // ignore frames submitted before this
    private(set) var shift = 0.0           // ns earlier than the display grid
    private var sweep: [(shift: Double, display: Double, safe: Bool)] = []
    private var sweeping = true
    private var steps = 0
    private var target: Double?            // host present lead steered to; nil until locked
    private var expected = 0.0             // share of recent frames shown at `target`
    // Frames by submit lead in binNs bins: all, and dropped ones; decayed
    // every step (~25 s memory).
    private var binAll: [Double] = [], binDropped: [Double] = []
    static let binNs = 0.25e6
    private var recent: [Double] = []      // lateness (submit − host present), last ~10 s
    private var margin = 3e6               // after the sweep: start this much earlier than its best step
    private var drops = 0

    static let sweepStep = 1e6, settleSteps = 3

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
            previous = nil
            period = nil
            gaps = []
        }
        startSweep()
    }

    private func startSweep() {
        sweeping = true
        sweep.removeAll()
        target = nil
        binAll.removeAll()
        binDropped.removeAll()
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
        if t.submitted > since, let a = lead(t.submitted) { leads.append((a, false, Self.late(t))) }
    }

    /// A frame reached the screen (any thread).
    func presented(_ t: FrameTiming) {
        lock.lock(); defer { lock.unlock() }
        if first == nil { first = t.presented }
        if t.presented != last { previous = last; last = t.presented; measure() }
        guard t.submitted > since, t.submitted > 0 else { return }
        let display = Double(Int64(t.presented) - Int64(t.submitted))
        samples.append(display)
        guard let a = lead(t.submitted) else { return }
        leads.append((a, true, Self.late(t)))
        shown += 1
        if display > a + interval * 1.5 { lateShown += 1 }
    }

    private static func late(_ t: FrameTiming) -> Double? {
        t.presentLocal.map { Double(Int64(t.submitted) - Int64($0)) }
    }

    /// The refresh interval: measured once enough refreshes have passed,
    /// the screen's nominal one until then.
    private var interval: Double { period ?? nominal }

    /// The refresh interval from consecutive flips only: each gap between two
    /// is a whole number of refreshes, 1–4 of them, which the nominal interval
    /// tells apart without doubt (a panel 1% off is 0.04 refresh off over 4);
    /// the median of the last 512. The screen's real refresh can differ from
    /// the nominal one: the Dell U2725QE runs at 8.3443 ms (119.84 Hz), not
    /// 1/120 s. Counting the refreshes over a long span instead, with the
    /// nominal interval, always read 8.333 (the grid then ran 1300 ppm fast,
    /// the host followed it, and the lock slipped a refresh every ~6.5 s);
    /// with the last estimate, any estimate a little off reproduced itself
    /// (8.404 ms in full screen: the span divided by it rounds to the count
    /// that gives it back).
    private func measure() {
        guard let previous, let last, last > previous else { return }
        let d = Double(last - previous), k = (d / nominal).rounded()
        guard k >= 1 && k <= 4 && abs(d / k - nominal) < nominal * 0.02 else { return }
        gaps.append(d / k)
        if gaps.count > 512 { gaps.removeFirst(gaps.count - 512) }
        if gaps.count >= 60 { period = gaps.sorted()[gaps.count / 2] }
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
        } else if !step.isEmpty {
            let around = { (x: Double, m: Double) -> Double in
                var y = (x - m).truncatingRemainder(dividingBy: t)
                if y > t / 2 { y -= t } else if y <= -t / 2 { y += t }
                return m + y
            }
            let host = step.compactMap { s in s.late.map { (s.a + $0).truncatingRemainder(dividingBy: t) } }
            let a = step.map(\.a).sorted()
            let mid = a[a.count / 2]
            // Per lead bin, the share of frames shown (decayed: ~25 s).
            let bins = Int((t / Self.binNs).rounded(.up))
            if binAll.count != bins {
                binAll = Array(repeating: 0, count: bins)
                binDropped = binAll
            }
            for i in 0..<bins { binAll[i] *= 0.98; binDropped[i] *= 0.98 }
            var missed = 0
            for f in step {
                let i = min(bins - 1, Int(f.a / Self.binNs))
                binAll[i] += 1
                if !f.ok { binDropped[i] += 1; missed += 1 }
            }
            // Unseen leads count as even odds.
            let shownShare: [Double] = (0..<bins).map { i in
                let shown: Double = binAll[i] - binDropped[i] + 1
                return shown / (binAll[i] + 2)
            }
            recent += step.compactMap(\.late)
            if recent.count > 1200 { recent.removeFirst(recent.count - 1200) }
            if host.count * 2 > step.count && recent.count >= 60 {
                var h = host.map { around($0, host[0]) }.sorted()[host.count / 2]
                h -= (h / t).rounded(.down) * t
                // The host present's lead, one bin apart round the refresh, under
                // which the recent latenesses would have been shown most often.
                var best = h, bestShown = -1.0
                for c in 0..<bins {
                    let lead = Double(c) * Self.binNs
                    var sum = 0.0
                    for l in recent {
                        var a = (lead - l).truncatingRemainder(dividingBy: t)
                        if a < 0 { a += t }
                        sum += shownShare[min(bins - 1, Int(a / Self.binNs))]
                    }
                    if sum > bestShown { bestShown = sum; best = lead }
                }
                target = best
                expected = bestShown / Double(recent.count)
                // The phase is circular: the smaller way round.
                var move = best - h
                move -= (move / t).rounded() * t
                shift += move / 2 // earlier raises every lead
                if getenv("GM_DEBUG_PHASE") != nil {
                    FileHandle.standardError.write(String(format: "locked shift %+.1f host lead %.2f target %.2f (%.0f%% shown expected) lead p50 %.2f dropped %d\n",
                        shift / 1e6, h / 1e6, best / 1e6, expected * 100, mid / 1e6, missed).data(using: .utf8)!)
                }
            }
        }
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
        return String(format: "phase: %@, shift %+.1f ms, refresh %.3f ms", state, shift / 1e6, interval / 1e6)
            + (target.map { String(format: ", host lead %.2f ms, %.0f%% shown expected", $0 / 1e6, expected * 100) } ?? "")
    }
}
