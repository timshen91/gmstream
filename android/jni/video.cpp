#include "video.h"

#include <cstring>

#include <android/hardware_buffer.h>
#include <media/NdkMediaFormat.h>
#include <unistd.h>

#include "log.h"

namespace gm {

static const char *mime_of(uint8_t codec)
{
	switch (codec) {
	case 0: return "video/avc";
	case 1: return "video/hevc";
	case 2: return "video/av01";
	default: return nullptr;
	}
}

static const char *qti_base(uint8_t codec)
{
	switch (codec) {
	case 0: return "c2.qti.avc.decoder";
	case 1: return "c2.qti.hevc.decoder";
	case 2: return "c2.qti.av1.decoder";
	default: return nullptr;
	}
}

// The reader and the images it has handed out: an image goes back to it when
// the screen is done with it, unless the reader (and with it every image) is gone.
struct VideoDecoder::ReaderState {
	std::mutex lock;
	bool alive = true;
};

VideoDecoder::~VideoDecoder()
{
	std::lock_guard<std::mutex> g(lock_);
	destroy();
}

void VideoDecoder::set_enabled(bool on)
{
	std::lock_guard<std::mutex> g(lock_);
	if (on == enabled_)
		return;
	enabled_ = on;
	destroy();
	need_key_ = true;
}

void VideoDecoder::reset()
{
	std::lock_guard<std::mutex> g(lock_);
	destroy();
	need_key_ = true;
}

void VideoDecoder::destroy()
{
	if (!codec_)
		return;
	running_ = false;
	if (out_thread_.joinable())
		out_thread_.join();
	AMediaCodec_stop(codec_);
	AMediaCodec_delete(codec_);
	codec_ = nullptr;
	std::shared_ptr<ReaderState> st;
	{
		std::lock_guard<std::mutex> g(state_lock_);
		st = std::move(state_);
	}
	{
		std::lock_guard<std::mutex> g(st->lock);
		st->alive = false;
	}
	AImageReader_delete(reader_);
	reader_ = nullptr;
	codec_id_ = 255;
	std::lock_guard<std::mutex> g(timings_lock_);
	timings_.clear();
}

static void on_image(void *ctx, AImageReader *r)
{
	static_cast<VideoDecoder *>(ctx)->image_available(r);
}

bool VideoDecoder::create(uint8_t codec, int w, int h)
{
	const char *mime = mime_of(codec);
	if (!mime)
		return false;
	// The decoder's pictures: buffers SurfaceFlinger can put on a display plane.
	AImageReader *reader = nullptr;
	if (AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_PRIVATE,
	                              AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY, 8,
	                              &reader) != AMEDIA_OK) {
		LOGE("video: no image reader");
		return false;
	}
	AImageReader_ImageListener listener{this, on_image};
	AImageReader_setImageListener(reader, &listener);
	ANativeWindow *win = nullptr;
	AImageReader_getWindow(reader, &win);
	auto try_config = [&](AMediaCodec *c, bool vendor) {
		AMediaFormat *f = AMediaFormat_new();
		AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w);
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 4 << 20);
		AMediaFormat_setInt32(f, "low-latency", 1);
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PRIORITY, 0); // real time
		if (vendor) {
			// What Moonlight (and its forks, for the fence) set on Qualcomm:
			// decode order out, no reordering wait, a software output fence.
			AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_OPERATING_RATE, 32767);
			AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
			AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
			AMediaFormat_setInt32(f, "vendor.qti-ext-output-sw-fence-enable.value", 1);
			AMediaFormat_setInt32(f, "vendor.qti-ext-output-fence.enable", 1);
			AMediaFormat_setInt32(f, "vendor.qti-ext-output-fence.fence_type", 1);
		}
		media_status_t s = AMediaCodec_configure(c, f, win, nullptr, 0);
		AMediaFormat_delete(f);
		return s == AMEDIA_OK;
	};
	AMediaCodec *c = nullptr;
	std::string name;
	if (const char *base = qti_base(codec)) {
		name = std::string(base) + ".low_latency";
		c = AMediaCodec_createCodecByName(name.c_str());
		if (c && !try_config(c, true) && !try_config(c, false)) {
			AMediaCodec_delete(c);
			c = nullptr;
		}
	}
	if (!c) {
		c = AMediaCodec_createDecoderByType(mime);
		char *n = nullptr;
		if (c) {
			name = AMediaCodec_getName(c, &n) == AMEDIA_OK && n ? n : mime;
			if (n)
				AMediaCodec_releaseName(c, n);
		}
		if (!c || (!try_config(c, true) && !try_config(c, false))) {
			if (c)
				AMediaCodec_delete(c);
			AImageReader_delete(reader);
			LOGE("video: cannot configure a %s decoder for %dx%d", mime, w, h);
			return false;
		}
	}
	if (AMediaCodec_start(c) != AMEDIA_OK) {
		AMediaCodec_delete(c);
		AImageReader_delete(reader);
		return false;
	}
	codec_ = c;
	reader_ = reader;
	{
		std::lock_guard<std::mutex> g(state_lock_);
		state_ = std::make_shared<ReaderState>();
	}
	name_ = name;
	codec_id_ = codec;
	width_ = w;
	height_ = h;
	running_ = true;
	out_thread_ = std::thread([this] { drain(); });
	LOGI("video: %s %dx%d", name.c_str(), w, h);
	if (on_size)
		on_size(w, h);
	return true;
}

bool VideoDecoder::decode(Frame &&f, FrameTiming t)
{
	std::lock_guard<std::mutex> g(lock_);
	if (!enabled_)
		return true; // nothing to show it on; a keyframe is asked for when a surface comes
	if (!codec_ || f.codec != codec_id_ || f.width != width_ || f.height != height_) {
		if (!f.key) {
			need_key_ = true;
			return false;
		}
		destroy();
		if (!create(f.codec, f.width, f.height))
			return false;
		need_key_ = false;
	}
	if (need_key_ && !f.key)
		return false;
	need_key_ = false;
	ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 20000);
	if (idx < 0) {
		LOGW("video: no input buffer");
		need_key_ = true;
		return false;
	}
	size_t cap = 0;
	uint8_t *buf = AMediaCodec_getInputBuffer(codec_, size_t(idx), &cap);
	if (!buf || cap < f.bits.size()) {
		AMediaCodec_queueInputBuffer(codec_, size_t(idx), 0, 0, 0, 0);
		need_key_ = true;
		return false;
	}
	memcpy(buf, f.bits.data(), f.bits.size());
	int64_t pts = int64_t(f.id) * 1000;
	t.decode_start = now_ns();
	{
		std::lock_guard<std::mutex> tg(timings_lock_);
		timings_[pts] = {t, f.width, f.height};
		while (timings_.size() > 64)
			timings_.erase(timings_.begin());
	}
	AMediaCodec_queueInputBuffer(codec_, size_t(idx), 0, f.bits.size(), uint64_t(pts), 0);
	return true;
}

// Every decoded picture goes to the reader at once; the image listener takes it on.
void VideoDecoder::drain()
{
	while (running_) {
		AMediaCodecBufferInfo info;
		ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10000);
		if (idx < 0)
			continue;
		{
			std::lock_guard<std::mutex> g(timings_lock_);
			auto it = timings_.find(info.presentationTimeUs);
			if (it != timings_.end())
				it->second.t.decoded = now_ns();
		}
		AMediaCodec_releaseOutputBuffer(codec_, size_t(idx), true);
	}
}

void VideoDecoder::image_available(AImageReader *r)
{
	AImage *img = nullptr;
	int fence = -1;
	if (AImageReader_acquireNextImageAsync(r, &img, &fence) != AMEDIA_OK || !img)
		return;
	int64_t ts = 0;
	AImage_getTimestamp(img, &ts);
	AHardwareBuffer *ahb = nullptr;
	AImage_getHardwareBuffer(img, &ahb);
	Pending p{};
	bool known = false;
	std::shared_ptr<ReaderState> state;
	{
		std::lock_guard<std::mutex> g(timings_lock_);
		auto it = timings_.find(ts / 1000);
		if (it != timings_.end()) {
			p = it->second;
			timings_.erase(it);
			known = true;
		}
	}
	{
		std::lock_guard<std::mutex> g(state_lock_);
		state = state_;
	}
	if (!state || !ahb) {
		if (fence >= 0)
			close(fence);
		AImage_delete(img);
		return;
	}
	if (!known) {
		p.w = width_;
		p.h = height_;
		p.t.decoded = now_ns();
	}
	if (!p.t.decoded)
		p.t.decoded = now_ns();
	presenter_.present(ahb, fence, p.w, p.h, p.t, [img, state](int release) {
		std::lock_guard<std::mutex> g(state->lock);
		if (state->alive)
			AImage_deleteAsync(img, release);
		else if (release >= 0)
			close(release);
	});
}

std::string VideoDecoder::summary()
{
	std::lock_guard<std::mutex> g(lock_);
	if (!codec_)
		return enabled_ ? "decoder: waiting for a keyframe" : "decoder: no surface";
	return name_ + " " + std::to_string(width_) + "x" + std::to_string(height_);
}

} // namespace gm
