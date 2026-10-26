// Audio: the host's Opus packets (48 kHz stereo, 5 ms each) decoded with
// Apple's own Opus decoder and played from a small jitter buffer, as the web
// client does: start once the target is buffered, and drop back to twice the
// target when more than four times it has piled up (host and device clocks
// drift).

import AVFoundation
import os

let audioRate = 48000.0

/// Opus packet → interleaved stereo Float32.
final class OpusDecoder {
    private let opus: AVAudioFormat
    private let pcm = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: audioRate, channels: 2, interleaved: true)!
    private let converter: AVAudioConverter

    init?() {
        var asbd = AudioStreamBasicDescription(mSampleRate: audioRate, mFormatID: kAudioFormatOpus, mFormatFlags: 0,
                                               mBytesPerPacket: 0, mFramesPerPacket: 240, mBytesPerFrame: 0,
                                               mChannelsPerFrame: 2, mBitsPerChannel: 0, mReserved: 0)
        guard let opus = AVAudioFormat(streamDescription: &asbd),
              let converter = AVAudioConverter(from: opus, to: pcm) else { return nil }
        self.opus = opus
        self.converter = converter
    }

    func decode(_ packet: Data) -> [Float] {
        let input = AVAudioCompressedBuffer(format: opus, packetCapacity: 1, maximumPacketSize: packet.count)
        packet.withUnsafeBytes { input.data.copyMemory(from: $0.baseAddress!, byteCount: packet.count) }
        input.packetDescriptions![0] = AudioStreamPacketDescription(mStartOffset: 0, mVariableFramesInPacket: 0,
                                                                    mDataByteSize: UInt32(packet.count))
        input.packetCount = 1
        input.byteLength = UInt32(packet.count)
        guard let output = AVAudioPCMBuffer(pcmFormat: pcm, frameCapacity: 5760) else { return [] }
        var given = false
        var error: NSError?
        converter.convert(to: output, error: &error) { _, status in
            if given {
                status.pointee = .noDataNow
                return nil
            }
            given = true
            status.pointee = .haveData
            return input
        }
        guard error == nil, let p = output.floatChannelData?[0] else { return [] }
        return Array(UnsafeBufferPointer(start: p, count: Int(output.frameLength) * 2))
    }
}

/// Plays interleaved stereo from a ring buffer filled by `push`.
final class AudioPlayer {
    var targetMs = 20.0 { didSet { target = Int(targetMs * audioRate / 1000) } }

    private let engine = AVAudioEngine()
    private let lock = OSAllocatedUnfairLock()
    private var ring = [Float](repeating: 0, count: Int(audioRate) * 2) // 1 s
    private var r = 0, w = 0
    private var started = false
    private var target = 960 // frames

    init() throws {
        let format = AVAudioFormat(standardFormatWithSampleRate: audioRate, channels: 2)!
        let source = AVAudioSourceNode(format: format) { [unowned self] silence, _, frames, list in
            let out = UnsafeMutableAudioBufferListPointer(list)
            let left = out[0].mData!.assumingMemoryBound(to: Float.self)
            let right = out[1].mData!.assumingMemoryBound(to: Float.self)
            self.lock.lock()
            let n = Int(frames)
            var i = 0
            if self.started || self.have() >= self.target {
                self.started = true
                while i < n && self.have() > 0 {
                    left[i] = self.ring[self.r]
                    right[i] = self.ring[self.r + 1]
                    self.r = (self.r + 2) % self.ring.count
                    i += 1
                }
                if i < n { self.started = false } // ran dry: buffer up again
            }
            self.lock.unlock()
            for j in i..<n { left[j] = 0; right[j] = 0 }
            if i == 0 { silence.pointee = true }
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        // Small device buffer for low output latency (best effort).
        if let unit = engine.outputNode.audioUnit {
            var frames: UInt32 = 256
            AudioUnitSetProperty(unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0,
                                 &frames, UInt32(MemoryLayout<UInt32>.size))
        }
        try engine.start()
    }

    /// Frames buffered (call with the lock held).
    private func have() -> Int { ((w - r + ring.count) % ring.count) / 2 }

    func push(_ samples: [Float]) {
        lock.lock()
        defer { lock.unlock() }
        for s in samples {
            ring[w] = s
            w = (w + 1) % ring.count
        }
        if have() > 4 * target { r = (w - 4 * target + ring.count) % ring.count }
    }

    var bufferedMs: Double {
        lock.lock()
        defer { lock.unlock() }
        return Double(have()) / audioRate * 1000
    }

    func stop() { engine.stop() }
}
