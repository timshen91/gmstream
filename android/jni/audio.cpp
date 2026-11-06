#include "audio.h"

#include <cstring>

#include <media/NdkMediaFormat.h>

#include "log.h"

namespace gm {

static aaudio_data_callback_result_t data_cb(AAudioStream *, void *user, void *data, int32_t frames)
{
	static_cast<AudioPlayer *>(user)->callback(static_cast<float *>(data), frames);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void error_cb(AAudioStream *, void *, aaudio_result_t err)
{
	LOGW("audio: stream error %s", AAudio_convertResultToText(err));
}

AudioPlayer::AudioPlayer() : ring_(ring_frames * 2, 0.f)
{
	// The decoder: Opus needs its header (csd-0) and the codec delay and seek
	// preroll (csd-1, csd-2, nanoseconds) before the first packet.
	codec_ = AMediaCodec_createDecoderByType("audio/opus");
	if (codec_) {
		AMediaFormat *f = AMediaFormat_new();
		AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/opus");
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, 48000);
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 2);
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PCM_ENCODING, 4); // float
		uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0};
		int64_t delay = 0, preroll = 80000000;
		AMediaFormat_setBuffer(f, "csd-0", head, sizeof(head));
		AMediaFormat_setBuffer(f, "csd-1", &delay, sizeof(delay));
		AMediaFormat_setBuffer(f, "csd-2", &preroll, sizeof(preroll));
		if (AMediaCodec_configure(codec_, f, nullptr, nullptr, 0) != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
			LOGE("audio: cannot start the Opus decoder");
			AMediaCodec_delete(codec_);
			codec_ = nullptr;
		}
		AMediaFormat_delete(f);
	}
	open_stream();
	thread_ = std::thread([this] { decode_loop(); });
}

void AudioPlayer::open_stream()
{
	AAudioStreamBuilder *b = nullptr;
	if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
		return;
	AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);
	AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
	AAudioStreamBuilder_setSampleRate(b, 48000);
	AAudioStreamBuilder_setChannelCount(b, 2);
	AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
	AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
	AAudioStreamBuilder_setDataCallback(b, data_cb, this);
	AAudioStreamBuilder_setErrorCallback(b, error_cb, this);
	if (AAudioStreamBuilder_openStream(b, &stream_) != AAUDIO_OK)
		stream_ = nullptr;
	AAudioStreamBuilder_delete(b);
	if (!stream_) {
		LOGE("audio: cannot open an output stream");
		return;
	}
	AAudioStream_setBufferSizeInFrames(stream_, AAudioStream_getFramesPerBurst(stream_) * 2);
	AAudioStream_requestStart(stream_);
	LOGI("audio: %d Hz, burst %d frames, %s", AAudioStream_getSampleRate(stream_), AAudioStream_getFramesPerBurst(stream_),
	     AAudioStream_getSharingMode(stream_) == AAUDIO_SHARING_MODE_EXCLUSIVE ? "exclusive" : "shared");
}

AudioPlayer::~AudioPlayer()
{
	running_ = false;
	qcv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	if (stream_) {
		AAudioStream_requestStop(stream_);
		AAudioStream_close(stream_);
	}
	if (codec_) {
		AMediaCodec_stop(codec_);
		AMediaCodec_delete(codec_);
	}
}

void AudioPlayer::push(const uint8_t *opus, size_t n)
{
	if (!codec_)
		return;
	std::lock_guard<std::mutex> g(qlock_);
	if (queue_.size() > 64)
		queue_.pop_front();
	queue_.emplace_back(opus, opus + n);
	qcv_.notify_one();
}

void AudioPlayer::decode_loop()
{
	int64_t pts = 0;
	bool float_out = true;
	std::vector<float> conv;
	while (running_) {
		std::vector<uint8_t> pkt;
		{
			std::unique_lock<std::mutex> lk(qlock_);
			qcv_.wait_for(lk, std::chrono::milliseconds(20), [&] { return !queue_.empty() || !running_; });
			if (!queue_.empty()) {
				pkt = std::move(queue_.front());
				queue_.pop_front();
			}
		}
		if (!codec_)
			continue;
		if (!pkt.empty()) {
			ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 10000);
			if (idx >= 0) {
				size_t cap = 0;
				uint8_t *buf = AMediaCodec_getInputBuffer(codec_, size_t(idx), &cap);
				size_t n = std::min(cap, pkt.size());
				memcpy(buf, pkt.data(), n);
				AMediaCodec_queueInputBuffer(codec_, size_t(idx), 0, n, uint64_t(pts), 0);
				pts += 5000;
			}
		}
		for (;;) {
			AMediaCodecBufferInfo info;
			ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
			if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
				AMediaFormat *f = AMediaCodec_getOutputFormat(codec_);
				int32_t enc = 2;
				if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_PCM_ENCODING, &enc))
					enc = 2;
				float_out = enc == 4;
				LOGI("audio: decoder gives %s", float_out ? "float" : "16-bit");
				AMediaFormat_delete(f);
				continue;
			}
			if (idx < 0)
				break;
			size_t cap = 0;
			uint8_t *buf = AMediaCodec_getOutputBuffer(codec_, size_t(idx), &cap);
			if (buf && info.size > 0) {
				if (float_out) {
					write(reinterpret_cast<const float *>(buf + info.offset), size_t(info.size) / 8);
				} else {
					auto *p = reinterpret_cast<const int16_t *>(buf + info.offset);
					size_t n = size_t(info.size) / 2;
					conv.resize(n);
					for (size_t i = 0; i < n; i++)
						conv[i] = float(p[i]) / 32768.f;
					write(conv.data(), n / 2);
				}
			}
			AMediaCodec_releaseOutputBuffer(codec_, size_t(idx), false);
		}
	}
}

void AudioPlayer::write(const float *s, size_t frames)
{
	size_t w = w_.load(std::memory_order_relaxed);
	for (size_t i = 0; i < frames; i++) {
		ring_[w * 2] = s[2 * i];
		ring_[w * 2 + 1] = s[2 * i + 1];
		w = (w + 1) % ring_frames;
	}
	w_.store(w, std::memory_order_release);
}

int AudioPlayer::callback(float *out, int frames)
{
	size_t w = w_.load(std::memory_order_acquire), r = r_.load(std::memory_order_relaxed);
	auto have = [&] { return (w + ring_frames - r) % ring_frames; };
	if (have() > 4 * target_)
		r = (w + ring_frames - 2 * target_) % ring_frames; // drifted: drop back to twice the target
	int i = 0;
	if (started_ || have() >= target_) {
		started_ = true;
		while (i < frames && have() > 0) {
			out[2 * i] = ring_[r * 2];
			out[2 * i + 1] = ring_[r * 2 + 1];
			r = (r + 1) % ring_frames;
			i++;
		}
		if (i < frames)
			started_ = false; // ran dry: buffer up again
	}
	r_.store(r, std::memory_order_release);
	for (int j = i; j < frames; j++)
		out[2 * j] = out[2 * j + 1] = 0;
	return i;
}

double AudioPlayer::buffered_ms() const
{
	size_t w = w_.load(), r = r_.load();
	return double((w + ring_frames - r) % ring_frames) / 48.0;
}

} // namespace gm
