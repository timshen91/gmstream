import AVFoundation
import Metal
import VideoToolbox
import XCTest

/// The decoders, fed by this Mac's own encoders.
final class VideoDecoderTests: XCTestCase {
    /// One frame encoded with VideoToolbox, as the host sends it: Annex B,
    /// parameter sets in front of a keyframe.
    func encode(_ type: CMVideoCodecType, width: Int, height: Int, key: Bool = true) throws -> Data {
        var session: VTCompressionSession?
        guard VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height), codecType: type,
                                         encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
                                         outputCallback: nil, refcon: nil, compressionSessionOut: &session) == noErr,
              let session else { throw XCTSkip("no \(type) encoder") }
        defer { VTCompressionSessionInvalidate(session) }
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        var pb: CVPixelBuffer?
        CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                            [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary, &pb)
        let buffer = try XCTUnwrap(pb)
        CVPixelBufferLockBaseAddress(buffer, [])
        for plane in 0..<2 {
            let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane)!.assumingMemoryBound(to: UInt8.self)
            let stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane), rows = CVPixelBufferGetHeightOfPlane(buffer, plane)
            for r in 0..<rows { for c in 0..<stride { base[r * stride + c] = plane == 0 ? UInt8((r + c) & 0xff) : 128 } }
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        var out: CMSampleBuffer?
        let props = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        VTCompressionSessionEncodeFrame(session, imageBuffer: buffer, presentationTimeStamp: CMTime(value: 0, timescale: 60),
                                        duration: .invalid, frameProperties: props, infoFlagsOut: nil) { status, _, sample in
            if status == noErr { out = sample }
        }
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        let sample = try XCTUnwrap(out)
        var annexB = Data()
        let start = Data([0, 0, 0, 1])
        if key, let format = CMSampleBufferGetFormatDescription(sample) {
            var count = 0
            for i in 0..<16 {
                var p: UnsafePointer<UInt8>?, size = 0
                let status = type == kCMVideoCodecType_H264
                    ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: i, parameterSetPointerOut: &p,
                                                                         parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
                    : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: i, parameterSetPointerOut: &p,
                                                                         parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
                guard status == noErr, let p else { break }
                annexB += start + Data(bytes: p, count: size)
                if i + 1 >= count { break }
            }
        }
        let block = try XCTUnwrap(CMSampleBufferGetDataBuffer(sample))
        var avcc = Data(count: CMBlockBufferGetDataLength(block))
        _ = avcc.withUnsafeMutableBytes { CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: $0.count, destination: $0.baseAddress!) }
        var i = 0
        while i + 4 <= avcc.count {
            let n = Int(avcc[i]) << 24 | Int(avcc[i + 1]) << 16 | Int(avcc[i + 2]) << 8 | Int(avcc[i + 3])
            annexB += start + avcc.subdata(in: i + 4..<i + 4 + n)
            i += 4 + n
        }
        return annexB
    }

    func frame(_ id: UInt32, codec: UInt8, _ bitstream: Data, width: Int, height: Int) -> Frame {
        Frame(id: id, key: true, codec: codec, width: width, height: height, slotNs: 1, encodedNs: 2, bitstream: bitstream, receivedNs: 3)
    }

    /// Decodes and returns the image's size (nil: nothing came out).
    func decode(_ d: Decoder, _ f: Frame) -> (Int, Int, OSType)? {
        let done = expectation(description: "decoded")
        var size: (Int, Int, OSType)?
        d.onDecoded = { image, shown, timing in
            size = (CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image), CVPixelBufferGetPixelFormatType(image))
            XCTAssertEqual(shown, f.height)
            XCTAssertGreaterThanOrEqual(timing.decoded, timing.decodeStart)
            done.fulfill()
        }
        let t = timing(present: 1)
        guard d.decode(f, timing: t) else { return nil }
        wait(for: [done], timeout: 5)
        return size
    }

    func testH264() throws {
        let bits = try encode(kCMVideoCodecType_H264, width: 320, height: 240)
        let d = Decoder()
        let r = decode(d, frame(1, codec: Codec.h264.rawValue, bits, width: 320, height: 240))
        XCTAssertEqual(r?.0, 320)
        XCTAssertEqual(r?.1, 240)
    }

    func testHEVC() throws {
        let bits = try encode(kCMVideoCodecType_HEVC, width: 640, height: 360)
        let r = decode(Decoder(), frame(1, codec: Codec.hevc.rawValue, bits, width: 640, height: 360))
        XCTAssertEqual(r?.0, 640)
        XCTAssertEqual(r?.1, 360)
    }

    func testNewParameterSetsMakeANewSession() throws {
        let d = Decoder()
        XCTAssertEqual(decode(d, frame(1, codec: 0, try encode(kCMVideoCodecType_H264, width: 320, height: 240), width: 320, height: 240))?.0, 320)
        XCTAssertEqual(decode(d, frame(2, codec: 0, try encode(kCMVideoCodecType_H264, width: 640, height: 480), width: 640, height: 480))?.0, 640)
    }

    func testNothingToDecodeWithoutParameterSets() throws {
        let bits = try encode(kCMVideoCodecType_H264, width: 320, height: 240)
        // The slices alone: no SPS/PPS yet, so no session.
        let slices = nalUnits(bits).filter { !$0.isEmpty && $0[$0.startIndex] & 0x1f != 7 && $0[$0.startIndex] & 0x1f != 8 }
        let onlySlices = slices.reduce(Data()) { $0 + Data([0, 0, 0, 1]) + $1 }
        XCTAssertFalse(Decoder().decode(frame(1, codec: 0, onlySlices, width: 320, height: 240), timing: timing(present: 1)))
        XCTAssertFalse(Decoder().decode(frame(1, codec: 2, bits, width: 320, height: 240), timing: timing(present: 1)), "H.264 as AV1")
        XCTAssertFalse(Decoder().decode(frame(1, codec: 9, bits, width: 320, height: 240), timing: timing(present: 1)), "not a codec")
    }

    /// Eight frames from a real host's encoder (av1_vaapi, as the host runs it),
    /// asked for 640x360: it pads that to 362 rows. Comes as a muxed stream,
    /// so a temporal delimiter opens each frame.
    func hostAV1() throws -> [Data] {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("av1-640x360.obu")
        var frames: [Data] = []
        for obu in av1OBUs(try Data(contentsOf: url)) {
            if OBU.type(obu) == OBU.temporalDelimiter { frames.append(Data()) }
            frames[frames.count - 1].append(obu)
        }
        return frames
    }

    func testAV1OBUs() throws {
        let frames = try hostAV1()
        XCTAssertGreaterThan(frames.count, 7)
        XCTAssertEqual(av1OBUs(frames[0]).map(OBU.type), [2, 1, 3, 4], "delimiter, sequence header, frame header, tiles")
        XCTAssertEqual(av1OBUs(frames[1]).map(OBU.type), [2, 3, 4])
        XCTAssertEqual(av1OBUs(frames[0].dropLast()), [], "cut short")
        XCTAssertEqual(av1OBUs(Data([0xFF, 0xFF])), [], "the forbidden bit")
    }

    func testAV1SequenceHeader() throws {
        let header = try XCTUnwrap(av1OBUs(try hostAV1()[0]).first { OBU.type($0) == OBU.sequenceHeader })
        let config = try XCTUnwrap(AV1Config(sequenceHeader: header))
        XCTAssertEqual(config.width, 640)
        XCTAssertEqual(config.height, 362)
        let a = [UInt8](config.av1C)
        XCTAssertEqual(a[0], 0x81)
        XCTAssertEqual(a[1] >> 5, 0, "Main profile")
        XCTAssertEqual(a[2] & 0b1111_1100, 0b0000_1100, "8 bits, 4:2:0")
        XCTAssertEqual(Data(a[4...]), Data(header))
        XCTAssertNil(AV1Config(sequenceHeader: header.prefix(5)), "cut short")
    }

    func testAV1FromTheHost() throws {
        guard Decoder.hasAV1 else { throw XCTSkip("no AV1 hardware decoder") }
        let d = Decoder()
        for (i, bits) in try hostAV1().enumerated() {
            let r = decode(d, frame(UInt32(i), codec: Codec.av1.rawValue, bits, width: 640, height: 360))
            XCTAssertEqual(r?.0, 640)
            XCTAssertEqual(r?.1, 362, "the padded image, frame \(i)")
        }
    }
}

final class PyroWaveDecoderTests: XCTestCase {
    let width = 256, height = 128
    var device: MTLDevice!
    var decoder: PyroWaveDecoder!
    var pw: pyrowave_device!
    var encoder: pyrowave_encoder!
    var luma: [UInt8] = []

    override func setUpWithError() throws {
        device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        guard let d = PyroWaveDecoder(device: device, queue: device.makeCommandQueue()!) else { throw XCTSkip("PyroWave unsupported here") }
        decoder = d
        var info = pyrowave_device_create_info(mtl_device: Unmanaged.passUnretained(device as AnyObject).toOpaque(),
                                               message_callback: nil, message_userdata: nil)
        var dev: pyrowave_device?
        XCTAssertEqual(pyrowave_device_create(&info, &dev), PYROWAVE_SUCCESS)
        pw = dev
        var e = pyrowave_encoder_create_info(device: pw, width: Int32(width), height: Int32(height), chroma: PYROWAVE_CHROMA_SUBSAMPLING_420)
        var enc: pyrowave_encoder?
        XCTAssertEqual(pyrowave_encoder_create(&e, &enc), PYROWAVE_SUCCESS)
        encoder = enc
        luma = (0..<width * height).map { i in UInt8((i % width + i / width * 2) & 0xff) }
    }

    override func tearDown() {
        if let encoder { pyrowave_encoder_destroy(encoder) }
        if let pw { pyrowave_device_destroy(pw) }
    }

    /// One frame's packets (each at most ~1400 bytes), as the host sends them.
    func encodeFrame() throws -> [Data] {
        var y = luma
        var cb = [UInt8](repeating: 100, count: width * height / 4), cr = [UInt8](repeating: 150, count: width * height / 4)
        let (w, h) = (width, height)
        let rc = try y.withUnsafeMutableBytes { yp in
            try cb.withUnsafeMutableBytes { cbp in
                try cr.withUnsafeMutableBytes { crp in
                    var input = pyrowave_cpu_buffer(data: (yp.baseAddress, cbp.baseAddress, crp.baseAddress),
                                                    row_stride_in_bytes: (w, w / 2, w / 2),
                                                    plane_size_in_bytes: (w * h, w * h / 4, w * h / 4),
                                                    width: Int32(w), height: Int32(h), format: PYROWAVE_CPU_BUFFER_FORMAT_YUV420P)
                    var rate = pyrowave_rate_control(maximum_bitstream_size: 200_000)
                    let r = pyrowave_encoder_encode_cpu(encoder, &input, &rate)
                    guard r == PYROWAVE_SUCCESS else { throw XCTSkip("encode: \(r)") }
                    return r
                }
            }
        }
        XCTAssertEqual(rc, PYROWAVE_SUCCESS)
        var n = 0
        XCTAssertEqual(pyrowave_encoder_compute_num_packets(encoder, 1400, &n), PYROWAVE_SUCCESS)
        var packets = [pyrowave_packet](repeating: pyrowave_packet(), count: n)
        var bits = [UInt8](repeating: 0, count: 400_000)
        var out = 0
        XCTAssertEqual(pyrowave_encoder_packetize(encoder, &packets, 1400, &out, &bits, bits.count), PYROWAVE_SUCCESS)
        XCTAssertGreaterThan(out, 1, "several packets, to test assembly")
        return packets.prefix(out).map { Data(bits[$0.offset..<$0.offset + $0.size]) }
    }

    func packet(_ frameId: UInt32, _ i: Int, of parts: [Data]) -> PyroWavePacket {
        PyroWavePacket(frameId: frameId, index: i, count: parts.count, width: width, height: height,
                       slotNs: 1, encodedNs: 2, payload: parts[i], receivedNs: 3, readNs: 4)
    }

    /// The luma plane of a decoded frame.
    func readLuma(_ planes: [MTLTexture]) -> [UInt8] {
        let buffer = device.makeBuffer(length: width * height, options: .storageModeShared)!
        let cb = device.makeCommandQueue()!.makeCommandBuffer()!
        let blit = cb.makeBlitCommandEncoder()!
        blit.copy(from: planes[0], sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: width, height: height, depth: 1), to: buffer, destinationOffset: 0,
                  destinationBytesPerRow: width, destinationBytesPerImage: width * height)
        blit.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        return Array(UnsafeBufferPointer(start: buffer.contents().assumingMemoryBound(to: UInt8.self), count: width * height))
    }

    func testRoundTrip() throws {
        let parts = try encodeFrame()
        let done = expectation(description: "decoded")
        var planes: [MTLTexture] = []
        decoder.onDecoded = { p, t in planes = p; XCTAssertGreaterThanOrEqual(t.decoded, t.decodeStart); done.fulfill() }
        decoder.onIncomplete = { XCTFail("incomplete") }
        for i in parts.indices.reversed() { decoder.packet(packet(1, i, of: parts)) { timing(present: 1) } } // any order
        wait(for: [done], timeout: 5)
        XCTAssertEqual(planes.map(\.width), [width, width / 2, width / 2])
        XCTAssertEqual(planes.map(\.height), [height, height / 2, height / 2])
        let decoded = readLuma(planes)
        let error = zip(decoded, luma).map { abs(Int($0) - Int($1)) }.reduce(0, +) / luma.count
        XCTAssertLessThan(error, 3, "mean luma error \(error)")
    }

    func testANewerFrameGivesUpOnAnIncompleteOneAndIgnoresStalePackets() throws {
        let a = try encodeFrame(), b = try encodeFrame()
        var decoded = 0, incomplete = 0
        let done = expectation(description: "decoded")
        decoder.onDecoded = { _, _ in decoded += 1; done.fulfill() }
        decoder.onIncomplete = { incomplete += 1 }
        decoder.packet(packet(1, 0, of: a)) { timing(present: 1) }
        for i in b.indices { decoder.packet(packet(2, i, of: b)) { timing(present: 2) } }
        for i in a.indices.dropFirst() { decoder.packet(packet(1, i, of: a)) { timing(present: 1) } } // stale now
        wait(for: [done], timeout: 5)
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        XCTAssertEqual(decoded, 1)
        XCTAssertEqual(incomplete, 1)
    }
}

/// Inter frames in stripes (PROTOCOL.md, "Riptide"): the host
/// codes each frame as two 128-row stripes, each an image of its own (an encoder
/// each) predicting from its own decode of the whole last frame, as
/// pyrowave_encoder_packetize_stripe hands them out.
final class RiptideTests: XCTestCase {
    let width = 256, height = 256, rows = 128
    var stripes: Int { height / rows }
    var device: MTLDevice!
    var decoder: RiptideDecoder!
    var pw: riptide_device!
    let queue = MTLCreateSystemDefaultDevice()!.makeCommandQueue()!
    // The host: an encoder and a decoder per stripe, its decoded frames.
    var encoders: [riptide_encoder] = []
    var hostDecoders: [riptide_decoder] = []
    var hostStripePlanes: [[MTLTexture]] = []
    var hostPlanes: [[MTLTexture]] = []
    var luma: [UInt8] = []

    func planes(_ w: Int, _ h: Int) -> [MTLTexture] {
        [(w, h), (w / 2, h / 2), (w / 2, h / 2)].map { w, h in
            let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: w, height: h, mipmapped: false)
            desc.usage = [.shaderRead, .shaderWrite]
            desc.storageMode = .private
            return device.makeTexture(descriptor: desc)!
        }
    }

    override func setUpWithError() throws {
        device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        guard let d = RiptideDecoder(device: device, queue: device.makeCommandQueue()!) else { throw XCTSkip("PyroWave unsupported here") }
        decoder = d
        var info = riptide_device_create_info(mtl_device: Unmanaged.passUnretained(device as AnyObject).toOpaque(),
                                               message_callback: nil, message_userdata: nil)
        var dev: riptide_device?
        XCTAssertEqual(riptide_device_create(&info, &dev), RIPTIDE_SUCCESS)
        pw = dev
        for _ in 0..<stripes {
            var e = riptide_encoder_create_info(device: pw, width: Int32(width), height: Int32(rows), chroma: RIPTIDE_CHROMA_SUBSAMPLING_420)
            var enc: riptide_encoder?
            XCTAssertEqual(riptide_encoder_create(&e, &enc), RIPTIDE_SUCCESS)
            encoders.append(enc!)
            var di = riptide_decoder_create_info(device: pw, width: Int32(width), height: Int32(rows), chroma: RIPTIDE_CHROMA_SUBSAMPLING_420)
            var hd: riptide_decoder?
            XCTAssertEqual(riptide_decoder_create(&di, &hd), RIPTIDE_SUCCESS)
            hostDecoders.append(hd!)
            hostStripePlanes.append(planes(width, rows))
        }
        hostPlanes = (0..<2).map { _ in planes(width, height) }
    }

    override func tearDown() {
        for d in hostDecoders { riptide_decoder_destroy(d) }
        for e in encoders { riptide_encoder_destroy(e) }
        if let pw { riptide_device_destroy(pw) }
    }

    let ptr = { (o: AnyObject) in Unmanaged.passUnretained(o).toOpaque() }

    /// Frame `n` of a picture moving 2 pixels a frame: each stripe's packets, and
    /// whether it is predicted.
    func encodeFrame(_ n: Int) throws -> (stripes: [[Data]], predicted: Bool) {
        let predicted = n > 0
        luma = (0..<width * height).map { i in UInt8((i % width + 2 * n + (i / width) * 2) & 0xff) }
        var out: [[Data]] = []
        let cb = queue.makeCommandBuffer()!
        for k in 0..<stripes {
            let encoder = encoders[k], y0 = k * rows
            if predicted {
                let r = hostPlanes[(n - 1) & 1]
                var ref = riptide_gpu_buffers(planes: (ptr(r[0]), ptr(r[1]), ptr(r[2])))
                withUnsafePointer(to: &ref) { _ = riptide_encoder_set_reference_stripe(encoder, OpaquePointer($0), Int32(y0)) } // declared before its struct
                riptide_decoder_set_reference_stripe(hostDecoders[k], &ref, Int32(y0))
            } else {
                riptide_encoder_set_reference_stripe(encoder, nil, 0)
                riptide_decoder_set_reference_stripe(hostDecoders[k], nil, Int32(y0))
            }
            var y = Array(luma[y0 * width..<(y0 + rows) * width])
            var cbp = [UInt8](repeating: 100, count: width * rows / 4), crp = [UInt8](repeating: 150, count: width * rows / 4)
            let (w, h) = (width, rows)
            let r = y.withUnsafeMutableBytes { yp in
                cbp.withUnsafeMutableBytes { cbb in
                    crp.withUnsafeMutableBytes { crb in
                        var input = riptide_cpu_buffer(data: (yp.baseAddress, cbb.baseAddress, crb.baseAddress),
                                                        row_stride_in_bytes: (w, w / 2, w / 2),
                                                        plane_size_in_bytes: (w * h, w * h / 4, w * h / 4),
                                                        width: Int32(w), height: Int32(h), format: RIPTIDE_CPU_BUFFER_FORMAT_YUV420P)
                        var rate = riptide_rate_control(maximum_bitstream_size: 100_000)
                        return riptide_encoder_encode_cpu(encoder, &input, &rate)
                    }
                }
            }
            guard r == RIPTIDE_SUCCESS else { throw XCTSkip("encode: \(r)") }
            var count = 0
            XCTAssertEqual(riptide_encoder_compute_num_packets(encoder, 1400, &count), RIPTIDE_SUCCESS)
            var packets = [riptide_packet](repeating: riptide_packet(), count: count)
            var bits = [UInt8](repeating: 0, count: 400_000)
            var got = 0
            XCTAssertEqual(riptide_encoder_packetize(encoder, &packets, 1400, &got, &bits, bits.count), RIPTIDE_SUCCESS)
            let parts = packets.prefix(got).map { Data(bits[$0.offset..<$0.offset + $0.size]) }
            XCTAssertGreaterThan(parts.count, 1)
            out.append(parts)
            // The host's own decode of the stripe, into its frame.
            let hd = hostDecoders[k]
            riptide_decoder_clear(hd)
            for p in parts { p.withUnsafeBytes { _ = riptide_decoder_push_packet(hd, $0.baseAddress, $0.count) } }
            let s = hostStripePlanes[k]
            var buffers = riptide_gpu_buffers(planes: (ptr(s[0]), ptr(s[1]), ptr(s[2])))
            XCTAssertEqual(riptide_decoder_decode_gpu_buffer(hd, ptr(cb), &buffers), RIPTIDE_SUCCESS)
            let blit = cb.makeBlitCommandEncoder()!
            for c in 0..<3 {
                blit.copy(from: s[c], sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                          sourceSize: MTLSize(width: s[c].width, height: s[c].height, depth: 1), to: hostPlanes[n & 1][c],
                          destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: c == 0 ? y0 : y0 / 2, z: 0))
            }
            blit.endEncoding()
        }
        cb.commit()
        cb.waitUntilCompleted()
        return (out, predicted)
    }

    /// Frame f's packets as the host numbers them: every stripe's, in order.
    func packets(_ id: UInt32, _ f: (stripes: [[Data]], predicted: Bool), repaired: UInt32 = 0) -> [RiptidePacket] {
        var all: [RiptidePacket] = []
        let total = f.stripes.map(\.count).reduce(0, +)
        var first = 0
        for (k, parts) in f.stripes.enumerated() {
            for (i, part) in parts.enumerated() {
                var p = RiptidePacket(frameId: id, index: first + i, count: k + 1 == f.stripes.count ? total : 0,
                                       width: width, height: height, slotNs: 1, encodedNs: 2, payload: part,
                                       receivedNs: 3, readNs: 4)
                p.predicted = f.predicted
                p.repaired = repaired
                p.stripe = k
                p.stripes = f.stripes.count
                p.firstRows = rows
                p.stripeRows = rows
                p.stripeFirst = first
                p.stripeCount = parts.count
                // Inter packets: extended, code 1 (bitstream/inter.md).
                p.stripeInter = parts.prefix { $0[3] & 0x80 != 0 && ($0.u32(4) >> 24) & 3 == 1 }.count
                all.append(p)
            }
            first += parts.count
        }
        return all
    }

    /// The luma plane of a decoded frame.
    func readLuma(_ planes: [MTLTexture]) -> [UInt8] {
        let buffer = device.makeBuffer(length: width * height, options: .storageModeShared)!
        let cb = queue.makeCommandBuffer()!
        let blit = cb.makeBlitCommandEncoder()!
        blit.copy(from: planes[0], sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: width, height: height, depth: 1), to: buffer, destinationOffset: 0,
                  destinationBytesPerRow: width, destinationBytesPerImage: width * height)
        blit.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        return Array(UnsafeBufferPointer(start: buffer.contents().assumingMemoryBound(to: UInt8.self), count: width * height))
    }

    func send(_ ps: [RiptidePacket]) {
        for p in ps { decoder.packet(p) { _, _ in timing(present: 1) } }
    }

    func testStripesDecodeInOrderFromTheLastFrame() throws {
        let frames = try (0..<4).map { try encodeFrame($0) }
        var planes: [[MTLTexture]] = []
        let done = expectation(description: "decoded")
        done.expectedFulfillmentCount = frames.count
        decoder.onDecoded = { p, _ in planes.append(p); done.fulfill() }
        decoder.onLoss = { id, _, _ in XCTFail("no loss, but frame \(id) reported") }
        decoder.onNeedIntra = { XCTFail("needs no intra frame") }
        for (n, f) in frames.enumerated() { send(packets(UInt32(10 + n), f)) }
        wait(for: [done], timeout: 5)
        let decoded = readLuma(planes.last!)
        XCTAssertEqual(decoded, readLuma(hostPlanes[3 & 1]), "the host's own decode, exactly")
        let error = zip(decoded, luma).map { abs(Int($0) - Int($1)) }.reduce(0, +) / luma.count
        XCTAssertLessThan(error, 3, "the last frame, predicted three times over: mean luma error \(error)")
    }

    func testAStripeIsDecodedOnceItIsIn() throws {
        let frames = try (0..<2).map { try encodeFrame($0) }
        var decoded = 0
        decoder.onDecoded = { _, _ in decoded += 1 }
        send(packets(10, frames[0]))
        let second = packets(11, frames[1])
        send(Array(second.prefix(frames[1].stripes[0].count))) // only its first stripe
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        XCTAssertEqual(decoded, 1, "the frame is shown once all its stripes are in")
        send(Array(second.dropFirst(frames[1].stripes[0].count)))
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        XCTAssertEqual(decoded, 2)
    }

    func testALostPacketIsReportedUntilTheHostTakesItIn() throws {
        let frames = try (0..<5).map { try encodeFrame($0) }
        var losses: [(UInt32, Int, [(Int, Int)])] = []
        var decoded = 0, incomplete = 0
        decoder.onDecoded = { _, _ in decoded += 1 }
        decoder.onIncomplete = { incomplete += 1 }
        decoder.onLoss = { losses.append(($0, $1, $2)) }
        for (n, f) in frames.enumerated() {
            var ps = packets(UInt32(10 + n), f, repaired: n == 4 ? 11 : 0)
            if n == 1 { ps.remove(at: 0) } // frame 11's first packet is lost
            send(ps)
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        XCTAssertEqual(incomplete, 1)
        XCTAssertEqual(decoded, 5, "the incomplete frame is decoded with what came: the next one predicts from it")
        XCTAssertEqual(losses.map(\.0), [11, 11, 11],
                       "reported once frame 11 was decoded, again as frames 12 and 13 began; frame 14 says the host has it")
        XCTAssertEqual(losses.first?.1, frames[1].stripes.map(\.count).reduce(0, +))
        XCTAssertEqual(losses.first?.2.map { [$0.0, $0.1] }, [[0, 1]])
    }

    func testALostLastStripeIsReportedAsTheRest() throws {
        let frames = try (0..<3).map { try encodeFrame($0) }
        var losses: [(UInt32, Int, [(Int, Int)])] = []
        var decoded = 0
        decoder.onDecoded = { _, _ in decoded += 1 }
        decoder.onLoss = { losses.append(($0, $1, $2)) }
        send(packets(10, frames[0]))
        send(Array(packets(11, frames[1]).prefix(frames[1].stripes[0].count)))
        send(packets(12, frames[2]))
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        XCTAssertEqual(decoded, 3)
        XCTAssertEqual(losses.first?.0, 11)
        XCTAssertEqual(losses.first?.1, 0, "the frame's packet count never came")
        XCTAssertEqual(losses.first?.2.map { [$0.0, $0.1] }, [[frames[1].stripes[0].count, 0xffff]])
    }

    func testAMissingFrameIsDecodedFromNothingAndReported() throws {
        let frames = try (0..<3).map { try encodeFrame($0) }
        var losses: [(UInt32, Int, [(Int, Int)])] = []
        var decoded = 0
        decoder.onDecoded = { _, _ in decoded += 1 }
        decoder.onLoss = { losses.append(($0, $1, $2)) }
        send(packets(10, frames[0]))
        send(packets(12, frames[2]))
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        XCTAssertEqual(decoded, 2, "the bridged frame is decoded, not shown")
        XCTAssertEqual(losses.map(\.0), [11])
        XCTAssertEqual(losses.first?.2.map { [$0.0, $0.1] }, [[0, 0xffff]], "all of it")
    }

    func testAPredictedFrameWithNothingBeforeItAsksForAnIntraFrame() throws {
        let frames = try (0..<2).map { try encodeFrame($0) }
        var needed = 0, decoded = 0
        decoder.onNeedIntra = { needed += 1 }
        decoder.onDecoded = { _, _ in decoded += 1 }
        send(packets(11, frames[1]))
        send(packets(12, frames[0]))
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        XCTAssertEqual(needed, 1, "once, not per packet")
        XCTAssertEqual(decoded, 1, "the intra frame")
    }
}

final class OpusDecoderTests: XCTestCase {
    func testDecodesA5msPacketOfAppleOpus() throws {
        let pcm = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: audioRate, channels: 2, interleaved: true)!
        var asbd = AudioStreamBasicDescription(mSampleRate: audioRate, mFormatID: kAudioFormatOpus, mFormatFlags: 0,
                                               mBytesPerPacket: 0, mFramesPerPacket: 240, mBytesPerFrame: 0,
                                               mChannelsPerFrame: 2, mBitsPerChannel: 0, mReserved: 0)
        guard let opus = AVAudioFormat(streamDescription: &asbd), let encoder = AVAudioConverter(from: pcm, to: opus) else {
            throw XCTSkip("no Opus encoder")
        }
        // 20 ms of a 1 kHz tone: four packets.
        let input = AVAudioPCMBuffer(pcmFormat: pcm, frameCapacity: 960)!
        input.frameLength = 960
        for i in 0..<960 {
            let v = Float(0.5 * sin(2 * Double.pi * 1000 * Double(i) / audioRate))
            input.floatChannelData![0][2 * i] = v
            input.floatChannelData![0][2 * i + 1] = v
        }
        let packet = AVAudioCompressedBuffer(format: opus, packetCapacity: 8, maximumPacketSize: max(encoder.maximumOutputPacketSize, 1500))
        var given = false
        var error: NSError?
        encoder.convert(to: packet, error: &error) { _, status in
            if given { status.pointee = .noDataNow; return nil }
            given = true
            status.pointee = .haveData
            return input
        }
        guard error == nil, packet.packetCount >= 3, let descs = packet.packetDescriptions else { throw XCTSkip("Opus encode failed") }
        let decoder = try XCTUnwrap(OpusDecoder())
        let decoded = (0..<Int(packet.packetCount)).map { i in
            decoder.decode(Data(bytes: packet.data.advanced(by: Int(descs[i].mStartOffset)), count: Int(descs[i].mDataByteSize)))
        }
        // The first comes out shorter: the codec's start-up delay is trimmed.
        XCTAssertLessThanOrEqual(decoded[0].count, 240 * 2)
        for samples in decoded.dropFirst() {
            XCTAssertEqual(samples.count, 240 * 2, "5 ms of interleaved stereo")
        }
        let all = decoded.joined()
        XCTAssertTrue(all.allSatisfy { $0.isFinite && abs($0) <= 1.01 })
        XCTAssertGreaterThan(all.map { $0 * $0 }.reduce(0, +) / Float(all.count), 0.05, "the tone, not silence")
        XCTAssertTrue(decoder.decode(Data([0xFF, 0xFF, 0xFF])).count <= 480, "garbage does not crash")
    }
}
