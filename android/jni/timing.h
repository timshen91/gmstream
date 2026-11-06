// Per-frame timestamps through every stage (apple/Video.swift, FrameTiming),
// all on this device's CLOCK_MONOTONIC except where noted.

#pragma once

#include <cstdint>

namespace gm {

struct FrameTiming {
	uint64_t encode_ns = 0;      // host: present -> bitstream ready
	uint64_t present_host = 0;   // gamescope presented it (host clock)
	bool local = false;          // the two below are known (clock offset)
	uint64_t present_local = 0;  // the same, ours
	uint64_t encoded_local = 0;  // bitstream ready, ours
	uint64_t received = 0;       // last datagram arrived (kernel)
	uint64_t first_received = 0; // first datagram arrived
	uint64_t overlap_ns = 0;     // sent before the encode finished
	uint64_t decode_start = 0;   // handed to the decoder
	uint64_t decoded = 0;        // the decoder gave it back
	uint64_t submitted = 0;      // handed to the display (released to the surface)
	uint64_t latched = 0;        // SurfaceFlinger took it
	uint64_t presented = 0;      // on screen
};

} // namespace gm
