import XCTest

/// Byte layouts PROTOCOL.md fixes: little-endian fields and the message ids.
final class WireTests: XCTestCase {
    func testWriterIsLittleEndian() {
        var w = Writer()
        w.u8(0xAB)
        w.u16(0x1234)
        w.i16(-2)
        w.u32(0xDEADBEEF)
        w.u64(0x0102030405060708)
        w.f32(1.5)
        XCTAssertEqual([UInt8](w.data), [0xAB, 0x34, 0x12, 0xFE, 0xFF, 0xEF, 0xBE, 0xAD, 0xDE,
                                         0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
                                         0x00, 0x00, 0xC0, 0x3F])
    }

    func testReadersRoundTripAtAnOffsetIntoASlice() {
        var w = Writer()
        w.u8(0)
        w.u16(65535)
        w.i16(-12345)
        w.u32(4_000_000_000)
        w.u64(UInt64.max - 7)
        // A slice whose startIndex is not 0, as Connection's subdata/removal leaves.
        let d = (Data([9, 9]) + w.data).dropFirst(2)
        XCTAssertNotEqual(d.startIndex, 0)
        XCTAssertEqual(d.u16(1), 65535)
        XCTAssertEqual(d.i16(3), -12345)
        XCTAssertEqual(d.u32(5), 4_000_000_000)
        XCTAssertEqual(d.u64(9), UInt64.max - 7)
    }

    func testMessageIdsMatchTheProtocol() {
        XCTAssertEqual(protocolVersion, 4)
        let client: [UInt8] = [Msg.hello, Msg.configure, Msg.keyframe, Msg.bitrate, Msg.key, Msg.button, Msg.motion,
                               Msg.position, Msg.scroll, Msg.gamepad, Msg.gamepadRemove, Msg.ping, Msg.displayGrid,
                               Msg.vrrPolicy, Msg.touch, Msg.auth]
        XCTAssertEqual(client, Array(0x01...0x10))
        let host: [UInt8] = [Msg.helloAck, Msg.cursor, Msg.relative, Msg.rumble, Msg.pong, Msg.error, Msg.cursorClip]
        XCTAssertEqual(host, Array(0x81...0x87))
        XCTAssertEqual([datagramAudio, datagramVideo, datagramControl, datagramPyroWave, datagramAwake], [1, 2, 3, 4, 5])
        XCTAssertEqual(maxDatagram, 1400)
    }
}
