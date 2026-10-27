// Video: the host's Annex B bitstream (start codes, in-band parameter sets on
// keyframes) → VideoToolbox hardware decode → a Metal layer, presented on the
// next display refresh. Each frame carries timestamps through every stage, and
// Metal reports when it actually reached the screen.

import AppKit
import CoreMedia
import Metal
import QuartzCore
import VideoToolbox

enum Codec: UInt8 {
    case h264 = 0, hevc = 1
}

/// Times (this Mac's CLOCK_UPTIME_RAW ns, i.e. mach time) one frame passes
/// through each stage. Host times are converted with the ping clock offset.
final class FrameTiming {
    let encodeNs: UInt64          // host: present → bitstream ready
    let presentLocal: UInt64?     // gamescope presented the frame
    let encodedLocal: UInt64?     // bitstream ready
    let received: UInt64          // last fragment arrived
    var decodeStart: UInt64 = 0
    var decoded: UInt64 = 0
    var submitted: UInt64 = 0     // Metal frame committed
    var presented: UInt64 = 0     // on screen

    init(slotNs: UInt64, encodedNs: UInt64, received: UInt64, toLocal: (UInt64) -> UInt64?) {
        encodeNs = encodedNs &- slotNs
        presentLocal = toLocal(slotNs)
        encodedLocal = toLocal(encodedNs)
        self.received = received
    }

    convenience init(frame f: Frame, toLocal: (UInt64) -> UInt64?) {
        self.init(slotNs: f.slotNs, encodedNs: f.encodedNs, received: f.receivedNs, toLocal: toLocal)
    }

    /// Measuring (GM_DEBUG_FRAMES): one line per frame shown ("shown") or
    /// never shown ("dropped"), every time in this Mac's ns.
    static let debug = getenv("GM_DEBUG_FRAMES") != nil
    func log(_ what: String) {
        FileHandle.standardError.write("frame \(what) \(presentLocal ?? 0) \(encodedLocal ?? 0) \(received) \(decodeStart) \(decoded) \(submitted) \(presented)\n".data(using: .utf8)!)
    }
}

/// Splits Annex B into NAL units (without start codes).
func nalUnits(_ data: Data) -> [Data] {
    var units: [Data] = []
    data.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
        let b = p.bindMemory(to: UInt8.self)
        let n = b.count
        var i = 0
        var start = -1
        while i + 2 < n {
            if b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 {
                if start >= 0 {
                    var end = i
                    if end > start && b[end - 1] == 0 { end -= 1 } // 4-byte start code
                    units.append(Data(b[start..<end]))
                }
                i += 3
                start = i
            } else {
                i += 1
            }
        }
        if start >= 0 && start < n { units.append(Data(b[start..<n])) }
    }
    return units
}

/// Hardware decode with VideoToolbox, into Metal-compatible BGRA buffers.
final class Decoder {
    /// NV12 (the decoder's own output, converted in our shader) rather than
    /// BGRA (an extra conversion pass in VideoToolbox).
    var yuv = true

    /// Called with each decoded image (on the decoder's thread).
    var onDecoded: (CVPixelBuffer, FrameTiming) -> Void = { _, _ in }

    private var session: VTDecompressionSession?
    private var format: CMVideoFormatDescription?
    private var parameterSets: [Data] = []

    /// Returns false when the frame could not be decoded (the caller asks for
    /// a keyframe).
    func decode(_ frame: Frame, timing: FrameTiming) -> Bool {
        guard let codec = Codec(rawValue: frame.codec) else { return false }
        var params: [Data] = []
        var sample = Data(capacity: frame.bitstream.count + 64)
        for nal in nalUnits(frame.bitstream) where !nal.isEmpty {
            let isParam: Bool, isAud: Bool
            switch codec {
            case .h264:
                let type = nal[nal.startIndex] & 0x1f
                isParam = type == 7 || type == 8
                isAud = type == 9
            case .hevc:
                let type = (nal[nal.startIndex] >> 1) & 0x3f
                isParam = type == 32 || type == 33 || type == 34
                isAud = type == 35
            }
            if isParam { params.append(nal); continue }
            if isAud { continue }
            var len = UInt32(nal.count).bigEndian
            withUnsafeBytes(of: &len) { sample.append(contentsOf: $0) }
            sample.append(nal)
        }
        if !params.isEmpty && params != parameterSets {
            parameterSets = params
            format = makeFormat(codec, params)
            if let session { VTDecompressionSessionInvalidate(session) }
            session = format.flatMap(makeSession)
        }
        guard let format, let session, !sample.isEmpty, let buffer = sampleBuffer(sample, format, frame) else { return false }
        timing.decodeStart = nowNs()
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: buffer, flags: [], infoFlagsOut: nil) {
            [weak self] status, _, image, _, _ in
            timing.decoded = nowNs()
            if status == noErr, let image { self?.onDecoded(image, timing) }
        }
        if status == kVTInvalidSessionErr {
            VTDecompressionSessionInvalidate(session)
            self.session = makeSession(format)
        }
        return status == noErr
    }

    private func makeSession(_ format: CMVideoFormatDescription) -> VTDecompressionSession? {
        let spec = [kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: true] as CFDictionary
        let attrs = [kCVPixelBufferPixelFormatTypeKey: yuv ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange : kCVPixelFormatType_32BGRA,
                     kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        var s: VTDecompressionSession?
        guard VTDecompressionSessionCreate(allocator: nil, formatDescription: format, decoderSpecification: spec,
                                           imageBufferAttributes: attrs, outputCallback: nil,
                                           decompressionSessionOut: &s) == noErr, let s else { return nil }
        VTSessionSetProperty(s, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        return s
    }

    private func sampleBuffer(_ sample: Data, _ format: CMVideoFormatDescription, _ frame: Frame) -> CMSampleBuffer? {
        var block: CMBlockBuffer?
        let size = sample.count
        guard CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: size,
                                                 blockAllocator: kCFAllocatorDefault, customBlockSource: nil,
                                                 offsetToData: 0, dataLength: size, flags: 0, blockBufferOut: &block) == noErr,
              let block else { return nil }
        sample.withUnsafeBytes { _ = CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: size) }
        var timing = CMSampleTimingInfo(duration: .invalid,
                                        presentationTimeStamp: CMTime(value: CMTimeValue(frame.slotNs), timescale: 1_000_000_000),
                                        decodeTimeStamp: .invalid)
        var sizes = [size]
        var buffer: CMSampleBuffer?
        guard CMSampleBufferCreateReady(allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format,
                                        sampleCount: 1, sampleTimingEntryCount: 1, sampleTimingArray: &timing,
                                        sampleSizeEntryCount: 1, sampleSizeArray: &sizes, sampleBufferOut: &buffer) == noErr
        else { return nil }
        return buffer
    }

    private func makeFormat(_ codec: Codec, _ params: [Data]) -> CMVideoFormatDescription? {
        var format: CMVideoFormatDescription?
        let datas = params.map { NSData(data: $0) }
        let pointers = datas.map { $0.bytes.assumingMemoryBound(to: UInt8.self) }
        let sizes = datas.map { $0.length }
        let status: OSStatus
        switch codec {
        case .h264:
            status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                allocator: kCFAllocatorDefault, parameterSetCount: params.count, parameterSetPointers: pointers,
                parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &format)
        case .hevc:
            status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                allocator: kCFAllocatorDefault, parameterSetCount: params.count, parameterSetPointers: pointers,
                parameterSetSizes: sizes, nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &format)
        }
        return status == noErr ? format : nil
    }
}

/// A decoded frame: VideoToolbox's image, or PyroWave's three planes.
enum DecodedImage {
    case buffer(CVPixelBuffer)
    case planes([MTLTexture])
}

/// Shows decoded frames on a CAMetalLayer from its own thread, vsync on.
/// Newest frame wins: one decoded while the thread waits for a drawable
/// replaces the one it was about to show (counted as skipped). macOS holds
/// each frame a fixed number of refreshes, so pacing is steady. (Presenting
/// with vsync off at scheduled times was tried: lower median, but macOS's
/// compositor shows such frames one or two refreshes later unpredictably,
/// which costs p90 and pacing.)
final class MetalRenderer {
    let layer = CAMetalLayer()
    /// Called when a frame reached the screen (on a Core Animation thread).
    var onPresented: (FrameTiming) -> Void = { _ in }
    var onSkipped: () -> Void = {}
    /// A submitted frame macOS never showed (replaced in its queue).
    var onDropped: (FrameTiming) -> Void = { _ in }

    let device: MTLDevice
    let queue: MTLCommandQueue // also used by the PyroWave decoder
    private let pipeline: MTLRenderPipelineState     // BGRA
    private let yuvPipeline: MTLRenderPipelineState  // NV12
    private let planarPipeline: MTLRenderPipelineState // PyroWave: Y, Cb, Cr
    private let overlayPipeline: MTLRenderPipelineState // premultiplied, blended
    private var cache: CVMetalTextureCache?
    private let cond = NSCondition()
    private var pending: (DecodedImage, FrameTiming)?
    // The stats overlay, drawn over each frame at `overlayInset` pixels from
    // the top left; with no new frame for a while, the last one is drawn
    // again so the overlay still updates.
    private var overlay: MTLTexture?
    private var overlayInset = 0.0
    private var last: DecodedImage?
    private var lastDrawn: UInt64 = 0
    private var repaint = false
    private var skip = false // the next frame is not shown
    private static var stallAt: UInt64? = getenv("GM_DEBUG_STALL").flatMap { Double(String(cString: $0)) }.map { nowNs() + UInt64($0 * 1e9) }

    private static let shader = """
        #include <metal_stdlib>
        using namespace metal;
        struct V { float4 pos [[position]]; float2 uv; };
        vertex V vs(uint id [[vertex_id]]) {
            float2 p = float2((id << 1) & 2, id & 2);
            V o;
            o.pos = float4(p * 2 - 1, 0, 1);
            o.uv = float2(p.x, 1 - p.y);
            return o;
        }
        fragment float4 fsOverlay(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {
            constexpr sampler s(filter::nearest);
            return t.sample(s, in.uv);
        }
        fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {
            constexpr sampler s(filter::linear);
            return t.sample(s, in.uv);
        }
        // PyroWave's planes: Y, Cb, Cr, BT.709 full range.
        fragment float4 fsPlanar(V in [[stage_in]], texture2d<float> y [[texture(0)]], texture2d<float> cb [[texture(1)]], texture2d<float> cr [[texture(2)]]) {
            constexpr sampler s(filter::linear);
            float l = y.sample(s, in.uv).r;
            float u = cb.sample(s, in.uv).r - 0.5, v = cr.sample(s, in.uv).r - 0.5;
            return float4(l + 1.5748 * v, l - 0.1873 * u - 0.4681 * v, l + 1.8556 * u, 1);
        }
        // NV12, BT.709 limited range (what the host encodes).
        fragment float4 fsYuv(V in [[stage_in]], texture2d<float> luma [[texture(0)]], texture2d<float> chroma [[texture(1)]]) {
            constexpr sampler s(filter::linear);
            float y = (luma.sample(s, in.uv).r - 16.0 / 255.0) * (255.0 / 219.0);
            float2 c = (chroma.sample(s, in.uv).rg - 128.0 / 255.0) * (255.0 / 224.0);
            return float4(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x, 1);
        }
        """

    /// Private CAMetalLayer / CAMetalDrawable properties to set by key
    /// (measuring: --layer, --drawable), e.g. ["lowLatency": 1].
    private let drawableKeys: [String: Double]

    init?(layerKeys: [String: Double] = [:], drawableKeys: [String: Double] = [:]) {
        self.drawableKeys = drawableKeys
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(),
              let library = try? device.makeLibrary(source: Self.shader, options: nil) else { return nil }
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = library.makeFunction(name: "vs")
        desc.fragmentFunction = library.makeFunction(name: "fs")
        desc.colorAttachments[0].pixelFormat = .bgra8Unorm
        guard let pipeline = try? device.makeRenderPipelineState(descriptor: desc) else { return nil }
        desc.fragmentFunction = library.makeFunction(name: "fsYuv")
        guard let yuvPipeline = try? device.makeRenderPipelineState(descriptor: desc) else { return nil }
        desc.fragmentFunction = library.makeFunction(name: "fsPlanar")
        guard let planarPipeline = try? device.makeRenderPipelineState(descriptor: desc) else { return nil }
        desc.fragmentFunction = library.makeFunction(name: "fsOverlay")
        let blend = desc.colorAttachments[0]!
        blend.isBlendingEnabled = true
        blend.sourceRGBBlendFactor = .one
        blend.sourceAlphaBlendFactor = .one
        blend.destinationRGBBlendFactor = .oneMinusSourceAlpha
        blend.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        guard let overlayPipeline = try? device.makeRenderPipelineState(descriptor: desc) else { return nil }
        self.device = device
        self.queue = queue
        self.pipeline = pipeline
        self.yuvPipeline = yuvPipeline
        self.planarPipeline = planarPipeline
        self.overlayPipeline = overlayPipeline
        CVMetalTextureCacheCreate(nil, nil, device, nil, &cache)
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        layer.displaySyncEnabled = true
        layer.maximumDrawableCount = 3
        layer.backgroundColor = NSColor.black.cgColor
        // Private: a new drawable replaces one still queued instead of lining
        // up behind it. With FIFO, one frame that misses its refresh delays
        // every later one by a refresh for good (the source runs at the
        // display's rate, so the queue never drains); without, it is dropped.
        // Measured: display stage 24-71 ms with FIFO, 19 ms without.
        if layer.responds(to: NSSelectorFromString("setDisableFIFO:")) {
            layer.setValue(true, forKey: "disableFIFO")
        }
        for (k, v) in layerKeys { layer.setValue(NSNumber(value: v), forKey: k) }
        let thread = Thread { [weak self] in self?.renderLoop() }
        thread.qualityOfService = .userInteractive
        thread.name = "gmstream-render"
        thread.start()
    }

    /// Shows one frame fewer: the next one is decoded but not presented, so a
    /// window server running a refresh behind catches up.
    func skipOne() {
        cond.lock(); skip = true; cond.unlock()
    }

    func submit(_ image: DecodedImage, _ timing: FrameTiming) {
        cond.lock()
        if pending != nil { onSkipped() }
        pending = (image, timing)
        cond.signal()
        cond.unlock()
    }

    /// Replaces the overlay (nil: none) with the image in `g`, drawn `inset`
    /// pixels from the top left.
    func setOverlay(_ g: CGContext?, inset: Double) {
        var texture: MTLTexture?
        if let g, let data = g.data {
            let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: g.width, height: g.height, mipmapped: false)
            desc.usage = .shaderRead
            texture = device.makeTexture(descriptor: desc)
            texture?.replace(region: MTLRegionMake2D(0, 0, g.width, g.height), mipmapLevel: 0, withBytes: data, bytesPerRow: g.bytesPerRow)
        }
        cond.lock()
        overlay = texture
        overlayInset = inset
        if pending == nil && nowNs() - lastDrawn > 200_000_000 {
            repaint = true
            cond.signal()
        }
        cond.unlock()
    }

    private func take() -> (DecodedImage, FrameTiming)? {
        cond.lock()
        defer { cond.unlock() }
        let p = pending
        pending = nil
        return p
    }

    private func renderLoop() {
        while true {
            cond.lock()
            while pending == nil && !repaint { cond.wait() }
            cond.unlock()
            // May wait for a free drawable (display refresh)...
            guard let drawable = layer.nextDrawable() else { continue }
            // ...so take the newest frame only now.
            guard var (image, timing) = take() else {
                cond.lock()
                let (image, again) = (last, repaint)
                repaint = false
                cond.unlock()
                if again { draw(image, nil, drawable) }
                continue
            }
            if let newer = take() {
                onSkipped()
                (image, timing) = newer
            }
            if let at = Self.stallAt, nowNs() > at {
                // Measuring (GM_DEBUG_STALL=<s>): one 30 ms hiccup, once.
                Self.stallAt = nil
                usleep(30_000)
            }
            cond.lock()
            let skipThis = skip
            skip = false
            if skipThis { last = image }
            cond.unlock()
            if skipThis { onSkipped(); continue } // the drawable goes back unpresented
            draw(image, timing, drawable)
        }
    }

    /// Draws `image` (nil: black) and the overlay; `timing` nil for a repaint.
    private func draw(_ image: DecodedImage?, _ timing: FrameTiming?, _ drawable: CAMetalDrawable) {
        let w: Int, h: Int
        let textures: [MTLTexture]
        var cvTextures: [CVMetalTexture] = []
        let pipeline: MTLRenderPipelineState
        switch image {
        case nil:
            (w, h, textures, pipeline) = (0, 0, [], self.pipeline)
        case .planes(let planes):
            // PyroWave's Y, Cb, Cr.
            (w, h, textures, pipeline) = (planes[0].width, planes[0].height, planes, planarPipeline)
        case .buffer(let buffer):
            w = CVPixelBufferGetWidth(buffer); h = CVPixelBufferGetHeight(buffer)
            let yuv = CVPixelBufferGetPlaneCount(buffer) == 2
            // Metal views of the decoder's buffer (no copy): one BGRA plane, or
            // NV12's luma and half-size interleaved chroma.
            let planes: [(MTLPixelFormat, Int, Int)] = yuv
                ? [(.r8Unorm, w, h), (.rg8Unorm, CVPixelBufferGetWidthOfPlane(buffer, 1), CVPixelBufferGetHeightOfPlane(buffer, 1))]
                : [(.bgra8Unorm, w, h)]
            for (i, (format, pw, ph)) in planes.enumerated() {
                var t: CVMetalTexture?
                guard let cache, CVMetalTextureCacheCreateTextureFromImage(nil, cache, buffer, nil, format, pw, ph, yuv ? i : 0, &t) == kCVReturnSuccess,
                      let t else { return }
                cvTextures.append(t)
            }
            textures = cvTextures.compactMap(CVMetalTextureGetTexture)
            guard textures.count == planes.count else { return }
            pipeline = yuv ? yuvPipeline : self.pipeline
        }
        guard let cb = queue.makeCommandBuffer() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
        let dw = Double(drawable.texture.width), dh = Double(drawable.texture.height)
        if !textures.isEmpty {
            // Fit the video into the drawable, letterboxed.
            let s = min(dw / Double(w), dh / Double(h))
            let vw = Double(w) * s, vh = Double(h) * s
            enc.setViewport(MTLViewport(originX: (dw - vw) / 2, originY: (dh - vh) / 2, width: vw, height: vh, znear: 0, zfar: 1))
            enc.setRenderPipelineState(pipeline)
            for (i, t) in textures.enumerated() { enc.setFragmentTexture(t, index: i) }
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        cond.lock()
        let (overlay, inset) = (self.overlay, overlayInset)
        if image != nil { last = image }
        lastDrawn = nowNs()
        cond.unlock()
        if let overlay {
            let ow = min(Double(overlay.width), dw - inset), oh = min(Double(overlay.height), dh - inset)
            if ow > 0 && oh > 0 {
                enc.setViewport(MTLViewport(originX: inset, originY: inset, width: Double(overlay.width), height: Double(overlay.height), znear: 0, zfar: 1))
                enc.setScissorRect(MTLScissorRect(x: Int(inset), y: Int(inset), width: Int(ow), height: Int(oh)))
                enc.setRenderPipelineState(overlayPipeline)
                enc.setFragmentTexture(overlay, index: 0)
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
        }
        enc.endEncoding()
        cb.addCompletedHandler { _ in _ = cvTextures } // keep the texture alive until the GPU is done
        guard let timing else {
            cb.present(drawable)
            cb.commit()
            return
        }
        drawable.addPresentedHandler { [weak self] d in
            guard d.presentedTime > 0 else { // never shown
                if FrameTiming.debug { timing.log("dropped") }
                self?.onDropped(timing)
                return
            }
            timing.presented = UInt64(d.presentedTime * 1e9)
            if FrameTiming.debug { timing.log("shown") }
            self?.onPresented(timing)
        }
        for (k, v) in drawableKeys { (drawable as AnyObject).setValue(NSNumber(value: v), forKey: k) }
        cb.present(drawable)
        timing.submitted = nowNs()
        cb.commit()
    }
}

extension MetalRenderer {
    /// CAMetalLayer's private direct-to-display status (measuring).
    var compositingStatus: String {
        guard layer.responds(to: NSSelectorFromString("displayCompositingInternalStatus")) else { return "-" }
        return "\(layer.value(forKey: "displayCompositingInternalStatus") ?? "nil")"
    }
}

extension MetalRenderer {
    /// Vsync off lets macOS flip frames straight to the panel (full screen).
    func setVsync(_ on: Bool) { layer.displaySyncEnabled = on }
}
