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
#include <mutex>

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

	std::mutex lock_;
	ANativeWindow *parent_ = nullptr;
	ASurfaceControl *sc_ = nullptr;
	uint64_t seq_ = 0;
	std::deque<InFlight> flight_;
	bool skip_ = false;
	int last_w_ = 0, last_h_ = 0, view_w_ = 0, view_h_ = 0;
};

} // namespace gm
