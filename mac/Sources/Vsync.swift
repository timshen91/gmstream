// The panel's vsync times. With vsync off in full screen, Metal's presented
// times are when each immediate flip happened, not refreshes, so the refresh
// grid has to come from elsewhere: CVDisplayLink, which is driven by the
// display's vblank, not by what this app presents (a CADisplayLink from the
// screen turned out to track our own flips).

import AppKit
import CoreVideo

final class VsyncClock {
    private let lock = NSLock()
    private var link: CVDisplayLink?
    private var lastVsync: UInt64 = 0   // this Mac's clock, ns
    private var interval: Double = 0    // ns

    func start(on screen: NSScreen) {
        if let link { CVDisplayLinkStop(link) }
        let id = (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
            ?? CGMainDisplayID()
        var l: CVDisplayLink?
        guard CVDisplayLinkCreateWithCGDisplay(id, &l) == kCVReturnSuccess, let l else { return }
        CVDisplayLinkSetOutputHandler(l) { [weak self] _, now, _, _, _ in
            // now: the vblank that just happened (host time, mach units).
            let t = now.pointee
            var tb = mach_timebase_info_data_t()
            mach_timebase_info(&tb)
            let ns = t.hostTime * UInt64(tb.numer) / UInt64(tb.denom)
            let period = Double(t.videoRefreshPeriod) / Double(t.videoTimeScale) * 1e9
            self?.lock.lock()
            self?.lastVsync = ns
            if period > 0 { self?.interval = period }
            self?.lock.unlock()
            return kCVReturnSuccess
        }
        CVDisplayLinkStart(l)
        link = l
    }

    /// The latest vsync and the interval, once known.
    func grid() -> (vsync: UInt64, interval: Double)? {
        lock.lock(); defer { lock.unlock() }
        return lastVsync > 0 && interval > 0 ? (lastVsync, interval) : nil
    }

}

/// The panel's real top refresh rate, from how close together frames reach
/// it while they come faster than it can show them. The Dell U2725QE
/// (Adaptive-Sync, nominally 48–120 Hz) tops out at 119.84 Hz (8.3443 ms): in
/// full screen with the host capped at the nominal 120.000 Hz, every flip
/// came exactly that far apart, frames queued in front of the panel (display
/// stage 6–8 ms instead of ~3.3) and one dropped every second or so, a
/// sawtooth no hold can flatten. Capping it 0.2% under was not enough: the
/// panel's VRR keeps refreshing on its own fastest ticks and only waits for a
/// frame that comes after the next one, so a frame ready a few µs early (all
/// of them, with sub-ms jitter) waited for the tick, and present → screen
/// walked through a whole refresh. Test frames in full screen: at 119.6 Hz
/// 98% of flips on the panel's ticks, at 118 45%, at 110 9%; present →
/// screen 3.3 ms p50, ~4 p90 from 117 Hz down. Hence a cap of the shortest
/// refresh plus 0.2 ms (117.0 Hz here). Measured once, while saturated: spacings
/// near the nominal interval (within 1%) and nearly all equal. Once the host
/// is capped below it, flips follow the host and say nothing about the panel.
final class PanelRate {
    private let lock = NSLock()
    private var last: UInt64 = 0
    private var spacings: [(at: UInt64, ns: Double)] = []

    func presented(_ t: UInt64) {
        lock.lock(); defer { lock.unlock() }
        if last > 0 && t > last { spacings.append((t, Double(t - last))) }
        last = t
        spacings.removeAll { $0.at &+ 2_000_000_000 < t }
    }

    func reset() {
        lock.lock(); defer { lock.unlock() }
        last = 0
        spacings = []
    }

    /// The top rate, Hz, if the last 2 s show the panel saturated a little
    /// below `nominalHz`.
    func top(nominalHz: Double) -> Double? {
        lock.lock(); defer { lock.unlock() }
        guard spacings.count >= Int(nominalHz) else { return nil }
        let s = spacings.map(\.ns).sorted()
        let nominal = 1e9 / nominalHz, low = s[s.count / 100], tenth = s[s.count / 10]
        guard low > nominal * 1.0003, low < nominal * 1.01, tenth - low < 20_000 else { return nil }
        return 1e9 / low
    }
}
