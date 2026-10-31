import Carbon.HIToolbox
import XCTest

/// Annex B splitting, frame admission and the per-second and per-stage stats.
final class NalUnitTests: XCTestCase {
    func testThreeAndFourByteStartCodes() {
        let d = Data([0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 4, 5, 0])
        XCTAssertEqual(nalUnits(d), [Data([0x67, 1, 2]), Data([0x68, 3]), Data([0x65, 4, 5, 0])])
    }

    func testNoStartCodeNoUnits() {
        XCTAssertEqual(nalUnits(Data()), [])
        XCTAssertEqual(nalUnits(Data([1, 2, 3, 4])), [])
    }

    func testBackToBackStartCodesGiveAnEmptyUnitTheDecoderSkips() {
        XCTAssertEqual(nalUnits(Data([0, 0, 1, 0, 0, 1, 0x09, 0xF0])).filter { !$0.isEmpty }, [Data([0x09, 0xF0])])
    }
}

final class FrameGateTests: XCTestCase {
    func frame(_ id: UInt32, key: Bool = false) -> Frame {
        Frame(id: id, key: key, codec: 0, width: 16, height: 16, slotNs: 0, encodedNs: 0, bitstream: Data(), receivedNs: 0)
    }

    func testFromAKeyframeInOrder() {
        let gate = FrameGate(), stats = Stats()
        var requests = 0
        XCTAssertFalse(gate.admit(frame(9), requestKeyframe: { requests += 1 }, stats: stats), "no keyframe yet")
        XCTAssertEqual(requests, 1)
        for (id, key) in [(10, true), (11, false), (12, false)] {
            XCTAssertTrue(gate.admit(frame(UInt32(id), key: key), requestKeyframe: { requests += 1 }, stats: stats))
        }
        XCTAssertEqual(requests, 1)
        XCTAssertEqual(stats.take().gaps, 0)
    }

    func testAGapWaitsForAKeyframeAndAsksForOneAtMostEvery200ms() {
        let gate = FrameGate(), stats = Stats()
        var requests = 0
        let ask = { requests += 1 }
        XCTAssertTrue(gate.admit(frame(10, key: true), requestKeyframe: ask, stats: stats))
        XCTAssertFalse(gate.admit(frame(12), requestKeyframe: ask, stats: stats))
        XCTAssertEqual(requests, 1)
        XCTAssertFalse(gate.admit(frame(13), requestKeyframe: ask, stats: stats))
        XCTAssertEqual(requests, 1, "asked again too soon")
        XCTAssertEqual(stats.take().gaps, 1, "one gap, not one per frame after it")
        XCTAssertTrue(gate.admit(frame(14, key: true), requestKeyframe: ask, stats: stats))
        XCTAssertTrue(gate.admit(frame(15), requestKeyframe: ask, stats: stats))
        Thread.sleep(forTimeInterval: 0.21)
        XCTAssertFalse(gate.admit(frame(17), requestKeyframe: ask, stats: stats))
        XCTAssertEqual(requests, 2)
    }

    func testStaleFramesAreDroppedQuietly() {
        let gate = FrameGate(), stats = Stats()
        var requests = 0
        XCTAssertTrue(gate.admit(frame(10, key: true), requestKeyframe: { requests += 1 }, stats: stats))
        XCTAssertTrue(gate.admit(frame(11), requestKeyframe: { requests += 1 }, stats: stats))
        XCTAssertFalse(gate.admit(frame(9), requestKeyframe: { requests += 1 }, stats: stats))
        XCTAssertTrue(gate.admit(frame(12), requestKeyframe: { requests += 1 }, stats: stats), "still in order")
        XCTAssertEqual(requests, 0)
        XCTAssertEqual(stats.take().gaps, 0)
    }

    func testIdsWrapAround() {
        let gate = FrameGate(), stats = Stats()
        XCTAssertTrue(gate.admit(frame(.max, key: true), requestKeyframe: {}, stats: stats))
        XCTAssertTrue(gate.admit(frame(0), requestKeyframe: {}, stats: stats))
        XCTAssertFalse(gate.admit(frame(.max), requestKeyframe: {}, stats: stats), "before 0 across the wrap: stale")
    }
}

final class StatsTests: XCTestCase {
    func testTakeAveragesAndResets() {
        let s = Stats()
        s.add(bytes: 1_000_000, netMs: 2, totalMs: 10)
        s.add(bytes: 500_000, netMs: nil, totalMs: 20)
        s.addBytes(500_000)
        s.gap()
        let r = s.take()
        XCTAssertEqual(r.frames, 2)
        XCTAssertEqual(r.mbps, 16, accuracy: 1e-9)
        XCTAssertEqual(r.gaps, 1)
        XCTAssertEqual(r.net, 2)
        XCTAssertEqual(r.total, 15)
        XCTAssertEqual(r.totalMax, 20)
        let empty = s.take()
        XCTAssertEqual(empty.frames, 0)
        XCTAssertNil(empty.net)
        XCTAssertNil(empty.totalMax)
        XCTAssertEqual(empty.gaps, 1, "gaps count for the whole session")
    }
}

final class StageStatsTests: XCTestCase {
    /// A frame whose every stage takes a known time (ms), shown at `presented`.
    func frame(presentedAt presented: UInt64, present: UInt64) -> FrameTiming {
        let ms: UInt64 = 1_000_000
        let t = timing(present: present, encoded: present + 3 * ms / 2, received: present + 13 * ms / 2)
        t.decoded = t.received + 2 * ms
        t.released = t.decoded + ms / 2
        t.submitted = t.released + ms / 4
        t.presented = presented
        return t
    }

    func testStagesFromOneFrame() {
        let s = StageStats()
        let now = nowNs(), ms: UInt64 = 1_000_000
        let present = now - 30 * ms
        s.add(frame(presentedAt: present + 19 * ms, present: present))
        s.skip(); s.drop(); s.drop()
        let r = s.take()
        let p50 = Dictionary(uniqueKeysWithValues: r.rows.map { ($0.name, $0.p50) })
        XCTAssertEqual(r.rows.map(\.name), StageStats.stages)
        XCTAssertEqual(p50["encode"]!!, 1.5, accuracy: 1e-9)
        XCTAssertEqual(p50["network"]!!, 5, accuracy: 1e-9)
        XCTAssertEqual(p50["decode"]!!, 2, accuracy: 1e-9)
        XCTAssertEqual(p50["hold"]!!, 0.5, accuracy: 1e-9)
        XCTAssertEqual(p50["render"]!!, 0.25, accuracy: 1e-9)
        XCTAssertEqual(p50["to macOS"]!!, 9.25, accuracy: 1e-9)
        XCTAssertEqual(p50["display"]!!, 9.75, accuracy: 1e-9)
        XCTAssertEqual(p50["total"]!!, 19, accuracy: 1e-9)
        XCTAssertNil(p50["spacing"]!, "needs a previous frame")
        XCTAssertEqual(r.shown, 1)
        XCTAssertEqual(r.skipped, 1)
        XCTAssertEqual(r.dropped, 2)
        let again = s.take()
        XCTAssertEqual(again.shown + again.skipped + again.dropped, 0, "counts reset; rows keep 5 s")
        XCTAssertNotNil(again.rows[0].p50)
    }

    func testSpacingIsTheChangeInFrameIntervals() {
        let s = StageStats()
        let now = nowNs(), ms: UInt64 = 1_000_000
        let p0 = now - 100 * ms
        s.add(frame(presentedAt: p0 + 20 * ms, present: p0))
        // Presented 1 ms further apart than the host presented them.
        s.add(frame(presentedAt: p0 + 20 * ms + 17_666_667, present: p0 + 16_666_667))
        let spacing = s.take().rows.first { $0.name == "spacing" }!
        XCTAssertEqual(spacing.p50!, 1, accuracy: 1e-6)
    }

    func testPercentilesAndTheFiveSecondWindow() {
        let s = StageStats()
        let now = nowNs(), ms: UInt64 = 1_000_000
        s.add(frame(presentedAt: now - 6_000 * ms, present: now - 6_100 * ms)) // out of the window
        for i in 0..<10 {
            let present = now - 1_000 * ms + UInt64(i) * 17 * ms
            s.add(frame(presentedAt: present + UInt64(20 + i) * ms, present: present))
        }
        let total = s.take().rows.first { $0.name == "total" }!
        XCTAssertEqual(total.p50!, 25, accuracy: 1e-9)
        XCTAssertEqual(total.p99!, 29, accuracy: 1e-9)
        XCTAssertEqual(total.max!, 29, accuracy: 1e-9)
    }
}

final class PanelRateTests: XCTestCase {
    func feed(_ p: PanelRate, spacing: (Int) -> Double, count: Int) {
        var t: UInt64 = 1_000_000_000
        for i in 0..<count {
            p.presented(t)
            t += UInt64(spacing(i))
        }
    }

    func testSaturatedPanelGivesItsTopRate() {
        let p = PanelRate()
        feed(p, spacing: { _ in 8_344_300 }, count: 200)
        XCTAssertEqual(p.top(nominalHz: 120)!, 1e9 / 8_344_300, accuracy: 0.001)
    }

    func testFramesAtTheNominalRateOrSlowerSayNothing() {
        let exact = PanelRate()
        feed(exact, spacing: { _ in 8_333_333 }, count: 200)
        XCTAssertNil(exact.top(nominalHz: 120))
        let slow = PanelRate()
        feed(slow, spacing: { _ in 10_000_000 }, count: 150)
        XCTAssertNil(slow.top(nominalHz: 120), "host-paced, not saturated")
    }

    func testTooFewOrUnevenSpacingsSayNothing() {
        let few = PanelRate()
        feed(few, spacing: { _ in 8_344_300 }, count: 100)
        XCTAssertNil(few.top(nominalHz: 120))
        let uneven = PanelRate()
        feed(uneven, spacing: { 8_344_300 + Double(($0 * 7919) % 200) * 5000 }, count: 200)
        XCTAssertNil(uneven.top(nominalHz: 120))
    }

    func testOnlyTheLastTwoSecondsAndReset() {
        let p = PanelRate()
        feed(p, spacing: { $0 < 200 ? 8_344_300 : 10_000_000 }, count: 450) // then 2.5 s host-paced
        XCTAssertNil(p.top(nominalHz: 120))
        let q = PanelRate()
        feed(q, spacing: { _ in 8_344_300 }, count: 200)
        q.reset()
        XCTAssertNil(q.top(nominalHz: 120))
    }
}

final class HoldTests: XCTestCase {
    let ms: Int64 = 1_000_000

    /// A frame the host presented `late` ns before now.
    func arriving(late: Int64) -> FrameTiming {
        timing(present: UInt64(Int64(nowNs()) - late))
    }

    func testOffHoldsNothing() {
        let h = Hold()
        h.arrived(arriving(late: 10 * ms))
        XCTAssertNil(h.schedule(arriving(late: 10 * ms)))
        XCTAssertNil(h.status(offset: 0).delayNs)
    }

    /// Two seconds of frames 10 ms late, every tenth 11 ms: the constant is
    /// set at their 99th percentile plus 0.3 ms.
    func settled() -> Hold {
        let h = Hold()
        h.enabled = true
        var i = 0
        let end = nowNs() + 2_050_000_000
        while nowNs() < end {
            h.arrived(arriving(late: i % 10 == 0 ? 11 * ms : 10 * ms))
            i += 1
            Thread.sleep(forTimeInterval: 0.008)
        }
        h.arrived(arriving(late: 10 * ms))
        return h
    }

    func testSettlesOnTheTailAndSchedulesFramesAConstantAfterTheHost() {
        let h = settled()
        let t = timing(present: 5_000_000_000)
        let due = try? XCTUnwrap(h.schedule(t))
        XCTAssertEqual(Double(Int64(due ?? 0) - 5_000_000_000), Double(11 * ms + 300_000), accuracy: 0.2e6)
        XCTAssertEqual(h.status(offset: 1_000).delayNs ?? 0, Double(Int64(due ?? 0) - 5_000_000_000 + 1_000), accuracy: 1)
        XCTAssertNil(h.status(offset: nil).delayNs, "shown only with the clock offset")

        // Never two presents closer than the minimum spacing.
        h.minSpacing = 8_300_000
        let first = h.schedule(timing(present: 6_000_000_000))!
        let second = h.schedule(timing(present: 6_001_000_000))!
        XCTAssertEqual(second - first, 8_300_000)
    }

    func testLateFramesCountAndThreeSpreadOnesRaiseTheConstant() {
        let h = settled()
        let before = h.schedule(timing(present: 1_000_000_000))! - 1_000_000_000
        h.arrived(arriving(late: 20 * ms))
        XCTAssertEqual(h.status(offset: 0).late, 1)
        XCTAssertEqual(h.status(offset: 0).late, 0, "counted since the last call")
        let still = h.schedule(timing(present: 2_000_000_000))! - 2_000_000_000
        XCTAssertEqual(still, before, "one late frame moves nothing")
        for _ in 0..<2 {
            Thread.sleep(forTimeInterval: 0.06)
            h.arrived(arriving(late: 20 * ms))
        }
        let after = h.schedule(timing(present: 3_000_000_000))! - 3_000_000_000
        XCTAssertGreaterThan(after, UInt64(20 * ms), "up to the new tail at once")
    }

    func testTurningItOffForgetsTheConstant() {
        let h = settled()
        XCTAssertNotNil(h.schedule(timing(present: 1)))
        h.enabled = false
        h.enabled = true
        XCTAssertNil(h.schedule(timing(present: 1)))
    }
}

final class KeysTests: XCTestCase {
    func testCommonKeys() {
        let expected: [Int: UInt16] = [
            kVK_ANSI_A: 30, kVK_ANSI_Z: 44, kVK_ANSI_1: 2, kVK_ANSI_0: 11, kVK_Return: 28, kVK_Escape: 1,
            kVK_Space: 57, kVK_Tab: 15, kVK_Delete: 14, kVK_ForwardDelete: 111, kVK_LeftArrow: 105,
            kVK_RightArrow: 106, kVK_UpArrow: 103, kVK_DownArrow: 108, kVK_F1: 59, kVK_F12: 88,
            kVK_Shift: 42, kVK_RightShift: 54, kVK_Control: 29, kVK_Option: 56, kVK_Command: 125,
            kVK_RightCommand: 126, kVK_CapsLock: 58,
        ]
        for (mac, evdev) in expected {
            XCTAssertEqual(evdevKeys[UInt16(mac)], evdev, "macOS key \(mac)")
        }
    }

    func testOneMacKeyPerLinuxKey() {
        XCTAssertEqual(Set(evdevKeys.values).count, evdevKeys.count)
    }

    func testEveryModifierIsMapped() {
        for k in modifierKeys { XCTAssertNotNil(evdevKeys[k], "modifier \(k)") }
    }
}
