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
// Leads are binned unwrapped around the median lateness (a frame later than
// usual counts as short of its flip, not as early), and a lead qualifies when
// the recent latenesses would show 99.9% of frames under it (`enough`); of
// those the lock takes the lowest, so it sits as close to the cut-off as the
// network's tail allows, and creeps toward unseen leads 1 ms at a time. On
// the iPad (cut-off ~0.8 ms before the flip): --mock display 12.8 → 9.3 ms;
// Wi-Fi with lateness p99 +5.4 ms over its median: 16.3 / 16.9 → 14.3 / 15.2
// ms p50 / p90 at ~0.12 drops/s, the late spikes.
//
// To find a starting point, sweep `shift` through one whole refresh,
// recording the median display stage at each step, and start at the lowest
// step without drops, moved `margin` earlier. Steering every step follows the drift between the two machines'
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
    // Since the last step; ok: shown on time; late: submit − the host's present
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
    /// The share of frames a lead must show to be chosen: one drop in
    /// 1000 frames (8 s at 120 Hz).
    static let enough = 0.999

    // Vsync off (full screen): macOS flips each frame straight to the panel
    // ~3 ms after it is submitted, wherever the scan is, so there is no
    // refresh cut-off to steer around; the frame tears where the flip lands.
    // The lock then steers the flips to `flipLead` before the panel's vblank
    // (where a tear shows least) on the grid measured while vsync was on:
    // immediate flips say nothing about the refreshes, and neither does
    // CVDisplayLink then (it follows our flips: ±8 ms off a line on the
    // built-in panel). The panel's grid is a
    // steady clock: vsync-on flips on the built-in panel lay on one line,
    // 16666.605 µs apart in every run, and a line fitted to one run placed
    // another's flips 149 s later within 5 µs.
    private var immediate = false
    private var flips: [Double] = []       // since the last step: flip − its target, ns
    private var flipError: Double?         // the last step's median
    static let flipLead = (getenv("GM_FLIP_LEAD").flatMap { Double(String(cString: $0)) } ?? 0.3) * 1e6
    /// Measuring (GM_HOST_LEAD=<ms>): steer to this host present lead
    /// instead of the learned best (finds where frames stop being shown).
    static let pinnedLead = getenv("GM_HOST_LEAD").flatMap { Double(String(cString: $0)) }.map { $0 * 1e6 }

    init(refreshHz: Double) {
        nominal = 1e9 / refreshHz
    }

    /// Switches between the two modes; back to vsync, it sweeps again.
    func setImmediate(_ on: Bool) {
        lock.lock(); defer { lock.unlock() }
        guard on != immediate else { return }
        immediate = on
        flips.removeAll()
        flipError = nil
        if !on { startSweep() }
    }

    /// The panel's refresh grid (a vblank and the interval) once measured
    /// from vsync-on flips, which vsync off needs.
    func grid() -> (vblank: UInt64, interval: Double)? {
        lock.lock(); defer { lock.unlock() }
        guard let period, let last else { return nil }
        return (last, period)
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
        // Every frame's lead counts, settling or not: it is measured on the
        // panel's grid, which a step does not move (drops right after a
        // step, when gamescope moves to the new grid, were missed).
        if t.submitted > 0, let a = lead(t.submitted) { leads.append((a, false, Self.late(t))) }
    }

    /// A frame reached the screen (any thread).
    func presented(_ t: FrameTiming) {
        lock.lock(); defer { lock.unlock() }
        if immediate {
            // A flip, not a refresh: how far from its target it landed.
            if t.submitted > since, let last {
                var e = (Double(Int64(t.presented) - Int64(last)) + Self.flipLead).truncatingRemainder(dividingBy: interval)
                if e > interval / 2 { e -= interval } else if e <= -interval / 2 { e += interval }
                flips.append(e)
            }
            return
        }
        if first == nil { first = t.presented }
        if t.presented != last { previous = last; last = t.presented; measure() }
        guard t.submitted > 0, let a = lead(t.submitted) else { return }
        // On time, a frame is on screen a refresh after the flip point it
        // led; one that missed it and was shown a refresh later (nothing
        // newer replaced it) missed as a dropped one did.
        let display = Double(Int64(t.presented) - Int64(t.submitted))
        let onTime = display <= a + interval * 1.5
        leads.append((a, onTime, Self.late(t)))
        guard t.submitted > since else { return }
        samples.append(display)
        shown += 1
        if !onTime { lateShown += 1 }
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
        let t = interval
        if immediate {
            // The vblank nearest now on the measured grid (the anchor sent
            // stays recent; the interval goes out rounded to the ns), and the
            // flips' median error halved into the shift: a later flip, an
            // earlier grid.
            let refresh = (Double(Int64(nowNs()) - Int64(last)) / t).rounded()
            let e = flips.sorted()
            flips.removeAll()
            if e.count >= 10 {
                flipError = e[e.count / 2]
                shift += flipError! / 2
            }
            since = nowNs() + 100_000_000
            return (UInt64(Int64(last) + Int64(refresh * t) - Int64(shift)), UInt64(t.rounded()))
        }
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
            recent += step.compactMap(\.late)
            if recent.count > 1200 { recent.removeFirst(recent.count - 1200) }
            // Leads unwrapped around the median lateness `med`: a frame's
            // lead is what a frame of median lateness had in its step (its
            // host lead minus `med`, within a refresh) minus how much later
            // than the median it was. A frame 5 ms later than usual then
            // counts as 5 ms short of its flip, not (wrapped) as almost a
            // refresh early; wrapped, Wi-Fi spikes that missed their flip
            // and were replaced marked the early bins, and the lock steered
            // to 1 ms over the cut-off and dropped 0.85 frames/s.
            let med = recent.isEmpty ? 0 : recent.sorted()[recent.count / 2]
            let wrap = { (x: Double) -> Double in let y = x.truncatingRemainder(dividingBy: t); return y < 0 ? y + t : y }
            // Per lead bin from -t to 2t, the share of frames shown (decayed: ~25 s).
            let bins = Int((3 * t / Self.binNs).rounded(.up))
            let bin = { (u: Double) in max(0, min(bins - 1, Int((u + t) / Self.binNs))) }
            if binAll.count != bins {
                binAll = Array(repeating: 0, count: bins)
                binDropped = binAll
            }
            for i in 0..<bins { binAll[i] *= 0.98; binDropped[i] *= 0.98 }
            var missed = 0
            for f in step {
                guard let l = f.late else { continue }
                let i = bin(wrap(f.a + l - med) - (l - med))
                binAll[i] += 1
                if !f.ok { binDropped[i] += 1; missed += 1 }
            }
            // Each bin's share starts from `Self.enough`, worth two frames,
            // if a bin within 1 ms has frames: a bin with few frames, all
            // shown, qualifies as well as one with many (with even odds as
            // the prior, the bin the lock sat on always outscored the lower
            // ones it had seen clean, and the lock never moved down: on the
            // iPad it held a 4.5 ms lead where 1.0 drops nothing), and the
            // lock creeps into unseen leads 1 ms at a time; one drop there
            // rules a bin out until it decays. Leads far from any seen are
            // even odds.
            let reach = Int((1e6 / Self.binNs).rounded())
            let shownShare: [Double] = (0..<bins).map { i in
                let near = (max(0, i - reach)...min(bins - 1, i + reach)).contains { binAll[$0] >= 1 }
                let prior = near ? Self.enough : 0.5
                return (binAll[i] - binDropped[i] + 2 * prior) / (binAll[i] + 2)
            }
            if host.count * 2 > step.count && recent.count >= 60 {
                var h = host.map { around($0, host[0]) }.sorted()[host.count / 2]
                h -= (h / t).rounded(.down) * t
                // The lead of a frame of median lateness, one bin apart over
                // a refresh: the lowest (the least latency) under which the
                // recent latenesses would show `enough` frames; none: the
                // one that would show the most. The host present's lead is
                // that plus the median lateness.
                let deviations = recent.map { $0 - med }
                let scores: [Double] = (0..<Int((t / Self.binNs).rounded(.up))).map { c in
                    let lead = Double(c) * Self.binNs
                    return deviations.reduce(0) { $0 + shownShare[bin(lead - $1)] }
                }
                let c = scores.firstIndex { $0 >= Self.enough * Double(recent.count) }
                    ?? scores.indices.max { scores[$0] < scores[$1] }!
                var best = wrap(Double(c) * Self.binNs + med)
                let bestShown = scores[c]
                if let pin = Self.pinnedLead { best = pin }
                target = best
                expected = bestShown / Double(recent.count)
                // The phase is circular: the smaller way round.
                var move = best - h
                move -= (move / t).rounded() * t
                shift += move / 2 // earlier raises every lead
                if getenv("GM_DEBUG_PHASE") != nil {
                    FileHandle.standardError.write(String(format: "locked shift %+.1f host lead %.2f target %.2f (%.2f%% shown expected) lead p50 %.2f dropped %d med %.2f\n",
                        shift / 1e6, h / 1e6, best / 1e6, expected * 100, mid / 1e6, missed, med / 1e6).data(using: .utf8)!)
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
            return String(format: "phase: vsync off, flips %@ from %.1f ms before the vblank, shift %+.1f ms, refresh %.3f ms",
                          flipError.map { String(format: "%+.2f ms", $0 / 1e6) } ?? "-", Self.flipLead / 1e6, shift / 1e6, interval / 1e6)
        }
        let state = sweeping ? "sweeping \(sweep.count)/\(Int((interval / Self.sweepStep).rounded(.up)))" : "locked"
        return String(format: "phase: %@, shift %+.1f ms, refresh %.3f ms", state, shift / 1e6, interval / 1e6)
            + (target.map { String(format: ", host lead %.2f ms, %.0f%% shown expected", $0 / 1e6, expected * 100) } ?? "")
    }
}
