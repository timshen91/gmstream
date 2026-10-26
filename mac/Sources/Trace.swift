// Measuring (GM_DEBUG_TRACE=<file>): every datagram's arrival, every frame
// on screen and every clock sample, as 32-byte little-endian records, for
// telling network stalls (all traffic late, then a burst) from losses
// (sequence holes) after the fact. tools/trace.py reads it.
//
//   u64 t        this Mac's CLOCK_UPTIME_RAW ns (arrival; presentation; now)
//   u32 aux      datagram: read − arrival ns; presented: presented − received;
//                clock: round trip ns
//   u8  kind     datagram type (1 audio, 2 video, 3 control, 4 PyroWave);
//                0xFE presented, 0xFF clock
//   u8  0
//   u16 size     datagram bytes
//   u32 id       audio sequence; video/PyroWave frame id; control sequence
//   u16 index    fragment/packet index
//   u16 count    fragment/packet count
//   u64 host     audio pts / PyroWave encoded (host ns); presented: gamescope
//                present (local ns, 0 if unknown); clock: host − local offset

import Foundation

final class Trace {
    static let shared: Trace? = getenv("GM_DEBUG_TRACE").flatMap { Trace(path: String(cString: $0)) }

    private let file: UnsafeMutablePointer<FILE>

    private init?(path: String) {
        guard let f = fopen(path, "wb") else { return nil }
        setvbuf(f, nil, _IOFBF, 1 << 20)
        file = f
        // Flushed now and then so the file is readable while streaming.
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [file] _ in fflush(file) }
    }

    /// From any thread (stdio locks the stream).
    func record(t: UInt64, aux: UInt64, kind: UInt8, size: Int = 0, id: UInt32 = 0, index: Int = 0, count: Int = 0, host: UInt64 = 0) {
        var w = Writer()
        w.u64(t)
        w.u32(UInt32(min(aux, UInt64(UInt32.max))))
        w.u8(kind); w.u8(0)
        w.u16(UInt16(min(size, 65535)))
        w.u32(id)
        w.u16(UInt16(truncatingIfNeeded: index)); w.u16(UInt16(truncatingIfNeeded: count))
        w.u64(host)
        w.data.withUnsafeBytes { _ = fwrite($0.baseAddress, 1, $0.count, file) }
    }
}
