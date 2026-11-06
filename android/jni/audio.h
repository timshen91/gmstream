// Audio: the host's Opus packets (48 kHz stereo, 5 ms each) decoded by the
// platform's Opus decoder (MediaCodec) and played through AAudio in low-latency
// mode from a small jitter buffer, as the Apple clients do (apple/Audio.swift):
// start once the target is buffered, and drop back when more than four times
// it has piled up (the two clocks drift).

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <aaudio/AAudio.h>
#include <media/NdkMediaCodec.h>

namespace gm {

class AudioPlayer {
public:
	AudioPlayer();
	~AudioPlayer();
	// Network thread: one Opus packet.
	void push(const uint8_t *opus, size_t n);
	double buffered_ms() const;
	bool ok() const { return stream_ != nullptr && codec_ != nullptr; }
	int callback(float *out, int frames); // AAudio's thread

private:
	void decode_loop();
	void write(const float *s, size_t frames);
	void open_stream();

	AAudioStream *stream_ = nullptr;
	AMediaCodec *codec_ = nullptr;
	std::thread thread_;
	std::atomic<bool> running_{true};
	std::mutex qlock_;
	std::condition_variable qcv_;
	std::deque<std::vector<uint8_t>> queue_;

	// Interleaved stereo ring, one writer (decode thread), one reader (AAudio).
	static constexpr size_t ring_frames = 48000;
	std::vector<float> ring_;
	std::atomic<size_t> r_{0}, w_{0};
	bool started_ = false;
	size_t target_ = 960; // 20 ms
};

} // namespace gm
