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
#include <string>
#include <thread>
#include <vector>

#include <android/choreographer.h>

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/surface_control.h>

#include "panel.h"
#include "timing.h"

namespace gm {

class Presenter {
public:
	std::function<void(const FrameTiming &)> on_presented, on_dropped;
	void set_schedule(const PanelSchedule &s) { schedule_ = s; }
	const PanelSchedule &schedule() const { return schedule_; }

	~Presenter();
	// The SurfaceView's window (nullptr: gone). Takes its own reference.
	void set_window(ANativeWindow *parent);
	bool ready();
	// Shows `buf` (cropped to crop_w x crop_h from its top left) once `fence`
	// signals; `release` gets the buffer back with a fence (-1: now) when the
	// screen is done with it. Takes ownership of `fence`.
	void present(AHardwareBuffer *buf, int fence, int crop_w, int crop_h, FrameTiming t,
	             std::function<void(int)> release, int32_t dataspace, int32_t transform = 0);
	void skip_one() { skip_ = true; }

	// Scanout ring (compositor.h): `bufs` are committed in turn, one per
	// refresh, each for a refresh SurfaceFlinger's latch ahead (frame
	// timelines), with no fence: the compositor writes each just before the
	// panel reads it. Empty: off.
	void set_ring(std::vector<AHardwareBuffer *> bufs, int crop_w, int crop_h, int32_t transform);
	bool ring() const { return ring_on_; }
	struct Commit {
		uint64_t seq;
		int buffer;
		uint64_t predicted; // its present (transfer start), from the frame timeline and what came before
	};
	// Committed and not yet presented, oldest first.
	std::vector<Commit> upcoming();
	// Each commit's real present (0: never presented); SurfaceFlinger's thread.
	std::function<void(uint64_t seq, int buffer, uint64_t present)> on_ring_presented;
	void ring_tick(const AChoreographerFrameCallbackData *data); // the vsync thread
	void ring_complete(uint64_t seq, ASurfaceTransactionStats *stats); // SurfaceFlinger's thread
	std::string ring_summary();
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

	void ring_loop();
	PanelSchedule schedule_;
	std::vector<AHardwareBuffer *> ring_bufs_;
	std::atomic<bool> ring_on_{false}, ring_run_{false};
	int ring_w_ = 0, ring_h_ = 0;
	int32_t ring_transform_ = 0;
	std::thread ring_thread_;
	void *ring_looper_ = nullptr;
	uint64_t ring_seq_ = 0;
	struct Pending { Commit c; uint64_t expected; };
	std::deque<Pending> ring_pending_;
	std::vector<double> ring_offsets_; // present - the timeline's expected present, ns
	double ring_offset_ = 0;
	int ring_slips_ = 0, ring_lost_ = 0;

	std::mutex lock_;
	ANativeWindow *parent_ = nullptr;
	ASurfaceControl *sc_ = nullptr;
	uint64_t seq_ = 0;
	std::deque<InFlight> flight_;
	bool skip_ = false;
	int last_w_ = 0, last_h_ = 0, view_w_ = 0, view_h_ = 0;
	int32_t last_dataspace_ = -1, last_transform_ = -1;
	uint64_t last_present_ = 0; // the previous transaction's present (debug: release timing)
};

} // namespace gm
