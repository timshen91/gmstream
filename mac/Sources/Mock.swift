// Measuring (--mock): test frames made on this Mac and handed straight to the
// renderer, with no host, network or decode, to measure the display path
// alone. A frame is "presented by gamescope" when it is made, so the total
// is made → on screen. Frames follow the display grid the phase lock would
// send the host, or else this Mac's clock at the stream's refresh rate.

import Metal

final class MockSource {
    private let lock = NSLock()
    private var grid: (anchor: UInt64, interval: Double)?
    private let interval: Double
    private let frames: [[MTLTexture]]

    init(device: MTLDevice, width: Int, height: Int, refreshHz: Double) {
        interval = 1e9 / refreshHz
        if getenv("GM_MOCK_COLORS") != nil {
            // A 4×2 card of known sRGB colors, BT.709 full range (color checks).
            let card: [(Double, Double, Double)] = [(200, 100, 50), (50, 150, 200), (100, 200, 80), (230, 200, 170),
                                                     (255, 0, 0), (0, 255, 0), (0, 0, 255), (128, 128, 128)]
            let ycc = card.map { c -> (Double, Double, Double) in
                let (r, g, b) = (c.0 / 255, c.1 / 255, c.2 / 255)
                let y = 0.2126 * r + 0.7152 * g + 0.0722 * b
                return (y, (b - y) / 1.8556 + 0.5, (r - y) / 1.5748 + 0.5)
            }
            let planes = [(width, height), (width / 2, height / 2), (width / 2, height / 2)].enumerated().map { p, size in
                let (w, h) = size
                var bytes = [UInt8](repeating: 0, count: w * h)
                for y in 0..<h {
                    for x in 0..<w {
                        let c = ycc[(y * 2 / h) * 4 + x * 4 / w]
                        bytes[y * w + x] = UInt8(max(0, min(255, ([c.0, c.1, c.2][p] * 255).rounded())))
                    }
                }
                let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: w, height: h, mipmapped: false)
                desc.usage = .shaderRead
                let t = device.makeTexture(descriptor: desc)!
                t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w)
                return t
            }
            frames = [planes]
            return
        }
        // Eight frames: a white bar moving across grey, BT.709 full range.
        frames = (0..<8).map { i in
            [(width, height, true), (width / 2, height / 2, false), (width / 2, height / 2, false)].map { w, h, luma in
                let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: w, height: h, mipmapped: false)
                desc.usage = .shaderRead
                let t = device.makeTexture(descriptor: desc)!
                var row = [UInt8](repeating: luma ? 96 : 128, count: w)
                if luma {
                    let x0 = i * w / 8
                    for x in x0..<min(w, x0 + w / 16) { row[x] = 255 }
                }
                let bytes = [UInt8]((0..<h).flatMap { _ in row })
                t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w)
                return t
            }
        }
    }

    /// The grid the phase lock would send the host (this Mac's clock).
    func setGrid(anchor: UInt64, interval: UInt64) {
        lock.lock()
        grid = interval > 0 ? (anchor, Double(interval)) : nil
        lock.unlock()
    }

    func start(submit: @escaping (DecodedImage, FrameTiming) -> Void) {
        let thread = Thread { [self] in
            var next = Double(nowNs()) + interval
            var i = 0
            while true {
                lock.lock()
                let g = grid
                lock.unlock()
                if let g {
                    // The first grid point after the last frame.
                    let k = ((next - Double(g.anchor)) / g.interval).rounded(.up)
                    next = Double(g.anchor) + k * g.interval
                }
                waitUntil(UInt64(next))
                let t = nowNs()
                let timing = FrameTiming(slotNs: t, encodedNs: t, received: t) { $0 }
                timing.decodeStart = t
                timing.decoded = t
                submit(.planes(frames[i % frames.count]), timing)
                i += 1
                next += g?.interval ?? interval
            }
        }
        thread.qualityOfService = .userInteractive
        thread.start()
    }
}

/// Sleeps until `ns` (CLOCK_UPTIME_RAW, the mach clock).
func waitUntil(_ ns: UInt64) {
    var tb = mach_timebase_info_data_t()
    mach_timebase_info(&tb)
    mach_wait_until(ns * UInt64(tb.denom) / UInt64(tb.numer))
}
