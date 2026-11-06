// Frames to the screen through a SurfaceControl of our own (a child of the
// stream's SurfaceView): each decoded buffer goes in its own transaction, with
// its acquire fence, cropped to the stream's picture and scaled to the view.
// SurfaceFlinger's completion callback gives each frame's latch time and its
// present fence, whose signal time is when it reached the screen; a frame
// replaced before it was latched shows up as one presented at the same
// refresh as the next, and counts as dropped. The phase lock and the stats
// both run on these times.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/surface_control.h>

#include "timing.h"

namespace gm {

class Presenter {
public:
	std::function<void(const FrameTiming &)> on_presented, on_dropped;

	~Presenter();
	// The SurfaceView's window (nullptr: gone). Takes its own reference.
	void set_window(ANativeWindow *parent);
	bool ready();
	// Shows `buf` (cropped to crop_w x crop_h from its top left) once `fence`
	// signals; `release` gets the buffer back with a fence (-1: now) when the
	// screen is done with it. Takes ownership of `fence`.
	void present(AHardwareBuffer *buf, int fence, int crop_w, int crop_h, FrameTiming t,
	             std::function<void(int)> release);
	void skip_one() { skip_ = true; }

	// Scanout mode (experimental): one buffer stays on the display plane and
	// frames are written into it in place; a commit goes in every refresh so
	// the (command-mode) panel is sent the buffer as it is at each refresh,
	// and a frame finished before that refresh's push is shown at it,
	// without waiting for SurfaceFlinger's latch. set_front(nullptr) ends it.
	void set_front(AHardwareBuffer *buf, int crop_w, int crop_h);
	bool front() const { return front_buf_ != nullptr; }
	// A frame written into the front buffer; `fence` (owned) signals when it is.
	void front_written(FrameTiming t, int fence);
	void front_complete(ASurfaceTransactionStats *stats); // SurfaceFlinger's thread
	void front_tick_public() { front_tick(); }
	// SurfaceFlinger's thread.
	void complete(uint64_t seq, ASurfaceTransactionStats *stats);

private:
	struct InFlight {
		uint64_t seq;
		FrameTiming t;
		std::function<void(int)> release;
		bool done = false, emitted = false;
		int64_t present = -1;
		int acquire = -1; // a copy of the acquire fence, for the decode's end time
	};
	void settle();

	void front_tick();
	void front_loop();
	AHardwareBuffer *front_buf_ = nullptr;
	int front_w_ = 0, front_h_ = 0;
	std::thread front_thread_;
	std::atomic<bool> front_run_{false};
	void *front_looper_ = nullptr;
	struct Written { FrameTiming t; int fence; uint64_t done; };
	std::deque<Written> written_;
	std::deque<uint64_t> presents_; // recent refreshes the buffer was sent at

	std::mutex lock_;
	ANativeWindow *parent_ = nullptr;
	ASurfaceControl *sc_ = nullptr;
	uint64_t seq_ = 0;
	std::deque<InFlight> flight_;
	bool skip_ = false;
	int last_w_ = 0, last_h_ = 0, view_w_ = 0, view_h_ = 0;
};

} // namespace gm
