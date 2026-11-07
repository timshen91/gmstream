// Scanout compositor: the newest decoded frame written into the buffer the
// panel reads next, just before it reads it, on the panel's schedule
// (panel.h). Both decoders hand their pictures here: Riptide its planes, the
// video decoder (MediaCodec) its own buffer, sampled through its YCbCr
// conversion. Neither waits for SurfaceFlinger's latch.
//
// How: a ring of three RGBA buffers in the panel's orientation, committed to
// SurfaceFlinger one per refresh for the refresh a latch ahead (the
// Presenter's ring mode), with no fence. Buffer k%3 is read during refresh
// k's transfer only (from P - prefill to P + transfer) and is idle for the two
// refreshes between, so it can be written any time in that ~17 ms. The
// compositor writes it at P - prefill - budget, with whatever frame is newest
// and ready then; budget covers the compose (wake-up, submit, the GPU's work,
// measured). Frames never wait in a queue: one that is not ready at a sample
// point goes at the next refresh's; one overtaken before it is sampled is
// dropped. A compose that ends after the prefetch began is a tear (the rows
// already fetched show the buffer's old content), reported with its row.
//
// The compose is a compute pass on the GPU's compute queue, a queue family of
// its own at high priority, so it does not wait behind Riptide's decode on
// the graphics queue.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <android/hardware_buffer.h>

#include "presenter.h"
#include "timing.h"

typedef struct VkImageView_T *VkImageView; // as vulkan_core.h has it (64-bit)

namespace gm {

struct Gpu;

class Compositor {
public:
	explicit Compositor(Presenter &p);
	~Compositor();
	// The client's one Vulkan device (made on first use; Riptide decodes on it).
	// Null in a build without Vulkan.
	Gpu *gpu();

	// The frame shown (latched = the sample point that took it, presented =
	// the transfer's start), or overtaken before a sample point took it, or
	// torn (its compose ended after the buffer's prefetch began).
	std::function<void(const FrameTiming &)> on_presented, on_dropped;
	std::function<void(const FrameTiming &, int row)> on_torn;
	// How much later each shown frame could have come and still made its
	// refresh (streamed: whole, against its rows' deadlines; whole frames:
	// its sample point). Negative: it tore, or only just missed the refresh
	// before and went out a refresh late. The phase lock steers on it.
	std::function<void(double ns)> on_slack;

	// Starts (or restarts, for a new size) with pictures of w x h shown turned
	// for the display's rotation (Surface.ROTATION_*). False: no GPU, or the
	// buffers can't be made; the caller then presents through SurfaceFlinger.
	// `turned`: the pictures come already turned to the panel (Riptide with
	// the Rotation message: w x h is then the panel's orientation), and can be
	// streamed (stream_stripe).
	bool start(int w, int h, int rotation, bool turned = false);
	void stop();
	bool running() const { return running_; }
	bool streaming() const { return running_ && turned_; }

	// Streaming: stripe k (of `count`) of `frame`, rows [y, y + rows) of a
	// turned picture, whose rows are the panel's: composed into the buffer of
	// a refresh claimed for the frame when its first stripe comes, as soon as
	// `fence` (owned) says it is decoded, ahead of the panel reading those
	// rows. The panel reads row r from P - prefill + max(0, r - prefill rows) *
	// line (panel.h), so stripe 0 has to be there by the prefetch and each
	// later one only by the time the scan reaches it: the decode runs inside
	// the scan. The frame's whole picture (planes) then serves the refreshes
	// after, as submit_planes. Network thread.
	void stream_stripe(uint32_t frame, int k, int count, const VkImageView planes[3], int w, int h, int y, int rows,
	                   int fence, const FrameTiming &t);

	// A picture (any thread), ready on the GPU when `fence` signals (owned; -1:
	// ready now). Riptide's planes (Y, Cb, Cr: GENERAL layout, shared with the
	// compute queue)...
	void submit_planes(const VkImageView planes[3], int w, int h, int fence, const FrameTiming &t);
	// ...or the video decoder's buffer, cropped to crop_w x crop_h; `release`
	// is called when the compositor is done with it.
	void submit_image(AHardwareBuffer *ahb, int fence, int crop_w, int crop_h, const FrameTiming &t,
	                  std::function<void()> release);
	// The planes' images, or the decoder's buffers, are going away: waits for
	// every compose that reads them and lets go of them.
	void forget_planes();
	void forget_images();

	// From the Presenter (SurfaceFlinger's thread): a commit's real present.
	void ring_presented(uint64_t seq, int buffer, uint64_t present);
	std::string summary();

	struct Impl;

private:
	Presenter &presenter_;
	std::unique_ptr<Impl> impl_;
	std::atomic<bool> running_{false};
	bool turned_ = false;
};

} // namespace gm
