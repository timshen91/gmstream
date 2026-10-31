// Slot schedule: each frame is given one point of this display's refresh grid
// before it is rendered, and the host is held to the same spacing, so that
// game-world time and real time stay a constant apart.
//
// The world clock is not commanded: no Vulkan or Wayland interface can tell a
// game to advance by X. What moves it is when the game is let go - holding
// wl_buffer.release sets a release-bound loop's period, and an engine taking
// its dt from the wall clock then tracks the release times with a bounded
// error. So the job is not to command the clock but to make the release
// schedule and the display schedule the same affine map, which is what this
// is. DESIGN.md section 11.11 for what the interfaces do and do not offer.
//
// The formulation is DESIGN.md section 11; the short version:
//
//   - One sequence, read two ways. The decision variable is the advancement
//     a[n]: how much game time frame n moves the world forward. World time is
//     its running sum, W[n] = sum(a[:n+1]), and frame n is shown at
//     D[n] = W[n] + L for one constant L. So D[n] - D[n-1] == a[n]: the
//     advancement sequence and the display-interval sequence are the same
//     list. The engine reads it as how far to step the world, the panel as
//     when the next picture appears, and there is nothing to keep in sync.
//
//   - The grid constraint is satisfied by construction. D[n] has to land on a
//     refresh, anchor + k*G. Since D[n] = anchor + W[n] + L and W[n] is a
//     running sum, making every advancement a whole number of refreshes puts
//     every frame on the grid with no snapping at display time:
//
//         a[n] = m[n] * G   (m[n] >= 1 integer)    fixed-refresh panel
//         a[n] = any real                          VRR panel (grid continuous)
//
//     That is the whole VRR / fixed-refresh unification: the decision variable
//     is integer in one case and real in the other. `granularity` 0 is the
//     VRR case and `step` below is then the identity.
//
//   - A fractional rate from integer steps. m[n] integer does not quantize the
//     rate: carry the rounding error into the next step (`step`), so a 22.2 ms
//     target on an 8.33 ms grid gives 3, 2, 3, 3, 2, 3 refreshes with mean
//     exactly 22.2. At a constant target that recurrence has a closed form,
//     sum(m[:n]) == round(n * gap / G), which is Bresenham's line and is what
//     `steps(n)` computes. They agree exactly, which is what lets this client
//     name a frame's slot without being sent the sequence.
//
//   - Feasibility. mean(a) >= mean(R) for R the host's render cost: frames
//     cannot be scheduled closer together than the game can make them. A
//     quantile of R rather than its mean buys margin for the jitter, and the
//     margin is paid for in render-ahead depth, which is input lag.
//
//   - The offset. L is one number for the whole schedule - that is what makes
//     the map affine - and it carries the two machines' clock offset, as the
//     hold's constant does (Video.swift, Hold), so no clock sync is needed:
//     ping-based sync moves 0.5-2.6 ms on busy Wi-Fi (DESIGN.md section 8),
//     which a fixed offset cannot absorb.
//
//   - Both ends meet at p99, from two measurements neither of which is
//     self-confirming (a quantity we caused is no evidence: DESIGN.md
//     section 8, "no self-confirming estimate"):
//
//         ready   = decoded - host present    encode + network + decode, and
//                                             the clock offset. Unaffected by
//                                             the schedule: when a frame
//                                             became showable, not when we
//                                             showed it.
//         display = presented - submitted     macOS's part, timed by Metal.
//
//         delay = p99(ready) + p99(display) + G       (L)
//         gap   = max(p99(host present spacing), minGap)
//
//     i.e. the interval the game actually sustains at p99, floored by what the
//     panel can show (PanelRate's cap). Where rounding `gap` up to a whole
//     refresh costs little rate, take it: a uniform cadence holds every frame
//     for the same time, and the residual artifact of an uneven one is uneven
//     persistence (motion smear changing with the pattern). Uniform is
//     preferred, not required.
//
//   - Re-planning. A `gap` change is free: the slope stays 1, only the sample
//     density changes, so a game dropping from 60 to 40 fps is not a pacing
//     event. A `delay` change is a one-time step in the map, so it is stepped,
//     never slewed - a slewed offset is many frames at the wrong slope, which
//     is the judder being avoided. (Measured, for the hold's constant: an
//     edge-creeping tracker "was smooth but never still".) Both move up at
//     once and down only after the estimate has sat lower for `settle`, and a
//     re-plan needs a material change, so the quality metric is re-plans per
//     minute rather than stutter.
//
//   - Binding. A frame's slot is decided from its host stamp, once. It is not
//     shown before its slot (an early frame breaks the map exactly as a late
//     one does, and every "show as soon as it is ready" path in the stack does
//     this) and not after: a frame past its slot is dropped, and the drop
//     feeds the next re-plan. Slots are used in order; a frame for a slot
//     already shown is stale.
//
// What this file is not: the commitment boundary, the lookahead and the
// render-ahead depth B (DESIGN.md 11.5-11.7) live on the host, which owns the
// world clock and the game's release. This client owns `delay` and proposes
// `gap`; the host refuses it implicitly, by missing slots. With a uniform
// advancement the schedule is exactly (anchor, interval), which DisplayGrid
// (protocol 0x0d) already carries, so this half needs no host change.

import Foundation

/// Where frames land: a grid point per frame, and the delay from the host's
/// slot stamp to showing it here. See the formulation at the top of this file.
struct Schedule: Equatable {
    /// A grid point, host clock.
    var anchor: UInt64
    /// The grid, ns. 0: none (a variable-refresh display), so an advancement
    /// may be any real and the slots are exactly `gap` apart.
    var granularity: UInt64
    /// Mean advancement, ns: the frame interval the host is held to. Need not
    /// be a multiple of `granularity`.
    var gap: Double
    /// slot (host clock) + `delay` = when to show it (our clock). This is L:
    /// one number for the whole schedule, carrying the clock offset, so no
    /// clock sync is needed.
    var delay: Int64
    /// Bumped on every re-plan.
    var epoch: UInt32 = 0

    /// One advancement and the error to carry into the next: the general form,
    /// for a target that varies per frame. At a constant target this agrees
    /// exactly with `steps`/`slot` below, which is what lets the client use
    /// the closed form while the host runs the recurrence.
    static func step(target: Double, carry: Double,
                     granularity: UInt64) -> (advancement: Double, carry: Double) {
        guard granularity > 0 else { return (target, 0) }
        let g = Double(granularity)
        let ideal = target + carry
        let m = max(1.0, (ideal / g).rounded())
        return (m * g, ideal - m * g)
    }

    /// Whole refreshes from `anchor` to frame `n`: round(n * gap / G), i.e.
    /// Bresenham's line, so the mean advancement is exactly `gap` and no slot
    /// sits more than half a refresh from its ideal time. At granularity 0
    /// this is in units of 1 ns and the slots are exactly `gap` apart.
    func steps(_ n: Int) -> Int {
        let unit = granularity > 0 ? Double(granularity) : 1
        return Int((Double(n) * gap / unit).rounded())
    }

    /// Frame `n`'s slot, host clock.
    func slot(_ n: Int) -> UInt64 {
        let unit = granularity > 0 ? Double(granularity) : 1
        let shift = Double(steps(n)) * unit
        return shift >= 0 ? anchor &+ UInt64(shift) : anchor &- UInt64(-shift)
    }

    /// How much game time frame `n` advances the world: a[n], the gap between
    /// consecutive slots, so that sum(advancement(k) for k < n) == slot(n) -
    /// anchor holds exactly.
    func advancement(_ n: Int) -> UInt64 {
        let a = slot(n), b = slot(n + 1)
        return b > a ? b - a : 0
    }

    /// Frame `n`'s advancement in whole refreshes: m[n], the 3, 2, 3 pattern.
    /// 0 on a variable-refresh display, which has no m.
    func refreshes(_ n: Int) -> Int {
        guard granularity > 0 else { return 0 }
        return steps(n + 1) - steps(n)
    }

    /// The frame index a host slot stamp belongs to.
    func index(host t: UInt64) -> Int {
        guard gap > 0 else { return 0 }
        return Int(((Double(t) - Double(anchor)) / gap).rounded())
    }

    /// When to show frame `n`, in our clock.
    func due(_ n: Int) -> UInt64 {
        let s = Int64(bitPattern: slot(n)) &+ delay
        return s > 0 ? UInt64(s) : 0
    }

    /// Mean refreshes per frame, for the stats ("3.00" uniform, "2.67" a
    /// 3, 3, 2 pattern).
    var cadence: Double { granularity > 0 ? gap / Double(granularity) : 0 }
}

/// Plans the schedule from both ends' recent latencies, and binds each frame
/// to its slot. See the formulation at the top of this file.
final class Scheduler {
    /// Both ends must meet here.
    static let quantile = 0.99
    /// Samples kept (the hold uses 4 s of latenesses, PanelRate 2 s).
    static let window: UInt64 = 2_000_000_000
    /// Before a re-plan: enough of both ends to have a p99 at all.
    static let minSamples = 60
    /// A re-plan needs this much change, so the schedule holds still.
    static let gapChange = 0.05
    static let delayChange: Int64 = 1_000_000
    /// Down only after the estimate has sat lower this long.
    static let settle: UInt64 = 3_000_000_000
    /// Rounding `gap` up to a whole number of refreshes is taken when it
    /// costs no more rate than this: a uniform cadence holds every frame for
    /// the same time.
    static let uniformCost = 1.10
    /// Shown this far past its slot still counts as on time.
    static let lateTolerance: UInt64 = 500_000

    private(set) var schedule: Schedule?
    private(set) var replans = 0
    private(set) var late = 0, stale = 0, bound = 0

    private var intervals: [(at: UInt64, ns: Double)] = []
    private var ready: [(at: UInt64, ns: Int64)] = []
    private var display: [(at: UInt64, ns: Double)] = []
    private var lates: [UInt64] = []
    private var lastHostPresent: UInt64 = 0
    private var lastIndex = Int.min
    private var lowSince: UInt64?
    private var epoch: UInt32 = 0

    /// How much evidence each end has given lately, for the stats overlay and
    /// to tell "not re-planned" from "not enough samples to re-plan".
    var samples: (intervals: Int, ready: Int, display: Int) {
        (intervals.count, ready.count, display.count)
    }

    /// Starts over: a new stream, a new screen, or the grid moved.
    func reset() {
        schedule = nil
        intervals = []; ready = []; display = []; lates = []
        lastHostPresent = 0
        lastIndex = Int.min
        lowSince = nil
        late = 0; stale = 0; bound = 0
    }

    // MARK: the two ends

    /// A frame became showable: the host end (encode + network + decode, and
    /// the clock offset), and how far apart the host is producing frames.
    func decoded(_ t: FrameTiming, now: UInt64 = nowNs()) {
        if lastHostPresent > 0 && t.presentHost > lastHostPresent {
            let d = Double(t.presentHost - lastHostPresent)
            // A gap in the stream is not the game's interval.
            if d < 500_000_000 { intervals.append((now, d)) }
        }
        lastHostPresent = max(lastHostPresent, t.presentHost)
        if t.decoded > 0 {
            ready.append((now, Int64(bitPattern: t.decoded) &- Int64(bitPattern: t.presentHost)))
        }
        trim(now)
    }

    /// A frame reached the screen: macOS's part, which the schedule does not
    /// cause (so it is evidence, unlike presented − our own due time).
    func shown(_ t: FrameTiming, now: UInt64 = nowNs()) {
        guard t.presented > t.submitted else { return }
        display.append((now, Double(t.presented - t.submitted)))
        trim(now)
    }

    private func trim(_ now: UInt64) {
        let cut = now > Self.window ? now - Self.window : 0
        intervals.removeAll { $0.at < cut }
        ready.removeAll { $0.at < cut }
        display.removeAll { $0.at < cut }
        let lateCut = now > 1_000_000_000 ? now - 1_000_000_000 : 0
        lates.removeAll { $0 < lateCut }
    }

    // MARK: planning

    /// The schedule to use, re-planned when both ends have moved enough.
    /// `grid`: a vblank and the refresh interval here, in host clock as
    /// DisplayGrid sends them (nil: a variable-refresh display, no
    /// granularity). `minGap`: the shortest spacing the display can show
    /// (PanelRate's cap), ns.
    @discardableResult
    func replan(grid: (anchor: UInt64, interval: UInt64)?, minGap: Double,
                now: UInt64 = nowNs()) -> Schedule? {
        guard intervals.count >= Self.minSamples, ready.count >= Self.minSamples,
              display.count >= Self.minSamples else { return schedule }
        let g = Double(grid?.interval ?? 0)
        // Feasibility: the host cannot be held to a gap shorter than the
        // interval it actually sustains.
        var gap = max(quantile(intervals.map(\.ns), Self.quantile), minGap, 1)
        if g > 0 {
            // Prefer a whole number of refreshes where the rate it costs is
            // small: every frame then held for the same time.
            let uniform = (gap / g).rounded(.up) * g
            if uniform <= gap * Self.uniformCost { gap = uniform }
            gap = max(gap, g)
        }
        let want = Schedule(
            anchor: grid?.anchor ?? (schedule?.anchor ?? lastHostPresent),
            granularity: grid?.interval ?? 0,
            gap: gap,
            delay: quantile(ready.map(\.ns), Self.quantile)
                &+ Int64(quantile(display.map(\.ns), Self.quantile))
                &+ Int64(g),
            epoch: epoch)

        guard let have = schedule else { return adopt(want, now) }
        // Up at once, down only once the estimate has sat lower for `settle`.
        let slower = want.gap > have.gap * (1 + Self.gapChange)
        let later = want.delay > have.delay &+ Self.delayChange
        if slower || later || !lates.isEmpty {
            lowSince = nil
            var next = want
            next.gap = max(want.gap, have.gap)
            next.delay = max(want.delay, have.delay)
            // Two late frames in a second: the offset is short whatever the
            // samples say (the hold's rule).
            if lates.count >= 2 { next.delay = max(next.delay, want.delay) }
            return adopt(next, now)
        }
        let faster = want.gap < have.gap * (1 - Self.gapChange)
        let sooner = want.delay < have.delay &- Self.delayChange
        guard faster || sooner else { lowSince = nil; return have }
        guard let since = lowSince else { lowSince = now; return have }
        guard now - since >= Self.settle else { return have }
        lowSince = nil
        return adopt(want, now)
    }

    private func adopt(_ s: Schedule, _ now: UInt64) -> Schedule {
        epoch &+= 1
        var next = s
        next.epoch = epoch
        if schedule != nil { replans += 1 }
        schedule = next
        lastIndex = Int.min
        return next
    }

    /// What the host should be told to pace to: the schedule as a grid
    /// (DisplayGrid, protocol 0x0d). nil until there is one. A fractional
    /// `gap` cannot be expressed this way - that is phase 2, DESIGN.md 11.10.
    var hostGrid: (anchor: UInt64, interval: UInt64)? {
        guard let s = schedule, s.gap >= 1 else { return nil }
        return (s.anchor, UInt64(s.gap.rounded()))
    }

    // MARK: binding

    enum Decision: Equatable {
        /// Show it at this time (our clock), for this slot (host clock).
        case show(at: UInt64, slot: UInt64)
        /// Its slot has passed.
        case late
        /// Its slot was already used.
        case stale
        /// No schedule yet: show it as it comes.
        case unscheduled
    }

    /// The slot a frame belongs to and when to show it. Never earlier than
    /// its slot, never later: a frame past its slot is dropped and the drop
    /// raises the offset at the next re-plan.
    func bind(_ t: FrameTiming, now: UInt64 = nowNs()) -> Decision {
        guard let s = schedule else { return .unscheduled }
        let n = s.index(host: t.presentHost)
        guard n > lastIndex else { stale += 1; return .stale }
        let due = s.due(n)
        if now > due &+ Self.lateTolerance {
            late += 1
            lates.append(now)
            return .late
        }
        lastIndex = n
        bound += 1
        return .show(at: due, slot: s.slot(n))
    }

    /// Checked again right before drawing, in case the wait overshot.
    func onTime(_ due: UInt64, now: UInt64 = nowNs()) -> Bool {
        if now <= due &+ Self.lateTolerance { return true }
        late += 1
        lates.append(now)
        return false
    }

    private func quantile<T: Comparable>(_ xs: [T], _ q: Double) -> T {
        let s = xs.sorted()
        return s[min(s.count - 1, max(0, Int(Double(s.count) * q)))]
    }
}
