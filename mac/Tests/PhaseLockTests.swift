import XCTest

/// The phase lock against a simulated display and host.
///
/// The display refreshes every `refresh` ns. With vsync, a frame submitted at
/// least `cutoff` before a vblank is flipped there and on screen `depth`
/// refreshes later; of several ready for one vblank the newest is shown and
/// the rest dropped (disableFIFO). With vsync off a frame is flipped
/// `flipDelay` after it is submitted, wherever the scan is. The host presents
/// a frame on every point of the grid the lock last sent (on its own nominal
/// grid before that), and each is submitted `lateness` (± `jitter`) later.
/// Times are this Mac's clock, far ahead of now so that none counts as
/// submitted before the lock's settle point (it uses the real clock).
final class Sim {
    let refresh: Double = 16_666_605 // the M4 MacBook Air's panel, measured
    let vblank0: UInt64 = nowNs() + 1_000_000_000_000
    var cutoff = 2e6
    var depth = 1
    var flipDelay = 3e6
    var lateness = 6e6
    var jitter = 0.5e6
    var immediate = false

    let lock = PhaseLock(refreshHz: 60)
    private var t: UInt64           // start of the next step
    private var grid: (anchor: UInt64, interval: UInt64)?
    private var pending: [FrameTiming] = []
    private var random: UInt64 = 0x9E3779B97F4A7C15

    struct Step {
        var shown = 0, dropped = 0
        var display: [Double] = []   // presented − submitted, ns
        var flipPhase: [Double] = [] // vsync off: flip − nearest vblank, ns
        var grid: (anchor: UInt64, interval: UInt64)?
        var medianDisplay: Double { display.sorted()[display.count / 2] }
        var medianFlipPhase: Double { flipPhase.sorted()[flipPhase.count / 2] }
    }

    init() { t = vblank0 }

    private func noise() -> Double { // uniform in [-1, 1), deterministic
        random = random &* 6364136223846793005 &+ 1442695040888963407
        return Double(random >> 11) / Double(1 << 52) * 2 - 1
    }

    /// Half a second of frames, then the lock's step.
    @discardableResult
    func step() -> Step {
        let end = t + 500_000_000
        var s = Step()
        // The host's frames for this half second.
        let (anchor, interval) = grid ?? (vblank0 + 5_000_000, 16_666_667)
        let first = (Double(Int64(t) - Int64(anchor)) / Double(interval)).rounded(.up)
        var k = first
        while true {
            let present = UInt64(Double(anchor) + k * Double(interval))
            guard present < end else { break }
            let f = timing(present: present)
            f.submitted = UInt64(Double(present) + lateness + jitter * noise())
            pending.append(f)
            k += 1
        }
        pending.sort { $0.submitted < $1.submitted }
        if immediate {
            for f in pending where f.submitted < end {
                f.presented = UInt64(Double(f.submitted) + flipDelay)
                lock.presented(f)
                s.shown += 1
                s.display.append(Double(f.presented - f.submitted))
                var ph = Double(Int64(f.presented) - Int64(vblank0)).truncatingRemainder(dividingBy: refresh)
                if ph > refresh / 2 { ph -= refresh } else if ph < -refresh / 2 { ph += refresh }
                s.flipPhase.append(ph)
            }
            pending.removeAll { $0.submitted < end }
        } else {
            var v = (Double(Int64(t) - Int64(vblank0)) / refresh).rounded(.up)
            while true {
                let vblank = UInt64(Double(vblank0) + v * refresh)
                guard vblank < end else { break }
                let ready = pending.filter { Double($0.submitted) <= Double(vblank) - cutoff }
                pending.removeAll { Double($0.submitted) <= Double(vblank) - cutoff }
                if let newest = ready.last {
                    for f in ready.dropLast() { lock.dropped(f); s.dropped += 1 }
                    newest.presented = UInt64(Double(vblank) + Double(depth) * refresh)
                    lock.presented(newest)
                    s.shown += 1
                    s.display.append(Double(newest.presented - newest.submitted))
                }
                v += 1
            }
        }
        t = end
        grid = lock.step()
        s.grid = grid
        return s
    }

    @discardableResult
    func steps(_ n: Int) -> [Step] { (0..<n).map { _ in step() } }
}

final class PhaseLockTests: XCTestCase {
    func testMeasuresTheRealRefreshFromFlips() {
        let lock = PhaseLock(refreshHz: 60)
        let refresh: UInt64 = 16_666_605, t0 = nowNs() + 1_000_000_000_000
        var t = t0
        XCTAssertNil(lock.grid())
        for i in 0..<70 {
            // Mostly one refresh apart, some two; one off-grid gap to ignore.
            t += i % 9 == 0 ? 2 * refresh : refresh
            if i == 30 { t += refresh / 2 }
            let f = timing(present: t - 20_000_000)
            f.submitted = t - 10_000_000
            f.presented = t
            lock.presented(f)
            if i == 40 { XCTAssertNil(lock.grid(), "fewer than 60 gaps") }
        }
        let g = try? XCTUnwrap(lock.grid())
        XCTAssertEqual(g?.interval, Double(refresh), "not the nominal 1/60 s")
        XCTAssertEqual(g?.vblank, t)
    }

    func testSweepsThenLocksJustAboveTheCutoffWithoutDrops() {
        let sim = Sim()
        let early = sim.steps(5)
        XCTAssertTrue(sim.lock.summary().contains("sweeping"), sim.lock.summary())
        XCTAssertGreaterThan(early.last!.shown, 25)
        let all = sim.steps(55)
        XCTAssertTrue(sim.lock.summary().contains("locked"), sim.lock.summary())
        let last = Array(all.suffix(10))
        let display = last.flatMap(\.display).sorted()
        let p50 = display[display.count / 2], p99 = display[display.count * 99 / 100]
        // On time: shown a refresh after the first vblank it makes, so above
        // refresh + cutoff, and close to it: not a whole refresh late.
        XCTAssertGreaterThan(p50, sim.refresh + sim.cutoff)
        XCTAssertLessThan(p99, sim.refresh + sim.cutoff + 2.5e6, "display p99 \(p99 / 1e6) ms: the lead is more than the jitter needs")
        XCTAssertLessThanOrEqual(last.map(\.dropped).reduce(0, +), 2)
        XCTAssertEqual(last.last!.grid?.interval, UInt64(sim.refresh), "the measured refresh goes to the host")
    }

    func testStaysLockedAndHoldsItsPhase() {
        let sim = Sim()
        sim.steps(40)
        let anchors = sim.steps(20).compactMap(\.grid).map { g -> Double in
            var ph = Double(Int64(g.anchor) - Int64(sim.vblank0)).truncatingRemainder(dividingBy: sim.refresh)
            if ph < 0 { ph += sim.refresh }
            return ph
        }
        let spread = anchors.max()! - anchors.min()!
        XCTAssertLessThan(spread, 1.5e6, "the grid's phase wanders \(spread / 1e6) ms")
    }

    func testFollowsTheNetworkGettingSlower() {
        let sim = Sim()
        sim.steps(40)
        sim.lateness += 4e6 // frames now arrive 4 ms later: below the cutoff where the lock sat
        let last = Array(sim.steps(40).suffix(10))
        let display = last.flatMap(\.display).sorted()
        let p99 = display[display.count * 99 / 100]
        XCTAssertLessThan(p99, sim.refresh + sim.cutoff + 5e6, "display p99 \(p99 / 1e6) ms: not back on time")
        XCTAssertLessThanOrEqual(last.map(\.dropped).reduce(0, +), 2)
    }

    func testDrainsWhenEveryFrameShowsARefreshLate() {
        let sim = Sim()
        sim.steps(40)
        XCTAssertFalse(sim.lock.takeDrain())
        sim.depth = 2 // the window server one frame deeper
        sim.steps(2)
        XCTAssertTrue(sim.lock.takeDrain())
        XCTAssertFalse(sim.lock.takeDrain(), "once")
    }

    func testVsyncOffSteersFlipsToJustBeforeTheVblank() {
        let sim = Sim()
        sim.steps(4) // vsync on long enough to measure the grid
        let measured = try? XCTUnwrap(sim.lock.grid())
        sim.immediate = true
        sim.lock.setImmediate(true)
        let steps = sim.steps(20)
        XCTAssertTrue(sim.lock.summary().contains("vsync off"), sim.lock.summary())
        let phase = steps.last!.medianFlipPhase
        XCTAssertEqual(phase, -PhaseLock.flipLead, accuracy: 0.2e6, "median flip \(phase / 1e6) ms from the vblank")
        let g = sim.lock.grid()
        XCTAssertEqual(g?.vblank, measured?.vblank, "immediate flips are not refreshes")
        XCTAssertEqual(g?.interval, measured?.interval)
        // Back to vsync: it sweeps again.
        sim.immediate = false
        sim.lock.setImmediate(false)
        sim.step()
        XCTAssertTrue(sim.lock.summary().contains("sweeping"), sim.lock.summary())
    }

    func testRestartOnAnotherScreenForgetsTheGrid() {
        let sim = Sim()
        sim.steps(4)
        XCTAssertNotNil(sim.lock.grid())
        sim.lock.restart(refreshHz: 120)
        XCTAssertNil(sim.lock.grid())
        XCTAssertTrue(sim.lock.summary().contains("waiting"), sim.lock.summary())
    }
}
