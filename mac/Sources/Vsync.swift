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
