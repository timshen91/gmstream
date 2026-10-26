// PyroWave decode (the Metal port of github.com/Themaister/pyrowave): each
// frame arrives as independent packets, one per datagram; once all of a
// frame's packets are in, it is decoded on the GPU into Y, Cb and Cr
// textures and handed to the renderer.

import Metal
import QuartzCore

final class PyroWaveDecoder {
    /// Called with each decoded frame (on a Metal completion thread).
    var onDecoded: ([MTLTexture], FrameTiming) -> Void = { _, _ in }
    /// Frames given up on because a newer one started before they completed.
    var onIncomplete: () -> Void = {}

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pw: pyrowave_device
    private var decoder: pyrowave_decoder?
    private var width = 0, height = 0
    // Planes rotate so a frame being decoded never overwrites one still on
    // screen (the renderer holds at most a few).
    private var ring: [[MTLTexture]] = []
    private var ringIndex = 0
    // The frame being assembled (network thread only).
    private var frameId: UInt32?
    private var got = 0

    init?(device: MTLDevice, queue: MTLCommandQueue) {
        let mtl = Unmanaged.passUnretained(device as AnyObject).toOpaque()
        guard pyrowave_device_is_supported(mtl) else { return nil }
        var info = pyrowave_device_create_info(mtl_device: mtl, message_callback: nil, message_userdata: nil)
        var pw: pyrowave_device?
        guard pyrowave_device_create(&info, &pw) == PYROWAVE_SUCCESS, let pw else { return nil }
        self.device = device
        self.queue = queue
        self.pw = pw
    }

    /// One packet (network thread). `timing` builds the frame's timing record.
    func packet(_ p: PyroWavePacket, timing: () -> FrameTiming) {
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
        decode(decoder, timing())
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
        width = w; height = h
        ring = (0..<4).map { _ in
            [(w, h), (w / 2, h / 2), (w / 2, h / 2)].map { pw, ph in
                let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: pw, height: ph, mipmapped: false)
                desc.usage = [.shaderWrite, .shaderRead]
                desc.storageMode = .private
                return device.makeTexture(descriptor: desc)!
            }
        }
        return true
    }

    private func decode(_ decoder: pyrowave_decoder, _ timing: FrameTiming) {
        let planes = ring[ringIndex]
        ringIndex = (ringIndex + 1) % ring.count
        guard let cb = queue.makeCommandBuffer() else { return }
        timing.decodeStart = nowNs()
        let ptr = { (o: AnyObject) in Unmanaged.passUnretained(o).toOpaque() }
        var buffers = pyrowave_gpu_buffers(planes: (ptr(planes[0]), ptr(planes[1]), ptr(planes[2])))
        guard pyrowave_decoder_decode_gpu_buffer(decoder, ptr(cb), &buffers) == PYROWAVE_SUCCESS else { return }
        let committed = CACurrentMediaTime()
        cb.addCompletedHandler { [weak self] cb in
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
}
