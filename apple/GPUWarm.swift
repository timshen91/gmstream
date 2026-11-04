// Keeps an Apple GPU's clock up while PyroWave frames are being decoded.
//
// The GPU's power management sets its clock by how busy it is, and decoding a
// frame stripe by stripe as it arrives keeps it lightly busy in short bursts:
// it then runs every decode at a fraction of its speed (measured on an M4,
// 2560x1600 at 120 Hz in 7 stripes: inverse transform 0.39 ms a stripe against
// 0.12 with the clock up, the last stripe decoded 2.4 ms after its last packet
// against 0.6). A tiny kernel, submitted back to back on a low priority queue so
// the GPU is never idle, holds the clock up; it does almost no work, and the
// decodes go first. Like Keep Wi-Fi awake: some power for latency, only while
// frames arrive (it stops a second after the last).

import Metal

final class GPUWarmer {
    private let queue: MTLCommandQueue
    private let pipeline: MTLComputePipelineState
    private let buffer: MTLBuffer
    private let lock = NSCondition()
    private var lastTouch: UInt64 = 0
    private var running = false
    private static let idleNs: UInt64 = 1_000_000_000

    init?(device: MTLDevice) {
        let source = """
        #include <metal_stdlib>
        using namespace metal;
        kernel void gm_warm(device float *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
            float v = float(i);
            for (int k = 0; k < 100; k++) v = fma(v, 1.0001, 0.5);
            if (v == 12345.0) out[0] = v; // never: keeps the loop
        }
        """
        guard let library = try? device.makeLibrary(source: source, options: nil),
              let function = library.makeFunction(name: "gm_warm"),
              let pipeline = try? device.makeComputePipelineState(function: function),
              let queue = device.makeCommandQueue(), let buffer = device.makeBuffer(length: 16) else { return nil }
        self.queue = queue
        self.pipeline = pipeline
        self.buffer = buffer
        // Metal's queue priority (private; 0 is the lowest a foreground app may
        // set), so the filler always yields to the decodes.
        typealias SetPriority = @convention(c) (AnyObject, Selector, UInt) -> Void
        let sel = NSSelectorFromString("setGPUPriority:")
        let q = queue as AnyObject
        if q.responds(to: sel) { unsafeBitCast(q.method(for: sel), to: SetPriority.self)(q, sel, 0) }
    }

    /// A frame is arriving: keep the clock up for the next second.
    func touch() {
        lock.lock()
        lastTouch = nowNs()
        if !running {
            running = true
            let t = Thread { [weak self] in self?.run() }
            t.qualityOfService = .userInitiated
            t.name = "GMStream GPU warmer"
            t.start()
        }
        lock.unlock()
    }

    private func run() {
        // Two command buffers in flight, so the GPU never waits for this thread.
        let inFlight = DispatchSemaphore(value: 2)
        while true {
            lock.lock()
            if nowNs() &- lastTouch > Self.idleNs {
                running = false
                lock.unlock()
                return
            }
            lock.unlock()
            inFlight.wait()
            autoreleasepool {
                guard let cb = queue.makeCommandBuffer(), let e = cb.makeComputeCommandEncoder() else {
                    inFlight.signal()
                    return
                }
                e.setComputePipelineState(pipeline)
                e.setBuffer(buffer, offset: 0, index: 0)
                e.dispatchThreads(MTLSize(width: 256, height: 1, depth: 1),
                                  threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
                e.endEncoding()
                cb.addCompletedHandler { _ in inFlight.signal() }
                cb.commit()
            }
        }
    }
}
