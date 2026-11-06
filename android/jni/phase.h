// Phase lock: the host's frames timed to this display's refreshes. A port of
// apple/Phase.swift (vsync-on mode only; Android has no vsync-off path). See
// that file for the reasoning; the numbers are the same.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "timing.h"

namespace gm {

class PhaseLock {
public:
	explicit PhaseLock(double refresh_hz) : nominal_(1e9 / refresh_hz) {}
	void restart(double refresh_hz = 0);
	void presented(const FrameTiming &t);
	void dropped(const FrameTiming &t);
	// Every half second: the grid to send (a recent refresh, ours, shifted; and
	// the interval), false until a frame has been shown.
	bool step(uint64_t &anchor, uint64_t &interval);
	bool take_drain();
	std::string summary();

private:
	double lead(uint64_t submitted) const;
	double interval() const { return period_ > 0 ? period_ : nominal_; }
	void measure();
	void start_sweep();
	static bool late(const FrameTiming &t, double &out);

	std::mutex lock_;
	double nominal_;
	uint64_t last_ = 0, previous_ = 0;
	double period_ = 0;
	std::vector<double> gaps_;
	std::vector<double> samples_;
	struct Lead { double a; bool ok; bool has_late; double late; };
	std::vector<Lead> leads_;
	int late_shown_ = 0, shown_ = 0;
	bool drain_ = false;
	uint64_t since_ = 0;
	double shift_ = 0;
	struct Sweep { double shift, display; bool safe; };
	std::vector<Sweep> sweep_;
	bool sweeping_ = true;
	int steps_ = 0;
	bool has_target_ = false;
	double target_ = 0, expected_ = 0;
	std::vector<double> bin_all_, bin_dropped_;
	std::vector<double> recent_;
	double margin_ = 3e6;
	int drops_ = 0;
};

} // namespace gm
