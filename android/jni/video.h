// H.264, HEVC and AV1 through MediaCodec (Qualcomm's low-latency decoders
// where present). The decoder renders into an AImageReader, and each image
// goes to the screen through the Presenter (a SurfaceControl of our own), so
// every frame's timings ride with it by its time stamp and its real present
// time comes back.

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>

#include "conn.h"
#include "presenter.h"
#include "timing.h"

namespace gm {

class VideoDecoder {
public:
	std::function<void(int, int)> on_size; // the stream picture's size

	explicit VideoDecoder(Presenter &p) : presenter_(p) {}
	~VideoDecoder();
	// Whether there is a surface to show frames on (frames are skipped until there is).
	void set_enabled(bool on);
	// Network thread. False: it could not be decoded (ask for a keyframe).
	bool decode(Frame &&f, FrameTiming t);
	void reset();
	std::string summary();
	void image_available(AImageReader *r); // the reader's thread

	struct ReaderState; // images handed out, shared with their release callbacks

private:
	bool create(uint8_t codec, int w, int h);
	void destroy();
	void drain();

	Presenter &presenter_;
	std::mutex lock_;
	bool enabled_ = false;
	AMediaCodec *codec_ = nullptr;
	AImageReader *reader_ = nullptr;
	std::mutex state_lock_; // only for state_ (the image listener must not wait on lock_)
	std::shared_ptr<ReaderState> state_;
	std::string name_;
	uint8_t codec_id_ = 255;
	int width_ = 0, height_ = 0;
	bool need_key_ = true;
	std::thread out_thread_;
	std::atomic<bool> running_{false};
	std::mutex timings_lock_;
	struct Pending { FrameTiming t; int w, h; };
	std::map<int64_t, Pending> timings_; // by pts (us)
};

} // namespace gm
