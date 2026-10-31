import XCTest

/// The slot schedule: the sequence itself (DESIGN.md 11.1-11.3), and the
/// planner that picks its two numbers from both ends' latencies.
final class ScheduleTests: XCTestCase {
    /// A 120 Hz grid, and 45 fps on it: 2.667 refreshes per frame, so the
    /// pattern is 3, 2, 3 and `gap` is not a multiple of the grid.
    let G: UInt64 = 8_333_333
    let fps45: Double = 22_222_222
    let anchor: UInt64 = 1_000_000_000_000

    func schedule(gap: Double, granularity: UInt64, delay: Int64 = 0) -> Schedule {
        Schedule(anchor: anchor, granularity: granularity, gap: gap, delay: delay)
    }

    // MARK: the sequence

    /// a[n] is the gap between consecutive slots, so the world clock stepping
    /// by a[n] lands exactly on slot(n): sum(a[:n]) == slot(n) - anchor. This
    /// is the invariant the whole scheme rests on (11.1).
    func testAdvancementsSumToSlots() {
        let s = schedule(gap: fps45, granularity: G)
        var sum: UInt64 = 0
        for n in 0..<2000 {
            XCTAssertEqual(anchor &+ sum, s.slot(n), "frame \(n)")
            sum &+= s.advancement(n)
        }
    }

    /// Every advancement is a whole number of refreshes, so every slot is on
    /// the grid with no snapping at display time (11.2).
    func testEveryAdvancementIsWholeRefreshes() {
        let s = schedule(gap: fps45, granularity: G)
        for n in 0..<2000 {
            XCTAssertEqual(s.advancement(n) % G, 0, "frame \(n)")
            XCTAssertEqual(s.advancement(n), UInt64(s.refreshes(n)) * G)
            XCTAssertGreaterThanOrEqual(s.refreshes(n), 1, "frame \(n): m >= 1")
        }
    }

    /// The pattern for a fractional rate: each frame held 2 or 3 refreshes,
    /// never further than one from the ideal, and the mean exactly the rate
    /// asked for (11.3).
    func testFractionalRateFromIntegerSteps() {
        let s = schedule(gap: fps45, granularity: G)
        let n = 3000
        var total = 0
        for i in 0..<n {
            let m = s.refreshes(i)
            XCTAssert(m == 2 || m == 3, "frame \(i): \(m) refreshes")
            total += m
        }
        XCTAssertEqual(Double(total) / Double(n), fps45 / Double(G), accuracy: 0.001)
        XCTAssertEqual(s.cadence, 2.6666, accuracy: 0.001)
    }

    /// No slot sits more than half a refresh from where an unquantized
    /// schedule would have put it.
    func testNoSlotFartherThanHalfARefresh() {
        let s = schedule(gap: fps45, granularity: G)
        for n in 0..<2000 {
            let ideal = Double(anchor) + Double(n) * fps45
            XCTAssertLessThanOrEqual(abs(Double(s.slot(n)) - ideal), Double(G) / 2 + 1,
                                     "frame \(n)")
        }
    }

    /// The equivalence that lets this client use the closed form while the
    /// host runs the recurrence: at a constant target, carrying the rounding
    /// error forward gives the same sequence as round(n * gap / G) (11.3).
    func testClosedFormMatchesTheRecurrence() {
        for gap in [fps45, Double(G), Double(G) * 1.5001, 33_333_333, 20_000_000] {
            let s = schedule(gap: gap, granularity: G)
            var carry = 0.0, sum: UInt64 = 0
            for n in 0..<1000 {
                XCTAssertEqual(anchor &+ sum, s.slot(n), "gap \(gap), frame \(n)")
                let (a, c) = Schedule.step(target: gap, carry: carry, granularity: G)
                sum &+= UInt64(a)
                carry = c
            }
        }
    }

    /// Nothing to quantize when the rate is already a whole refresh.
    func testUniformRateIsExact() {
        let s = schedule(gap: Double(G) * 2, granularity: G)
        for n in 0..<100 {
            XCTAssertEqual(s.refreshes(n), 2)
            XCTAssertEqual(s.advancement(n), G * 2)
            XCTAssertEqual(s.slot(n), anchor &+ UInt64(n) * G * 2)
        }
    }

    /// Granularity 0 is the VRR case: the advancement is the target itself,
    /// the rounding is the identity, and there is no m.
    func testVRRTakesTheTargetDirectly() {
        let s = schedule(gap: fps45, granularity: 0)
        for n in 0..<100 {
            XCTAssertEqual(Double(s.advancement(n)), fps45, accuracy: 1)
            XCTAssertEqual(s.refreshes(n), 0)
        }
        XCTAssertEqual(s.cadence, 0)
        let (a, c) = Schedule.step(target: fps45, carry: 0, granularity: 0)
        XCTAssertEqual(a, fps45)
        XCTAssertEqual(c, 0)
    }

    /// A frame's host stamp names its index, for every rate the planner can
    /// produce (gap >= G, so the half-refresh error is always under half a
    /// frame).
    func testIndexInvertsSlot() {
        for mult in [1.0, 1.0001, 1.5, 2.6666, 3.0, 7.3] {
            let s = schedule(gap: Double(G) * mult, granularity: G)
            for n in 0..<500 {
                XCTAssertEqual(s.index(host: s.slot(n)), n, "x\(mult), frame \(n)")
            }
        }
    }

    func testDueIsSlotPlusTheOffset() {
        let s = schedule(gap: fps45, granularity: G, delay: 12_000_000)
        for n in 0..<50 {
            XCTAssertEqual(s.due(n), s.slot(n) &+ 12_000_000)
        }
        // A negative offset (the host's clock ahead of ours) still works.
        let back = schedule(gap: fps45, granularity: G, delay: -12_000_000)
        XCTAssertEqual(back.due(3), back.slot(3) &- 12_000_000)
    }

    // MARK: planning

    /// The next host present stamp `feed` hands out. One stream per test, so
    /// that feeding twice looks like a game whose rate changed, rather than
    /// like the host's clock jumping backwards (which the planner discards).
    private var host: UInt64 = 0

    /// Feeds both ends `count` frames: the host presenting every `interval`,
    /// each showable `ready` after its host stamp and on screen `display`
    /// after being submitted.
    @discardableResult
    func feed(_ s: Scheduler, count: Int = 61, interval: UInt64 = 8_333_333,
              ready: UInt64 = 10_000_000, display: UInt64 = 4_000_000,
              from base: UInt64 = 2_000_000_000) -> UInt64 {
        if host == 0 { host = anchor }
        var now = base
        for _ in 0..<count {
            let t = timing(present: host)
            t.decoded = host &+ ready
            t.submitted = now
            t.presented = now &+ display
            s.decoded(t, now: now)
            s.shown(t, now: now)
            now &+= interval
            host &+= interval
        }
        return now
    }

    /// Both ends are needed: one of them alone plans nothing, because the
    /// offset is the sum of two independent measurements (11.9).
    func testNoScheduleUntilBothEndsHaveSpoken() {
        let s = Scheduler()
        var now: UInt64 = 2_000_000_000
        for i in 0..<61 {
            let t = timing(present: anchor &+ UInt64(i) * G)
            t.decoded = t.presentHost &+ 10_000_000
            s.decoded(t, now: now)
            now &+= G
        }
        XCTAssertNil(s.replan(grid: (anchor, G), minGap: Double(G), now: now))
        XCTAssertNil(s.schedule)
    }

    /// gap from the host end alone, delay from both, with the grid's
    /// quantization added.
    func testPlansFromBothEnds() {
        let s = Scheduler()
        let now = feed(s, interval: G, ready: 10_000_000, display: 4_000_000)
        guard let plan = s.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(plan.granularity, G)
        XCTAssertEqual(plan.anchor, anchor)
        XCTAssertEqual(plan.gap, Double(G), accuracy: 1)
        XCTAssertEqual(plan.delay, 10_000_000 + 4_000_000 + Int64(G))
        XCTAssertEqual(plan.epoch, 1)
        XCTAssertEqual(s.replans, 0, "the first plan is not a re-plan")
    }

    /// Feasibility: the host cannot be held to a gap shorter than the
    /// interval it sustains, nor shorter than the panel can show (11.4).
    func testGapIsFlooredByBothEnds() {
        let slow = Scheduler()
        var now = feed(slow, interval: G * 3)
        guard let plan = slow.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(plan.gap, Double(G * 3), accuracy: 1, "the game's own interval")

        let fast = Scheduler()
        now = feed(fast, interval: 4_000_000)
        guard let capped = fast.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(capped.gap, Double(G), accuracy: 1, "the panel's cap")
    }

    /// A whole number of refreshes is taken when it costs little rate (every
    /// frame then held the same time), and not when it costs a lot.
    func testUniformPreferredOnlyWhenCheap() {
        let cheap = Scheduler()
        // 2.95 refreshes: rounding up to 3 costs 1.7% of the rate.
        var now = feed(cheap, interval: UInt64(Double(G) * 2.95))
        guard let a = cheap.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(a.gap / Double(G), 3.0, accuracy: 0.001)
        XCTAssertEqual(a.refreshes(0), 3)

        let dear = Scheduler()
        // 2.1 refreshes: rounding up to 3 would cost 43%.
        now = feed(dear, interval: UInt64(Double(G) * 2.1))
        guard let b = dear.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(b.gap / Double(G), 2.1, accuracy: 0.01)
        XCTAssert(b.refreshes(0) == 2 || b.refreshes(1) == 2, "a 2 in the pattern")
    }

    /// A variable-refresh display: no grid, so no quantization anywhere.
    func testVRRPlanHasNoGranularity() {
        let s = Scheduler()
        let now = feed(s, interval: 22_222_222)
        guard let plan = s.replan(grid: nil, minGap: 8_333_333, now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(plan.granularity, 0)
        XCTAssertEqual(plan.gap, 22_222_222, accuracy: 1)
        XCTAssertEqual(plan.delay, 10_000_000 + 4_000_000, "no grid to add")
    }

    /// Up at once, down only after the estimate has sat lower for `settle`:
    /// the schedule holds still rather than tracking the noise.
    func testRaisesAtOnceAndLowersOnlyAfterSettle() {
        let s = Scheduler()
        var now = feed(s, interval: G * 2)
        guard let first = s.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(first.gap, Double(G * 2), accuracy: 1)

        // The game slows down: taken at the next re-plan. (Each jump past the
        // sample window is what retires the intervals of the old rate; p99 of
        // a mixture is the slower one, which is the point of taking p99.)
        now &+= Scheduler.window &+ 1
        now = feed(s, interval: G * 3, from: now)
        guard let slower = s.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(slower.gap, Double(G * 3), accuracy: 1, "not taken at once")
        XCTAssertEqual(s.replans, 1)

        // It speeds back up. The first window still holds one sample of the
        // old interval - the boundary between the two feeds - and p99 of a
        // window this size is its maximum (minSamples is 60, and this
        // quantile of 60 samples is the largest), so one stale sample pins
        // the estimate for a whole window. Conservative on purpose: the gap
        // the host is held to should not fall on thin evidence.
        now &+= Scheduler.window &+ 1
        now = feed(s, interval: G * 2, from: now)
        XCTAssertEqual(s.samples.intervals, 61)
        XCTAssertEqual(s.replan(grid: (anchor, G), minGap: Double(G), now: now)?.gap,
                       slower.gap, "lowered on one stale sample")

        // A clean window of the faster rate: still not taken, only noted.
        now &+= Scheduler.window &+ 1
        now = feed(s, interval: G * 2, from: now)
        XCTAssertEqual(s.replan(grid: (anchor, G), minGap: Double(G), now: now)?.gap,
                       slower.gap, "lowered before settling")
        XCTAssertEqual(s.replans, 1)

        // Still faster after `settle`: now taken.
        now &+= Scheduler.settle
        now = feed(s, interval: G * 2, from: now)
        guard let faster = s.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(faster.gap, Double(G * 2), accuracy: 1)
        XCTAssertGreaterThan(faster.epoch, slower.epoch)
        XCTAssertEqual(s.replans, 2)
    }

    /// With a uniform advancement the schedule is exactly what DisplayGrid
    /// already carries, which is why this half needs no host change (11.10).
    func testHostGridIsTheSchedule() {
        let s = Scheduler()
        XCTAssertNil(s.hostGrid)
        let now = feed(s, interval: G * 2)
        _ = s.replan(grid: (anchor, G), minGap: Double(G), now: now)
        guard let grid = s.hostGrid else { return XCTFail("no grid") }
        XCTAssertEqual(grid.anchor, anchor)
        XCTAssertEqual(grid.interval, G * 2)
    }

    // MARK: binding

    /// A frame is shown at its slot, once, in order: never early, never late.
    func testBindShowsStaleAndLate() {
        let s = Scheduler()
        let now = feed(s, interval: G * 2)
        guard let plan = s.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }

        // Frame 100, arriving well before its slot: shown at the slot, not now.
        let due = plan.due(100)
        let t = timing(present: plan.slot(100))
        XCTAssertEqual(s.bind(t, now: due &- 5_000_000),
                       .show(at: due, slot: plan.slot(100)))
        XCTAssertEqual(s.bound, 1)

        // The same slot again: already used.
        XCTAssertEqual(s.bind(timing(present: plan.slot(100)), now: due), .stale)
        // An earlier slot, arriving out of order: also stale.
        XCTAssertEqual(s.bind(timing(present: plan.slot(99)), now: due), .stale)
        XCTAssertEqual(s.stale, 2)

        // Frame 101, arriving after its slot: dropped.
        XCTAssertEqual(s.bind(timing(present: plan.slot(101)),
                              now: plan.due(101) &+ 5_000_000), .late)
        XCTAssertEqual(s.late, 1)
        // And the slot was not consumed, so 102 still binds.
        if case .show = s.bind(timing(present: plan.slot(102)), now: plan.due(102)) {} else {
            XCTFail("102 did not bind")
        }
    }

    /// Without a schedule, frames are shown as they come.
    func testUnscheduledBeforeThereIsAPlan() {
        let s = Scheduler()
        XCTAssertEqual(s.bind(timing(present: anchor)), .unscheduled)
    }

    /// The last check before drawing, in case the wait overshot.
    func testOnTimeTolerance() {
        let s = Scheduler()
        let due: UInt64 = 1_000_000_000
        XCTAssert(s.onTime(due, now: due))
        XCTAssert(s.onTime(due, now: due &+ Scheduler.lateTolerance))
        XCTAssertFalse(s.onTime(due, now: due &+ Scheduler.lateTolerance &+ 1))
        XCTAssertEqual(s.late, 1)
    }

    /// A late frame raises the offset at the next re-plan, whatever the
    /// samples say: that is how the host refuses a gap it cannot meet (11.9).
    func testLatenessRaisesTheOffset() {
        let s = Scheduler()
        var now = feed(s, interval: G * 2)
        guard let first = s.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertEqual(s.replans, 0)

        // Two frames miss their slots, then the ends report a longer delivery.
        _ = s.onTime(now, now: now &+ 10_000_000)
        _ = s.onTime(now, now: now &+ 10_000_000)
        now = feed(s, interval: G * 2, display: 9_000_000, from: now)
        guard let after = s.replan(grid: (anchor, G), minGap: Double(G), now: now) else {
            return XCTFail("no schedule")
        }
        XCTAssertGreaterThan(after.delay, first.delay)
        XCTAssertEqual(s.replans, 1)
    }

    func testResetClearsEverything() {
        let s = Scheduler()
        let now = feed(s)
        _ = s.replan(grid: (anchor, G), minGap: Double(G), now: now)
        XCTAssertNotNil(s.schedule)
        s.reset()
        XCTAssertNil(s.schedule)
        XCTAssertNil(s.hostGrid)
        XCTAssertEqual(s.bind(timing(present: anchor)), .unscheduled)
        XCTAssertEqual(s.late + s.stale + s.bound, 0)
    }
}
