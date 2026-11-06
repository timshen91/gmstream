#include "phase.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "conn.h" // now_ns
#include "log.h"
#include <sys/system_properties.h>

static bool debug_phase()
{
	char v[PROP_VALUE_MAX] = {};
	return __system_property_get("debug.gmstream.phase", v) > 0 && v[0] == '1';
}

namespace gm {

static constexpr double bin_ns = 0.25e6, sweep_step = 1e6, enough = 0.999;
static constexpr int settle_steps = 3;

static double fmod_pos(double x, double m)
{
	double y = std::fmod(x, m);
	return y < 0 ? y + m : y;
}

static double median(std::vector<double> v)
{
	if (v.empty())
		return 0;
	std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
	return v[v.size() / 2];
}

void PhaseLock::restart(double refresh_hz)
{
	std::lock_guard<std::mutex> g(lock_);
	if (refresh_hz > 0) {
		nominal_ = 1e9 / refresh_hz;
		last_ = previous_ = 0;
		period_ = 0;
		gaps_.clear();
	}
	start_sweep();
}

void PhaseLock::start_sweep()
{
	slack_steps_ = 0;
	slack_.clear();
	sweeping_ = true;
	sweep_.clear();
	has_target_ = false;
	bin_all_.clear();
	bin_dropped_.clear();
	recent_.clear();
	margin_ = 3e6;
	steps_ = 0;
	samples_.clear();
	leads_.clear();
}

double PhaseLock::lead(uint64_t submitted) const
{
	return fmod_pos(double(int64_t(last_) - int64_t(submitted)), interval());
}

bool PhaseLock::late(const FrameTiming &t, double &out)
{
	if (!t.local)
		return false;
	out = double(int64_t(t.submitted) - int64_t(t.present_local));
	return true;
}

void PhaseLock::slack(double ns)
{
	std::lock_guard<std::mutex> g(lock_);
	slack_.push_back(ns);
}

void PhaseLock::dropped(const FrameTiming &t)
{
	std::lock_guard<std::mutex> g(lock_);
	drops_++;
	if (t.submitted > 0 && last_) {
		Lead l{lead(t.submitted), false, false, 0};
		l.has_late = late(t, l.late);
		leads_.push_back(l);
	}
}

void PhaseLock::presented(const FrameTiming &t)
{
	std::lock_guard<std::mutex> g(lock_);
	if (t.presented != last_) {
		previous_ = last_;
		last_ = t.presented;
		measure();
	}
	if (t.submitted == 0 || !last_)
		return;
	double a = lead(t.submitted);
	double display = double(int64_t(t.presented) - int64_t(t.submitted));
	bool on_time = display <= a + interval() * 1.5;
	Lead l{a, on_time, false, 0};
	l.has_late = late(t, l.late);
	leads_.push_back(l);
	if (t.submitted <= since_)
		return;
	samples_.push_back(display);
	shown_++;
	if (!on_time)
		late_shown_++;
}

void PhaseLock::measure()
{
	if (!previous_ || last_ <= previous_)
		return;
	double d = double(last_ - previous_), k = std::round(d / nominal_);
	if (k < 1 || k > 4 || std::fabs(d / k - nominal_) >= nominal_ * 0.02)
		return;
	gaps_.push_back(d / k);
	if (gaps_.size() > 512)
		gaps_.erase(gaps_.begin(), gaps_.begin() + long(gaps_.size() - 512));
	if (gaps_.size() >= 60)
		period_ = median(gaps_);
}

bool PhaseLock::step(uint64_t &anchor, uint64_t &interval_out)
{
	std::lock_guard<std::mutex> g(lock_);
	if (!last_)
		return false;
	const double t = interval();
	std::vector<double> d = samples_;
	std::sort(d.begin(), d.end());
	std::vector<Lead> step = leads_;
	int late = late_shown_, all = shown_;
	int drops = drops_;
	samples_.clear();
	leads_.clear();
	late_shown_ = shown_ = 0;
	drops_ = 0;
	steps_++;
	auto result = [&] {
		anchor = uint64_t(int64_t(last_) - int64_t(shift_));
		interval_out = uint64_t(t);
		return true;
	};
	if (steps_ <= settle_steps || d.size() < 10) {
		slack_.clear();
		return result();
	}
	if (slack_.size() >= 20) {
		// Streamed frames: each says how much later it could have come and
		// still gone out whole at its refresh (Compositor::on_slack). Move the
		// host's frames later until the earliest percent of them have just a
		// small margin left; back at once on one that tore or went late. (No
		// sweep: the slack is what the sweep would look for.)
		std::vector<double> s = slack_;
		slack_.clear();
		std::sort(s.begin(), s.end());
		double low = s[s.size() / 100], least = s.front();
		static constexpr double keep = 0.5e6;
		// A larger shift moves the host's frames earlier (measured: +2 ms of
		// shift gave +2 ms of slack). Gently: a step sees ~60 frames, and
		// their earliest percent is one or two of them.
		double move = std::clamp((keep - low) * 0.3, -0.5e6, 0.5e6);
		shift_ += move;
		sweeping_ = false;
		has_target_ = true;
		target_ = low;
		expected_ = double(std::count_if(s.begin(), s.end(), [](double x) { return x >= 0; })) / double(s.size());
		slack_steps_++;
		if (debug_phase())
			LOGI("phase: streamed, slack p1 %.2f min %.2f p50 %.2f ms, shift %+.2f (moved %+.2f)", low / 1e6,
			     least / 1e6, s[s.size() / 2] / 1e6, shift_ / 1e6, move / 1e6);
		since_ = now_ns() + 100000000ull;
		return result();
	}
	slack_.clear();
	if (sweeping_) {
		double display = d[d.size() / 2];
		sweep_.push_back({shift_, display, drops * 30 < int(d.size())});
		if (double(sweep_.size()) * sweep_step >= t) {
			const Sweep *best = nullptr;
			for (auto &s : sweep_)
				if (s.safe && (!best || s.display < best->display))
					best = &s;
			if (!best)
				for (auto &s : sweep_)
					if (!best || s.display < best->display)
						best = &s;
			shift_ = best->shift + margin_;
			sweeping_ = false;
			LOGI("phase: swept %zu steps, start at shift %+.1f ms", sweep_.size(), shift_ / 1e6);
		} else {
			shift_ -= sweep_step;
		}
	} else if (all >= 10 && late * 5 > all * 4) {
		drain_ = true;
		recent_.clear();
	} else if (!step.empty()) {
		auto around = [&](double x, double m) {
			double y = std::fmod(x - m, t);
			if (y > t / 2)
				y -= t;
			else if (y <= -t / 2)
				y += t;
			return m + y;
		};
		std::vector<double> host;
		for (auto &s : step)
			if (s.has_late)
				host.push_back(std::fmod(s.a + s.late, t));
		for (auto &s : step)
			if (s.has_late)
				recent_.push_back(s.late);
		if (recent_.size() > 1200)
			recent_.erase(recent_.begin(), recent_.begin() + long(recent_.size() - 1200));
		double med = median(recent_);
		int bins = int(std::ceil(3 * t / bin_ns));
		auto bin = [&](double u) { return std::max(0, std::min(bins - 1, int((u + t) / bin_ns))); };
		if (int(bin_all_.size()) != bins) {
			bin_all_.assign(size_t(bins), 0);
			bin_dropped_.assign(size_t(bins), 0);
		}
		for (int i = 0; i < bins; i++) {
			bin_all_[size_t(i)] *= 0.98;
			bin_dropped_[size_t(i)] *= 0.98;
		}
		for (auto &f : step) {
			if (!f.has_late)
				continue;
			double l = f.late;
			int i = bin(fmod_pos(f.a + l - med, t) - (l - med));
			bin_all_[size_t(i)] += 1;
			if (!f.ok)
				bin_dropped_[size_t(i)] += 1;
		}
		int reach = int(std::round(1e6 / bin_ns));
		std::vector<double> share(static_cast<size_t>(bins));
		for (int i = 0; i < bins; i++) {
			bool near = false;
			for (int j = std::max(0, i - reach); j <= std::min(bins - 1, i + reach); j++)
				if (bin_all_[size_t(j)] >= 1)
					near = true;
			double prior = near ? enough : 0.5;
			share[size_t(i)] = (bin_all_[size_t(i)] - bin_dropped_[size_t(i)] + 2 * prior) / (bin_all_[size_t(i)] + 2);
		}
		if (host.size() * 2 > step.size() && recent_.size() >= 60) {
			std::vector<double> hs;
			for (double x : host)
				hs.push_back(around(x, host[0]));
			double h = median(hs);
			h -= std::floor(h / t) * t;
			int cands = int(std::ceil(t / bin_ns));
			std::vector<double> scores(static_cast<size_t>(cands));
			for (int c = 0; c < cands; c++) {
				double lead = c * bin_ns, s = 0;
				for (double r : recent_)
					s += share[size_t(bin(lead - (r - med)))];
				scores[size_t(c)] = s;
			}
			int c = -1;
			for (int i = 0; i < cands; i++)
				if (scores[size_t(i)] >= enough * double(recent_.size())) {
					c = i;
					break;
				}
			if (c < 0)
				c = int(std::max_element(scores.begin(), scores.end()) - scores.begin());
			double best = fmod_pos(c * bin_ns + med, t);
			target_ = best;
			has_target_ = true;
			expected_ = scores[size_t(c)] / double(recent_.size());
			double move = best - h;
			move -= std::round(move / t) * t;
			shift_ += move / 2;
			if (debug_phase()) {
				std::vector<double> r = recent_;
				std::sort(r.begin(), r.end());
				LOGI("phase: shift %+.2f host lead %.2f target %.2f (%.2f%% expected) med %.2f late p10 %.2f p90 %.2f p99 %.2f n %zu",
				     shift_ / 1e6, h / 1e6, best / 1e6, expected_ * 100, med / 1e6, r[r.size() / 10] / 1e6,
				     r[r.size() * 9 / 10] / 1e6, r[r.size() * 99 / 100] / 1e6, r.size());
			}
		}
	}
	since_ = now_ns() + 100000000ull;
	return result();
}

bool PhaseLock::take_drain()
{
	std::lock_guard<std::mutex> g(lock_);
	bool d = drain_;
	drain_ = false;
	return d;
}

std::string PhaseLock::summary()
{
	std::lock_guard<std::mutex> g(lock_);
	if (!last_)
		return "phase: waiting for the display";
	char b[200];
	if (sweeping_)
		snprintf(b, sizeof(b), "phase: sweeping %zu/%d, shift %+.1f ms, refresh %.3f ms", sweep_.size(),
		         int(std::ceil(interval() / sweep_step)), shift_ / 1e6, interval() / 1e6);
	else if (slack_steps_ > 0)
		snprintf(b, sizeof(b), "phase: streamed, shift %+.1f ms, refresh %.3f ms, slack p1 %.2f ms, %.1f%% untorn",
		         shift_ / 1e6, interval() / 1e6, target_ / 1e6, expected_ * 100);
	else if (has_target_)
		snprintf(b, sizeof(b), "phase: locked, shift %+.1f ms, refresh %.3f ms, host lead %.2f ms, %.0f%% shown expected",
		         shift_ / 1e6, interval() / 1e6, target_ / 1e6, expected_ * 100);
	else
		snprintf(b, sizeof(b), "phase: locked, shift %+.1f ms, refresh %.3f ms", shift_ / 1e6, interval() / 1e6);
	return b;
}

} // namespace gm
