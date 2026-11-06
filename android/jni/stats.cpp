#include "stats.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "conn.h"

namespace gm {

static constexpr uint64_t window_ns = 5000000000ull;

const std::vector<std::string> &StageStats::names()
{
	// As on the Mac, minus its hold and render stages: "to Android" is
	// gamescope's present -> the frame handed to SurfaceFlinger, "display" from
	// there to on screen.
	static const std::vector<std::string> n = {"encode", "network", "overlap", "read", "decode",
	                                           "to Android", "display", "total", "spacing"};
	return n;
}

void StageStats::add(const FrameTiming &t)
{
	auto ms = [](uint64_t a, uint64_t b) { return double(int64_t(b) - int64_t(a)) / 1e6; };
	double overlap = double(t.overlap_ns) / 1e6;
	Entry e;
	e.at = t.presented;
	e.v.assign(names().size(), 0);
	e.has.assign(names().size(), false);
	auto set = [&](size_t i, double v) { e.v[i] = v; e.has[i] = true; };
	set(0, double(t.encode_ns) / 1e6);
	if (t.local)
		set(1, ms(t.encoded_local, t.received) + overlap);
	set(2, -overlap);
	if (t.decode_start)
		set(3, ms(t.received, t.decode_start));
	if (t.decoded)
		set(4, ms(t.decode_start ? t.decode_start : t.received, t.decoded));
	if (t.local)
		set(5, ms(t.present_local, t.submitted));
	set(6, ms(t.submitted, t.presented));
	if (t.local)
		set(7, ms(t.present_local, t.presented));
	std::lock_guard<std::mutex> g(lock_);
	if (last_presented_ && last_present_ && t.local && t.present_local > last_present_ &&
	    t.present_local - last_present_ < 100000000ull)
		set(8, std::fabs(double(int64_t(t.presented) - int64_t(last_presented_)) -
		                 double(t.present_local - last_present_)) / 1e6);
	last_presented_ = t.presented;
	last_present_ = t.local ? t.present_local : 0;
	shown_++;
	frames_.push_back(std::move(e));
}

void StageStats::drop()
{
	std::lock_guard<std::mutex> g(lock_);
	dropped_++;
}

StageStats::Taken StageStats::take()
{
	std::lock_guard<std::mutex> g(lock_);
	uint64_t cutoff = now_ns() - window_ns;
	frames_.erase(std::remove_if(frames_.begin(), frames_.end(), [&](const Entry &e) { return e.at < cutoff; }),
	              frames_.end());
	Taken out;
	for (size_t i = 0; i < names().size(); i++) {
		std::vector<double> s;
		for (auto &f : frames_)
			if (f.has[i])
				s.push_back(f.v[i]);
		std::sort(s.begin(), s.end());
		Row r{names()[i], !s.empty(), 0, 0, 0};
		if (!s.empty()) {
			r.p50 = s[s.size() / 2];
			r.p99 = s[std::min(s.size() - 1, size_t(double(s.size()) * 0.99))];
			r.max = s.back();
		}
		out.rows.push_back(r);
	}
	out.shown = shown_;
	out.dropped = dropped_;
	shown_ = dropped_ = 0;
	return out;
}

void Throughput::take(double &mbps, int &gaps)
{
	std::lock_guard<std::mutex> g(lock_);
	uint64_t now = now_ns();
	double secs = since_ ? double(now - since_) / 1e9 : 1;
	mbps = double(bytes_) * 8 / 1e6 / std::max(secs, 0.001);
	gaps = gaps_;
	bytes_ = 0;
	gaps_ = 0;
	since_ = now;
}

bool FrameGate::admit(uint32_t id, bool key, bool &request_keyframe, Throughput &stats)
{
	request_keyframe = false;
	if (has_expect_ && id == expect_) {
		expect_ = id + 1;
		return true;
	}
	if (key) {
		has_expect_ = true;
		expect_ = id + 1;
		return true;
	}
	if (has_expect_ && int32_t(id - expect_) < 0)
		return false; // stale
	if (has_expect_)
		stats.gap();
	has_expect_ = false;
	uint64_t now = now_ns();
	if (now - last_request_ > 200000000ull) {
		last_request_ = now;
		request_keyframe = true;
	}
	return false;
}

} // namespace gm
