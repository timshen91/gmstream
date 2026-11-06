// Per-stage latency over the last 5 s (apple/Latency.swift, StageStats), and
// the stream's throughput and gaps (apple/Stats.swift).

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "timing.h"

namespace gm {

class StageStats {
public:
	static const std::vector<std::string> &names();
	void add(const FrameTiming &t);
	void drop();
	struct Row { std::string name; bool has; double p50, p99, max; };
	struct Taken { std::vector<Row> rows; int shown = 0, dropped = 0; };
	Taken take();

private:
	std::mutex lock_;
	struct Entry { uint64_t at; std::vector<double> v; std::vector<bool> has; };
	std::vector<Entry> frames_;
	int shown_ = 0, dropped_ = 0;
	uint64_t last_presented_ = 0, last_present_ = 0;
};

class Throughput {
public:
	void bytes(size_t n) { std::lock_guard<std::mutex> g(lock_); bytes_ += n; }
	void gap() { std::lock_guard<std::mutex> g(lock_); gaps_++; }
	// Mb/s and gaps since the last call.
	void take(double &mbps, int &gaps);

private:
	std::mutex lock_;
	uint64_t bytes_ = 0, since_ = 0;
	int gaps_ = 0;
};

// Whether to decode a frame: in order, or from a keyframe after a gap
// (apple/Stats.swift, FrameGate).
class FrameGate {
public:
	bool admit(uint32_t id, bool key, bool &request_keyframe, Throughput &stats);
	void reset() { has_expect_ = false; }

private:
	bool has_expect_ = false;
	uint32_t expect_ = 0;
	uint64_t last_request_ = 0;
};

} // namespace gm
