// What the macOS app knows of a display.

import AppKit

extension NSScreen {
    /// Adaptive-Sync: in full screen macOS refreshes it when a frame comes.
    var variableRefresh: Bool { maximumRefreshInterval > minimumRefreshInterval * 1.01 }
}

extension Pacing {
    /// VRR only in full screen: in a window macOS composites the desktop at
    /// a fixed rate.
    init(options: Options, fullScreen: Bool, screen: NSScreen?) {
        self.init(options: options, vrr: fullScreen && screen?.variableRefresh == true)
    }
}
