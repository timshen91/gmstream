// AV1's bitstream, as far as VideoToolbox needs it: the host sends each frame
// as low-overhead OBUs (every OBU carries its size), the sequence header on
// keyframes. VideoToolbox takes a frame's OBUs as they are, and describes the
// stream with the av1C record of ISO BMFF, which is built from that sequence
// header.

import Foundation

enum OBU {
    static let sequenceHeader: UInt8 = 1, temporalDelimiter: UInt8 = 2

    static func type(_ obu: Data) -> UInt8 { (obu[obu.startIndex] >> 3) & 0xf }
}

/// Splits a frame into OBUs (header included); empty if it isn't well formed.
func av1OBUs(_ data: Data) -> [Data] {
    var obus: [Data] = []
    var i = data.startIndex
    while i < data.endIndex {
        guard let (payload, size) = obuPayload(data, at: i) else { return [] }
        obus.append(data[i..<payload + size])
        i = payload + size
    }
    return obus
}

/// Where the OBU at `start` has its payload, and the payload's size. Without
/// a size field it runs to the end of `data`.
private func obuPayload(_ data: Data, at start: Int) -> (payload: Int, size: Int)? {
    let header = data[start]
    guard header & 0x80 == 0 else { return nil } // the forbidden bit
    var p = start + (header & 0x04 != 0 ? 2 : 1) // extension byte
    guard p <= data.endIndex else { return nil }
    var size = data.endIndex - p
    if header & 0x02 != 0 {
        size = 0
        var more = true
        for shift in 0..<8 where more { // leb128
            guard p < data.endIndex else { return nil }
            size |= Int(data[p] & 0x7f) << (7 * shift)
            more = data[p] & 0x80 != 0
            p += 1
        }
        guard !more else { return nil }
    }
    guard size >= 0, size <= data.endIndex - p else { return nil }
    return (p, size)
}

/// What the sequence header says about the stream.
struct AV1Config {
    let width: Int, height: Int // the largest frame
    /// The transfer function the stream declares; 16 is PQ (SMPTE ST 2084),
    /// i.e. HDR10. The client shows those frames on an HDR layer.
    let transfer: Int
    /// Ten-bit (high_bitdepth in the sequence header).
    let tenBit: Bool
    /// The av1C record: four bytes, then the sequence header OBU.
    let av1C: Data

    /// `obu`: a whole sequence header OBU. nil if it is cut short.
    init?(sequenceHeader obu: Data) {
        guard let (payload, size) = obuPayload(obu, at: obu.startIndex) else { return nil }
        var r = BitReader(obu[payload..<payload + size])
        let profile = r.bits(3)
        r.skip(1) // still_picture
        let reduced = r.flag()
        var level = 0, tier = 0
        if reduced {
            level = r.bits(5)
        } else {
            var decoderModel = false, delayBits = 0
            if r.flag() { // timing_info_present_flag
                r.skip(64) // num_units_in_display_tick, time_scale
                if r.flag() { r.uvlc() } // equal_picture_interval, num_ticks_per_picture
                decoderModel = r.flag()
                if decoderModel {
                    delayBits = r.bits(5) + 1
                    r.skip(32 + 5 + 5)
                }
            }
            let displayDelay = r.flag()
            let points = r.bits(5) + 1
            for i in 0..<points {
                r.skip(12) // operating_point_idc
                let l = r.bits(5)
                let t = l > 7 ? r.bits(1) : 0
                if i == 0 { (level, tier) = (l, t) }
                if decoderModel, r.flag() { r.skip(2 * delayBits + 1) }
                if displayDelay, r.flag() { r.skip(4) }
            }
        }
        let widthBits = r.bits(4) + 1, heightBits = r.bits(4) + 1
        width = r.bits(widthBits) + 1
        height = r.bits(heightBits) + 1
        if !reduced, r.flag() { r.skip(4 + 3) } // frame_id_numbers_present_flag
        r.skip(3) // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter
        if !reduced {
            r.skip(4) // enable_interintra_compound, masked_compound, warped_motion, dual_filter
            let orderHint = r.flag()
            if orderHint { r.skip(2) } // enable_jnt_comp, enable_ref_frame_mvs
            let chooseTools = r.flag()
            if chooseTools || r.flag() { // seq_force_screen_content_tools > 0
                if !r.flag() { r.skip(1) } // seq_choose_integer_mv, seq_force_integer_mv
            }
            if orderHint { r.skip(3) }
        }
        r.skip(3) // enable_superres, enable_cdef, enable_restoration
        // color_config
        let high = r.bits(1)
        let twelve = profile == 2 && high != 0 ? r.bits(1) : 0
        let mono = profile == 1 ? 0 : r.bits(1)
        var primaries = 2, transfer = 2, matrix = 2
        if r.flag() { (primaries, transfer, matrix) = (r.bits(8), r.bits(8), r.bits(8)) }
        var subX = 1, subY = 1, position = 0
        if mono == 0 {
            if primaries == 1 && transfer == 13 && matrix == 0 { // sRGB
                (subX, subY) = (0, 0)
            } else {
                r.skip(1) // color_range
                switch profile {
                case 0: (subX, subY) = (1, 1)
                case 1: (subX, subY) = (0, 0)
                default:
                    if twelve != 0 {
                        subX = r.bits(1)
                        subY = subX != 0 ? r.bits(1) : 0
                    } else {
                        (subX, subY) = (1, 0)
                    }
                }
                if subX != 0 && subY != 0 { position = r.bits(2) }
            }
        }
        guard !r.overrun else { return nil }
        self.transfer = transfer
        self.tenBit = high != 0
        av1C = Data([0x81, // marker, version
                     UInt8(profile << 5 | level),
                     UInt8(tier << 7 | high << 6 | twelve << 5 | mono << 4 | subX << 3 | subY << 2 | position),
                     0]) + obu
    }
}

/// Reads bits, most significant first; past the end, zeros and `overrun`.
private struct BitReader {
    private let data: Data
    private var bit = 0
    private(set) var overrun = false

    init(_ data: Data) { self.data = data }

    mutating func bits(_ n: Int) -> Int {
        var v = 0
        for _ in 0..<n {
            let i = data.startIndex + bit / 8
            guard i < data.endIndex else {
                overrun = true
                return v << 1
            }
            v = v << 1 | Int(data[i] >> (7 - bit % 8)) & 1
            bit += 1
        }
        return v
    }

    mutating func flag() -> Bool { bits(1) != 0 }
    mutating func skip(_ n: Int) { for _ in 0..<n { _ = bits(1) } }

    mutating func uvlc() {
        var zeros = 0
        while zeros < 32, !overrun, !flag() { zeros += 1 }
        skip(zeros)
    }
}
