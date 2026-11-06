#include "presenter.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <android/data_space.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/system_properties.h>
#include <algorithm>
#include <thread>

#include <android/choreographer.h>
#include <android/looper.h>

#include "conn.h"
#include "log.h"

namespace gm {

static bool debug_fences()
{
	char v[PROP_VALUE_MAX] = {};
	return __system_property_get("debug.gmstream.fences", v) > 0 && v[0] == '1';
}

// When a fence signalled (CLOCK_MONOTONIC ns), waiting up to 50 ms for it; 0
// if it never did.
static uint64_t signal_time(int fd, int wait_ms = 50)
{
	if (fd < 0)
		return 0;
	pollfd p{fd, POLLIN, 0};
	if (poll(&p, 1, wait_ms) <= 0)
		return 0;
	sync_file_info info{};
	if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.num_fences == 0)
		return 0;
	std::vector<sync_fence_info> fences(info.num_fences);
	info.sync_fence_info = reinterpret_cast<uint64_t>(fences.data());
	if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0)
		return 0;
	uint64_t t = 0;
	for (auto &f : fences)
		if (f.status == 1 && f.timestamp_ns > t)
			t = f.timestamp_ns;
	return t;
}

Presenter::~Presenter()
{
	set_ring({}, 0, 0, 0);
	set_window(nullptr);
}

// ---- Scanout ring -------------------------------------------------------------

void Presenter::set_ring(std::vector<AHardwareBuffer *> bufs, int crop_w, int crop_h, int32_t transform)
{
	if (ring_thread_.joinable()) {
		ring_run_ = false;
		if (ring_looper_)
			ALooper_wake(static_cast<ALooper *>(ring_looper_));
		ring_thread_.join();
	}
	{
		std::lock_guard<std::mutex> g(lock_);
		ring_bufs_ = std::move(bufs);
		ring_w_ = crop_w;
		ring_h_ = crop_h;
		ring_transform_ = transform;
		ring_pending_.clear();
		ring_offsets_.clear();
		ring_offset_ = 0;
		ring_slips_ = ring_lost_ = 0;
		last_w_ = last_h_ = 0; // geometry again
		ring_on_ = !ring_bufs_.empty();
	}
	if (!ring_on_)
		return;
	ring_run_ = true;
	ring_thread_ = std::thread([this] { ring_loop(); });
	LOGI("presenter: scanout ring of %zu buffers", ring_bufs_.size());
}

static thread_local AChoreographer *t_choreographer;

static void on_vsync(const AChoreographerFrameCallbackData *data, void *ctx)
{
	auto *p = static_cast<Presenter *>(ctx);
	p->ring_tick(data);
	AChoreographer_postVsyncCallback(t_choreographer, on_vsync, ctx);
}

void Presenter::ring_loop()
{
	ALooper *l = ALooper_prepare(0);
	ring_looper_ = l;
	t_choreographer = AChoreographer_getInstance();
	AChoreographer_postVsyncCallback(t_choreographer, on_vsync, this);
	while (ring_run_)
		ALooper_pollOnce(100, nullptr, nullptr, nullptr);
	ring_looper_ = nullptr;
}

struct RingCtx {
	Presenter *p;
	uint64_t seq;
};

static void on_ring_complete(void *ctx, ASurfaceTransactionStats *stats)
{
	auto *c = static_cast<RingCtx *>(ctx);
	c->p->ring_complete(c->seq, stats);
	delete c;
}

// Every refresh: the next buffer of the ring, for the frame timeline
// SurfaceFlinger prefers (it latches it in time for that timeline's present).
// The buffer's content is not there yet: the compositor writes it just
// before its transfer, long after the latch.
void Presenter::ring_tick(const AChoreographerFrameCallbackData *data)
{
	std::lock_guard<std::mutex> g(lock_);
	if (!sc_ || ring_bufs_.empty() || !ring_run_)
		return;
	size_t i = AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(data);
	AVsyncId vsync = AChoreographerFrameCallbackData_getFrameTimelineVsyncId(data, i);
	int64_t expected = AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(data, i);
	ASurfaceTransaction *txn = ASurfaceTransaction_create();
	int vw = ANativeWindow_getWidth(parent_), vh = ANativeWindow_getHeight(parent_);
	if (ring_w_ != last_w_ || ring_h_ != last_h_ || vw != view_w_ || vh != view_h_) {
		ASurfaceTransaction_setBufferTransform(txn, sc_, ring_transform_);
		last_transform_ = ring_transform_;
		ARect crop{0, 0, ring_w_, ring_h_};
		ASurfaceTransaction_setCrop(txn, sc_, crop);
		ASurfaceTransaction_setPosition(txn, sc_, 0, 0);
		ASurfaceTransaction_setScale(txn, sc_, float(vw) / float(ring_w_), float(vh) / float(ring_h_));
		ASurfaceTransaction_setVisibility(txn, sc_, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
		ASurfaceTransaction_setBufferTransparency(txn, sc_, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
		ASurfaceTransaction_setBufferDataSpace(txn, sc_, ADATASPACE_SRGB);
		last_dataspace_ = ADATASPACE_SRGB;
		last_w_ = ring_w_;
		last_h_ = ring_h_;
		view_w_ = vw;
		view_h_ = vh;
	}
	uint64_t seq = ++ring_seq_;
	int b = int(seq % ring_bufs_.size());
	ASurfaceTransaction_setBuffer(txn, sc_, ring_bufs_[size_t(b)], -1);
	ASurfaceTransaction_setFrameTimeline(txn, vsync);
	ASurfaceTransaction_setOnComplete(txn, new RingCtx{this, seq}, on_ring_complete);
	ASurfaceTransaction_apply(txn);
	ASurfaceTransaction_delete(txn);
	ring_pending_.push_back({{seq, b, uint64_t(double(expected) + ring_offset_)}, uint64_t(expected)});
	while (ring_pending_.size() > 8)
		ring_pending_.pop_front(); // never completed
}

std::vector<Presenter::Commit> Presenter::upcoming()
{
	std::lock_guard<std::mutex> g(lock_);
	std::vector<Commit> out;
	for (auto &p : ring_pending_)
		out.push_back({p.c.seq, p.c.buffer, uint64_t(double(p.expected) + ring_offset_)});
	return out;
}

void Presenter::ring_complete(uint64_t seq, ASurfaceTransactionStats *stats)
{
	int present_fd = ASurfaceTransactionStats_getPresentFenceFd(stats);
	uint64_t present = signal_time(present_fd);
	if (present_fd >= 0)
		close(present_fd);
	int buffer = -1;
	{
		std::lock_guard<std::mutex> g(lock_);
		if (sc_) {
			int rel = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, sc_);
			if (rel >= 0)
				close(rel); // nothing waits on these: the compositor works to the schedule
		}
		for (auto it = ring_pending_.begin(); it != ring_pending_.end(); ++it) {
			if (it->c.seq != seq)
				continue;
			buffer = it->c.buffer;
			if (present) {
				// The timeline's expected present against the present fence
				// (the transfer's start): a fixed offset, measured.
				double off = double(int64_t(present - it->expected));
				double period = schedule_.period_ns;
				if (ring_offsets_.size() >= 8 && std::fabs(off - ring_offset_) > period / 2)
					ring_slips_++; // shown a refresh away from the one it was committed for
				else {
					ring_offsets_.push_back(off);
					if (ring_offsets_.size() > 240)
						ring_offsets_.erase(ring_offsets_.begin());
					std::vector<double> s = ring_offsets_;
					std::nth_element(s.begin(), s.begin() + long(s.size() / 2), s.end());
					ring_offset_ = s[s.size() / 2];
				}
			} else {
				ring_lost_++;
			}
			ring_pending_.erase(it);
			break;
		}
	}
	if (buffer >= 0 && on_ring_presented)
		on_ring_presented(seq, buffer, present);
}

std::string Presenter::ring_summary()
{
	std::lock_guard<std::mutex> g(lock_);
	char b[160];
	snprintf(b, sizeof(b), "ring: %zu buffers, present = timeline %+.2f ms, slipped %d, lost %d",
	         ring_bufs_.size(), ring_offset_ / 1e6, ring_slips_, ring_lost_);
	ring_slips_ = ring_lost_ = 0;
	return b;
}

void Presenter::set_window(ANativeWindow *parent)
{
	std::lock_guard<std::mutex> g(lock_);
	if (parent == parent_)
		return;
	if (sc_) {
		ASurfaceTransaction *t = ASurfaceTransaction_create();
		ASurfaceTransaction_reparent(t, sc_, nullptr);
		ASurfaceTransaction_apply(t);
		ASurfaceTransaction_delete(t);
		ASurfaceControl_release(sc_);
		sc_ = nullptr;
	}
	if (parent_)
		ANativeWindow_release(parent_);
	parent_ = parent;
	last_w_ = last_h_ = view_w_ = view_h_ = 0;
	if (!parent)
		return;
	ANativeWindow_acquire(parent);
	sc_ = ASurfaceControl_createFromWindow(parent, "gmstream video");
	if (!sc_)
		LOGE("presenter: cannot create a SurfaceControl");
}

bool Presenter::ready()
{
	std::lock_guard<std::mutex> g(lock_);
	return sc_ != nullptr;
}

struct CompleteCtx {
	Presenter *p;
	uint64_t seq;
};

static void on_complete(void *ctx, ASurfaceTransactionStats *stats)
{
	auto *c = static_cast<CompleteCtx *>(ctx);
	c->p->complete(c->seq, stats);
	delete c;
}

void Presenter::present(AHardwareBuffer *buf, int fence, int crop_w, int crop_h, FrameTiming t,
                        std::function<void(int)> release, int32_t dataspace, int32_t transform)
{
	std::unique_lock<std::mutex> g(lock_);
	if (!sc_ || skip_) {
		skip_ = false;
		g.unlock();
		if (fence >= 0)
			close(fence);
		release(-1);
		if (on_dropped)
			on_dropped(t);
		return;
	}
	ASurfaceTransaction *txn = ASurfaceTransaction_create();
	int vw = ANativeWindow_getWidth(parent_), vh = ANativeWindow_getHeight(parent_);
	if (crop_w != last_w_ || crop_h != last_h_ || vw != view_w_ || vh != view_h_ || dataspace != last_dataspace_ ||
	    transform != last_transform_) {
		// A buffer drawn in the panel's own orientation says so, and the
		// display then scans it out without rotating it (Qualcomm's display
		// engine rotates only compressed buffers; anything else costs a GPU
		// composite every frame).
		ASurfaceTransaction_setBufferTransform(txn, sc_, transform);
		last_transform_ = transform;
		// The picture's own rows only (the decoder's buffer may carry padding),
		// scaled to fill the view (Java letterboxes the view to the stream).
		ARect crop{0, 0, crop_w, crop_h};
		ASurfaceTransaction_setCrop(txn, sc_, crop);
		ASurfaceTransaction_setPosition(txn, sc_, 0, 0);
		ASurfaceTransaction_setScale(txn, sc_, float(vw) / float(crop_w), float(vh) / float(crop_h));
		ASurfaceTransaction_setVisibility(txn, sc_, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
		ASurfaceTransaction_setBufferTransparency(txn, sc_, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
		// What the buffer holds: YUV from the video decoder (BT.709), or RGB
		// (sRGB). An RGB buffer tagged as video can't go on a display plane
		// and costs a GPU composite every frame.
		ASurfaceTransaction_setBufferDataSpace(txn, sc_, ADataSpace(dataspace));
		last_dataspace_ = dataspace;
		last_w_ = crop_w;
		last_h_ = crop_h;
		view_w_ = vw;
		view_h_ = vh;
	}
	ASurfaceTransaction_setBuffer(txn, sc_, buf, fence);
	uint64_t seq = ++seq_;
	ASurfaceTransaction_setOnComplete(txn, new CompleteCtx{this, seq}, on_complete);
	t.submitted = now_ns();
	flight_.push_back({seq, t, std::move(release)});
	if (!t.decoded && fence >= 0)
		flight_.back().acquire = dup(fence); // the frame was done on the GPU when this signalled
	while (flight_.size() > 16) {
		// Never completed (should not happen): give up on the oldest.
		if (flight_.front().release)
			flight_.front().release(-1);
		flight_.pop_front();
	}
	ASurfaceTransaction_apply(txn);
	ASurfaceTransaction_delete(txn);
}

void Presenter::complete(uint64_t seq, ASurfaceTransactionStats *stats)
{
	int64_t latch = ASurfaceTransactionStats_getLatchTime(stats);
	int present_fd = ASurfaceTransactionStats_getPresentFenceFd(stats);
	uint64_t present = signal_time(present_fd);
	if (present_fd >= 0)
		close(present_fd);
	int release_fd = -1;
	{
		std::lock_guard<std::mutex> g(lock_);
		if (sc_)
			release_fd = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, sc_);
	}
	std::function<void(int)> release;
	{
		std::lock_guard<std::mutex> g(lock_);
		for (auto &f : flight_) {
			if (f.seq == seq) {
				f.done = true;
				f.present = present ? int64_t(present) : -1;
				f.t.latched = latch > 0 ? uint64_t(latch) : 0;
				if (f.acquire >= 0) {
					f.t.decoded = signal_time(f.acquire);
					close(f.acquire);
					f.acquire = -1;
				}
			}
			// The buffer before this one is SurfaceFlinger's to release now.
			if (f.seq + 1 == seq && f.release) {
				release = std::move(f.release);
				f.release = nullptr;
			}
		}
	}
	if (debug_fences() && release_fd >= 0 && present) {
		// Measuring: when the display stopped reading the buffer this one
		// replaced, against this one's present fence (the question being
		// whether the present fence is the start or the end of the push).
		int dupfd = dup(release_fd);
		std::thread([dupfd, present] {
			uint64_t rel = signal_time(dupfd, 40);
			close(dupfd);
			static std::mutex m;
			static std::vector<double> v;
			std::lock_guard<std::mutex> g(m);
			if (rel)
				v.push_back((double(int64_t(rel) - int64_t(present))) / 1e6);
			if (v.size() >= 120) {
				std::sort(v.begin(), v.end());
				LOGI("fences: previous buffer released at present %+.3f ms p1, %+.3f p50, %+.3f p99 (n %zu)",
				     v[v.size() / 100], v[v.size() / 2], v[v.size() * 99 / 100], v.size());
				v.clear();
			}
		}).detach();
	}
	if (release)
		release(release_fd);
	else if (release_fd >= 0)
		close(release_fd);
	settle();
}

// Frames in order: one whose successor came out at the same refresh (or that
// never came out) was replaced before it was shown.
void Presenter::settle()
{
	std::vector<std::pair<FrameTiming, bool>> out;
	{
		std::lock_guard<std::mutex> g(lock_);
		for (size_t i = 0; i + 1 < flight_.size(); i++) {
			InFlight &a = flight_[i], &b = flight_[i + 1];
			if (a.emitted)
				continue;
			if (!a.done || !b.done)
				break;
			bool shown = a.present > 0 && a.present != b.present;
			a.t.presented = shown ? uint64_t(a.present) : 0;
			a.emitted = true;
			out.emplace_back(a.t, shown);
		}
		// Gone once told and given back.
		while (!flight_.empty() && flight_.front().emitted && !flight_.front().release)
			flight_.pop_front();
	}
	for (auto &[t, shown] : out) {
		if (shown && on_presented)
			on_presented(t);
		else if (!shown && on_dropped)
			on_dropped(t);
	}
}

} // namespace gm
