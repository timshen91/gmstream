// PyroWave decode (the Metal port of github.com/Themaister/pyrowave): each
// frame arrives as independent packets, one per datagram; once all of a
// frame's packets are in, it is decoded on the GPU into Y, Cb and Cr
// textures and handed to the renderer.
//
// With inter frames (PROTOCOL.md, "PyroWave inter frames") a frame is the
// difference from a prediction from the previous decoded frame, so every frame
// is decoded, in order, whatever arrived of it, and what did not arrive is
// reported: the host replays this decoder and predicts the next frames from
// what it actually has. The frame comes in stripes, horizontal bands each coded
// as an image of its own (a decoder each) that predicts from the whole previous
// frame; the host sends each as soon as its GPU has finished it, and each is
// decoded as soon as it has arrived, into its rows of the frame, while the next
// is still on the way.

import Metal
import QuartzCore

final class PyroWaveDecoder {
    /// Called with each decoded frame (on a Metal completion thread).
    var onDecoded: ([MTLTexture], FrameTiming) -> Void = { _, _ in }
    /// Frames given up on because a newer one started before they completed
    /// (inter frames: decoded with what arrived, and reported).
    var onIncomplete: () -> Void = {}
    /// Inter frames: packets of a frame that never arrived (frame id, the
    /// frame's packet count as known, missing (first, length) ranges), sent
    /// again until the host says it took them in.
    var onLoss: (UInt32, Int, [(Int, Int)]) -> Void = { _, _, _ in }
    /// Inter frames: there is nothing to predict from; the host should send an
    /// intra frame.
    var onNeedIntra: () -> Void = {}
    /// Keep the GPU's clock up while frames arrive (GPUWarmer).
    var keepGPUAwake = false
    private lazy var warmer = GPUWarmer(device: device)

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pw: pyrowave_device
    private var decoder: pyrowave_decoder?
    private var width = 0, height = 0
    // Planes rotate so a frame being decoded never overwrites one still on
    // screen (the renderer holds at most a few), nor the reference.
    private var ring: [[MTLTexture]] = []
    private var ringIndex = 0
    // The frame being assembled (network thread only).
    private var frameId: UInt32?
    private var got = 0

    // Inter frames (network thread only).
    private struct InterStripe {
        var first = -1, count = 0 // its packet numbers, once one has arrived
        var inter = 0             // its first packets that are inter packets
        var received: [Bool] = []
        var receivedCount = 0
        var prepared = false      // its prediction formed ahead of its coefficients
    }
    private struct InterFrame {
        let id: UInt32
        let predicted: Bool
        var stripes: [InterStripe]
        var next = 0        // the next stripe to decode
        var maxSeen = -1    // the latest stripe a packet of has arrived
        var timing: FrameTiming?
        var firstReceived: UInt64 = 0, firstEncoded: UInt64 = 0
        var decodeStart: UInt64 = 0 // the first stripe's (timing is made afresh with each packet)
        var lost = false
        /// Stripe k is as complete as it gets: all in, its last packet in (the
        /// host sends a stripe's packets in order), or a later stripe started.
        func ready(_ k: Int) -> Bool {
            let s = stripes[k]
            return (s.first >= 0 && (s.receivedCount == s.count || s.received[s.count - 1])) || maxSeen > k
        }
        var complete: Bool { stripes.allSatisfy { $0.first >= 0 && $0.receivedCount == $0.count } }
        /// What did not arrive, as (first, length) over the frame's packet
        /// numbers; a length of 0xffff is the rest. The frame's packet count if known.
        var missing: (count: Int, ranges: [(Int, Int)]) {
            let last = stripes[stripes.count - 1]
            let total = last.first >= 0 ? last.first + last.count : 0
            var known = 0
            for s in stripes where s.first >= 0 { known = max(known, s.first + s.count) }
            var got = [Bool](repeating: false, count: max(total, known))
            for s in stripes where s.first >= 0 {
                for i in 0..<s.count where s.received[i] { got[s.first + i] = true }
            }
            var ranges: [(Int, Int)] = []
            var i = 0
            while i < got.count {
                if got[i] { i += 1; continue }
                let start = i
                while i < got.count && !got[i] { i += 1 }
                ranges.append((start, i - start))
            }
            if total == 0 { ranges.append((got.count, 0xffff)) } // the last stripe never came: the rest too
            return (total, ranges)
        }
    }
    private var interFrame: InterFrame?
    private var lastDecoded: UInt32? // the newest frame decoded in the inter sequence
    private var reference: Int?      // its planes in the ring
    private var waitingForIntra = false
    private var pending: [UInt32: (Int, [(Int, Int)])] = [:] // reports the host has not taken in
    // The stripes: a decoder each, each writing its rows of the frame's planes.
    private var stripeRows = 0, firstRows = 0
    private var stripeDecoders: [pyrowave_decoder] = []
    private var stripeY: [Int] = []
    private var current: Int? // the frame being decoded's planes in the ring
    /// The most frames decoded from nothing to bridge a gap; the host keeps
    /// about this many to replay.
    private static let maxBridge = 6

    init?(device: MTLDevice, queue: MTLCommandQueue) {
        let mtl = Unmanaged.passUnretained(device as AnyObject).toOpaque()
        guard pyrowave_device_is_supported(mtl) else { return nil }
        var info = pyrowave_device_create_info(mtl_device: mtl, message_callback: nil, message_userdata: nil)
        var pw: pyrowave_device?
        guard pyrowave_device_create(&info, &pw) == PYROWAVE_SUCCESS, let pw else { return nil }
        self.device = device
        self.queue = queue
        self.pw = pw
        if let v = getenv("GM_GPU_PRIORITY").map({ UInt(atoi($0)) }) {
            // Measuring: Metal's private queue priority (0 ... 2 for a foreground app, 1 by default).
            typealias Set = @convention(c) (AnyObject, Selector, UInt) -> Void
            let sel = NSSelectorFromString("setGPUPriority:")
            let q = queue as AnyObject
            if q.responds(to: sel) { unsafeBitCast(q.method(for: sel), to: Set.self)(q, sel, v) }
        }
    }

    /// One packet (network thread). `timing` builds the frame's timing record,
    /// given when its first packet arrived and how much earlier than the rest it
    /// was sent (inter frames send their inter packets ahead; else 0, 0).
    func packet(_ p: PyroWavePacket, timing: (_ firstReceived: UInt64, _ overlapNs: UInt64) -> FrameTiming) {
        if keepGPUAwake { warmer?.touch() }
        if p.inter {
            interPacket(p, timing: timing)
            return
        }
        if let id = frameId, Int32(bitPattern: p.frameId &- id) < 0 { return } // stale
        if p.frameId != frameId {
            if frameId != nil && got > 0 { onIncomplete() }
            frameId = p.frameId
            got = 0
            guard ensureDecoder(p.width, p.height) else { return }
        }
        guard let decoder else { return }
        p.payload.withUnsafeBytes { _ = pyrowave_decoder_push_packet(decoder, $0.baseAddress, $0.count) }
        got += 1
        guard got == p.count else { return }
        got = 0
        decode(decoder, timing(0, 0))
    }

    private func ensureDecoder(_ w: Int, _ h: Int) -> Bool {
        if decoder != nil && w == width && h == height { return true }
        if let decoder { pyrowave_decoder_destroy(decoder) }
        decoder = nil
        var info = pyrowave_decoder_create_info(device: pw, width: Int32(w), height: Int32(h),
                                                chroma: PYROWAVE_CHROMA_SUBSAMPLING_420)
        var d: pyrowave_decoder?
        guard pyrowave_decoder_create(&info, &d) == PYROWAVE_SUCCESS, let d else { return false }
        decoder = d
        ensureRing(w, h)
        return true
    }

    /// The frames' planes, for frames of this size.
    private func ensureRing(_ w: Int, _ h: Int) {
        if !ring.isEmpty && w == width && h == height { return }
        width = w; height = h
        ring = (0..<10).map { _ in
            [(w, h), (w / 2, h / 2), (w / 2, h / 2)].map { pw, ph in
                let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: pw, height: ph, mipmapped: false)
                desc.usage = [.shaderWrite, .shaderRead]
                desc.storageMode = .private
                return device.makeTexture(descriptor: desc)!
            }
        }
        // New planes have nothing to predict from.
        reference = nil
        lastDecoded = nil
        interFrame = nil
    }

    private func decode(_ decoder: pyrowave_decoder, _ timing: FrameTiming?, show: Bool = true) {
        let index = ringIndex
        let planes = ring[index]
        ringIndex = (ringIndex + 1) % ring.count
        guard let cb = queue.makeCommandBuffer() else { return }
        timing?.decodeStart = nowNs()
        var buffers = pyrowave_gpu_buffers(planes: (ptr(planes[0]), ptr(planes[1]), ptr(planes[2])))
        guard pyrowave_decoder_decode_gpu_buffer(decoder, ptr(cb), &buffers) == PYROWAVE_SUCCESS else { return }
        reference = index
        let committed = CACurrentMediaTime()
        cb.addCompletedHandler { [weak self] cb in
            guard show, let timing else { return }
            timing.decoded = nowNs()
            if getenv("GM_DEBUG_DECODE") != nil {
                // Measuring: where the decode stage goes.
                let done = CACurrentMediaTime()
                FileHandle.standardError.write(String(format: "decode queue %.2f gpu %.2f handler %.2f ms\n",
                    (cb.gpuStartTime - committed) * 1e3, (cb.gpuEndTime - cb.gpuStartTime) * 1e3,
                    (done - cb.gpuEndTime) * 1e3).data(using: .utf8)!)
            }
            self?.onDecoded(planes, timing)
        }
        cb.commit()
    }

    private func ptr(_ o: AnyObject) -> UnsafeMutableRawPointer { Unmanaged.passUnretained(o).toOpaque() }

    /// Measuring (PYROWAVE_TIMESTAMPS=1): the decoder's GPU time per pass since
    /// the last call, to stderr.
    func reportPerformance() {
        pyrowave_device_report_performance_stats(pw, { _, msg in
            if let msg { FileHandle.standardError.write("pyrowave: \(String(cString: msg))\n".data(using: .utf8)!) }
        }, nil, true)
    }

    // MARK: inter frames

    private func interPacket(_ p: PyroWavePacket, timing: (UInt64, UInt64) -> FrameTiming) {
        // Reports the host has taken in, or made moot by starting over; the rest
        // again, once a frame.
        pending = pending.filter { Int32(bitPattern: $0.key &- p.repaired) > 0 }
        if interFrame?.id != p.frameId && (lastDecoded.map { Int32(bitPattern: p.frameId &- $0) > 0 } ?? true) {
            for (id, r) in pending { onLoss(id, r.0, r.1) }
        }
        if var f = interFrame, f.id != p.frameId {
            guard Int32(bitPattern: p.frameId &- f.id) > 0 else { return } // late: counted lost already
            interFrame = nil
            finish(&f) // a newer frame has started: this one is as complete as it gets
        }
        if interFrame == nil {
            if let last = lastDecoded, Int32(bitPattern: p.frameId &- last) <= 0 { return } // late
            guard start(p) else { return }
        }
        guard var f = interFrame, p.stripe < f.stripes.count, p.stripeCount > 0,
              p.index >= p.stripeFirst, p.index < p.stripeFirst + p.stripeCount else { return }
        let k = p.stripe
        if f.stripes[k].first < 0 {
            f.stripes[k].first = p.stripeFirst
            f.stripes[k].count = p.stripeCount
            f.stripes[k].inter = min(p.stripeInter, p.stripeCount)
            f.stripes[k].received = [Bool](repeating: false, count: p.stripeCount)
        }
        let i = p.index - f.stripes[k].first
        guard k >= f.next, i < f.stripes[k].count, !f.stripes[k].received[i] else { return } // late or a duplicate
        f.stripes[k].received[i] = true
        f.stripes[k].receivedCount += 1
        f.maxSeen = max(f.maxSeen, k)
        p.payload.withUnsafeBytes { _ = pyrowave_decoder_push_packet(stripeDecoders[k], $0.baseAddress, $0.count) }
        // The stripe's inter packets all in: its prediction now, while its
        // coefficients arrive.
        let st = f.stripes[k]
        if f.predicted && !st.prepared && st.inter > 0 && st.receivedCount < st.count &&
           st.received[0..<st.inter].allSatisfy({ $0 }), let cb = queue.makeCommandBuffer(),
           pyrowave_decoder_prepare_prediction(stripeDecoders[k], ptr(cb)) == PYROWAVE_SUCCESS {
            cb.commit()
            f.stripes[k].prepared = true
        }
        if f.firstReceived == 0 { f.firstReceived = p.receivedNs; f.firstEncoded = p.encodedNs }
        f.timing = timing(f.firstReceived, p.encodedNs > f.firstEncoded ? p.encodedNs - f.firstEncoded : 0)
        // Each stripe as soon as it is in, in order.
        while f.next < f.stripes.count && f.ready(f.next) {
            decodeStripe(&f, f.next)
            f.next += 1
        }
        if f.next == f.stripes.count {
            interFrame = nil
            done(f)
        } else {
            interFrame = f
        }
    }

    /// The stripes' decoders and planes for a frame of this size.
    private func ensureStripes(_ p: PyroWavePacket) -> Bool {
        let resized = ring.isEmpty || p.width != width || p.height != height
        ensureRing(p.width, p.height)
        let rows = p.stripes > 1 ? p.stripeRows : 0, first = p.stripes > 1 ? p.firstRows : 0
        if !resized && stripeDecoders.count == p.stripes && stripeRows == rows && firstRows == first { return true }
        for d in stripeDecoders { pyrowave_decoder_destroy(d) }
        stripeDecoders = []
        stripeY = []
        reference = nil
        lastDecoded = nil
        for k in 0..<p.stripes {
            let y = k == 0 ? 0 : first + (k - 1) * rows
            let h = k + 1 == p.stripes ? p.height - y : k == 0 ? first : rows
            guard h >= 2, h % 2 == 0 else { return false }
            var info = pyrowave_decoder_create_info(device: pw, width: Int32(p.width), height: Int32(h),
                                                    chroma: PYROWAVE_CHROMA_SUBSAMPLING_420)
            var d: pyrowave_decoder?
            guard pyrowave_decoder_create(&info, &d) == PYROWAVE_SUCCESS, let d else { return false }
            stripeDecoders.append(d)
            stripeY.append(y)
        }
        stripeRows = rows
        firstRows = first
        return true
    }

    /// The first packet of a frame: bridges any frames that never arrived, and
    /// sets up the decode. False to drop the frame (nothing to predict it from).
    private func start(_ p: PyroWavePacket) -> Bool {
        guard ensureStripes(p) else { return false }
        if !p.predicted {
            waitingForIntra = false
            pending.removeAll() // starting over: nothing to repair
        } else if waitingForIntra {
            return false
        } else if reference == nil || lastDecoded == nil {
            needIntra()
            return false
        } else if let last = lastDecoded {
            let gap = Int(p.frameId &- last) - 1
            if gap > Self.maxBridge {
                needIntra()
                return false
            }
            // Frames none of whose packets arrived: decoded from nothing (last
            // frame's motion), as the host will replay them, and reported.
            for k in 0..<gap {
                let id = last &+ UInt32(k + 1)
                var f = InterFrame(id: id, predicted: true, stripes: Array(repeating: InterStripe(), count: stripeDecoders.count))
                beginFrame(predicted: true)
                for s in 0..<f.stripes.count { decodeStripe(&f, s) }
                endFrame(nil)
                lastDecoded = id
                report(id, 0, [(0, 0xffff)])
            }
        }
        beginFrame(predicted: p.predicted)
        interFrame = InterFrame(id: p.frameId, predicted: p.predicted,
                                stripes: Array(repeating: InterStripe(), count: stripeDecoders.count))
        return true
    }

    /// A frame's planes in the ring, and its stripes' decoders cleared and
    /// given the previous frame to predict from (predicted: true).
    private func beginFrame(predicted: Bool) {
        current = ringIndex
        ringIndex = (ringIndex + 1) % ring.count
        for (k, d) in stripeDecoders.enumerated() {
            pyrowave_decoder_clear(d)
            if predicted, let reference {
                let r = ring[reference]
                var buffers = pyrowave_gpu_buffers(planes: (ptr(r[0]), ptr(r[1]), ptr(r[2])))
                pyrowave_decoder_set_reference_stripe(d, &buffers, Int32(stripeY[k]))
            } else {
                pyrowave_decoder_set_reference_stripe(d, nil, Int32(stripeY[k]))
            }
        }
    }

    /// Stripe k, from what arrived of it (nothing: from last frame's motion),
    /// into its rows of the frame.
    private func decodeStripe(_ f: inout InterFrame, _ k: Int) {
        guard let current, let cb = queue.makeCommandBuffer() else { return }
        let d = stripeDecoders[k] // its reference set when the frame began
        if k == 0 { f.decodeStart = nowNs() }
        // Straight into the stripe's rows of the frame.
        let frame = ring[current]
        var buffers = pyrowave_gpu_buffers(planes: (ptr(frame[0]), ptr(frame[1]), ptr(frame[2])))
        let decoded = pyrowave_decoder_decode_gpu_buffer_rows(d, ptr(cb), &buffers, Int32(stripeY[k])) == PYROWAVE_SUCCESS
        if !decoded || f.stripes[k].first < 0 || f.stripes[k].receivedCount < f.stripes[k].count { f.lost = true }
        if k + 1 == f.stripes.count {
            f.timing?.decodeStart = f.decodeStart
            endFrame(f.timing, cb)
        } else {
            cb.commit()
        }
    }

    /// The frame is decoded once this command buffer is: the next one's reference,
    /// and shown (with a timing; frames bridged over are not).
    private func endFrame(_ timing: FrameTiming?, _ cb: MTLCommandBuffer? = nil) {
        guard let index = current else { return }
        reference = index
        current = nil
        guard let cb = cb ?? queue.makeCommandBuffer() else { return }
        let planes = ring[index]
        let committed = CACurrentMediaTime()
        cb.addCompletedHandler { [weak self] cb in
            guard let timing else { return }
            timing.decoded = nowNs()
            if getenv("GM_DEBUG_DECODE") != nil {
                // Measuring: where the last stripe's decode goes.
                let done = CACurrentMediaTime()
                FileHandle.standardError.write(String(format: "decode queue %.2f gpu %.2f handler %.2f ms\n",
                    (cb.gpuStartTime - committed) * 1e3, (cb.gpuEndTime - cb.gpuStartTime) * 1e3,
                    (done - cb.gpuEndTime) * 1e3).data(using: .utf8)!)
            }
            self?.onDecoded(planes, timing)
        }
        cb.commit()
    }

    /// A newer frame has started: the rest of this one as it is.
    private func finish(_ f: inout InterFrame) {
        f.maxSeen = f.stripes.count
        while f.next < f.stripes.count {
            decodeStripe(&f, f.next)
            f.next += 1
        }
        done(f)
    }

    private func done(_ f: InterFrame) {
        lastDecoded = f.id
        if f.lost {
            onIncomplete()
            let m = f.missing
            report(f.id, m.count, m.ranges)
        }
    }

    private func report(_ id: UInt32, _ count: Int, _ ranges: [(Int, Int)]) {
        pending[id] = (count, ranges)
        onLoss(id, count, ranges)
    }

    private func needIntra() {
        waitingForIntra = true
        interFrame = nil
        pending.removeAll()
        onNeedIntra()
    }
}
