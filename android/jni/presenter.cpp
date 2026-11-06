#include "presenter.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <android/data_space.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "conn.h"
#include "log.h"

namespace gm {

// When a fence signalled (CLOCK_MONOTONIC ns), waiting up to 50 ms for it; 0
// if it never did.
static uint64_t signal_time(int fd)
{
	if (fd < 0)
		return 0;
	pollfd p{fd, POLLIN, 0};
	if (poll(&p, 1, 50) <= 0)
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
	set_window(nullptr);
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
                        std::function<void(int)> release)
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
	if (crop_w != last_w_ || crop_h != last_h_ || vw != view_w_ || vh != view_h_) {
		// The picture's own rows only (the decoder's buffer may carry padding),
		// scaled to fill the view (Java letterboxes the view to the stream).
		ARect crop{0, 0, crop_w, crop_h};
		ASurfaceTransaction_setCrop(txn, sc_, crop);
		ASurfaceTransaction_setPosition(txn, sc_, 0, 0);
		ASurfaceTransaction_setScale(txn, sc_, float(vw) / float(crop_w), float(vh) / float(crop_h));
		ASurfaceTransaction_setVisibility(txn, sc_, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
		ASurfaceTransaction_setBufferTransparency(txn, sc_, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
		ASurfaceTransaction_setBufferDataSpace(txn, sc_, ADATASPACE_BT709);
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
