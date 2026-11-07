// Inter frames: the side information's bitstream, shared by the encoders and
// decoders (Vulkan and Metal). No graphics API dependency. Specified in
// bitstream/inter.md.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace Riptide
{
static constexpr uint32_t BITSTREAM_EXTENDED_CODE_INTER = 1;

// Same first word as BitstreamHeader (payload_words, sequence and extended in
// the same bits), and `code` where BitstreamSequenceHeader has it.
struct BitstreamInterHeader
{
	uint32_t reserved0 : 16;
	uint32_t payload_words : 12; // including this header
	uint32_t sequence : 3;
	uint32_t extended : 1; // 1
	uint32_t first_tile : 12;
	uint32_t tile_count : 12;
	uint32_t code : 2; // BITSTREAM_EXTENDED_CODE_INTER
	uint32_t reserved1 : 6;
};
static_assert(sizeof(BitstreamInterHeader) == 8, "BitstreamInterHeader is not 8 bytes.");

// The frame's grid: one vector per 16x16 luma block, one weight (map bit) per
// 32x32, a tile is 4x4 vectors (2x2 weights, 64x64 pixels).
struct InterGeometry
{
	int vw, vh; // vectors
	int mw, mh; // weight blocks
	int tw, th; // tiles
	InterGeometry() = default;
	InterGeometry(int width, int height);
	int tiles() const { return tw * th; }
};

// A tile's record in bits: one map bit per weight block inside the frame,
// then per component (x, y) a 4-bit width w and each of the tile's vectors'
// zigzag delta from its previous vector in w bits. The same rule as
// shaders/inter_side_cost.comp.
uint32_t inter_tile_bits(const InterGeometry &g, int tile, const int16_t *mv, const int16_t *mv_prev);

// The side information's share of a frame's target at most, in bits: past it,
// shaders/inter_side_cost.comp sends the costliest tiles as last frame's
// vectors. RT_SIDE_CAP (a fraction of the target) is for measuring.
uint32_t inter_side_cap_bits(size_t target_bytes);

// How the records are split into packets: greedily in tile order, each packet
// as many whole records as fit in `packet_boundary` bytes (header and padding
// to a word included). Returns the total bytes, and the packets' first tiles
// if asked. The same rule as shaders/inter_side_cost.comp.
size_t inter_layout(const InterGeometry &g, const uint32_t *tile_bits, size_t packet_boundary,
                    std::vector<int> *first_tiles = nullptr);

// Encoder: writes the frame's inter packets back to back into `out`, and each
// packet's size into `sizes`. map: one byte per weight block (nonzero: predict);
// mv, mv_prev: two int16 per vector, quarter-pel luma.
size_t inter_packetize(const InterGeometry &g, const uint8_t *map, const int16_t *mv, const int16_t *mv_prev,
                       uint32_t sequence, size_t packet_boundary,
                       std::vector<uint8_t> &out, std::vector<size_t> &sizes);

// Decoder side: the received packets of one frame, applied to the previous
// vectors. A tile none of whose records arrived keeps its previous vectors and
// predicts all its weight blocks (the rule a loss report's replay relies on).
class InterParser
{
public:
	void init(const InterGeometry &g);
	void clear(); // a new frame
	// One inter packet (header included); false if malformed.
	bool push(const void *data, size_t size);
	// map: one byte per weight block, 0 or 255; mv: the frame's vectors, from mv_prev.
	void resolve(const int16_t *mv_prev, uint8_t *map, int16_t *mv) const;
	int tiles_received() const { return received_count; }
	const InterGeometry &geometry() const { return g; }

private:
	InterGeometry g;
	std::vector<std::vector<uint32_t>> packets; // this frame's, as received
	std::vector<uint8_t> received; // per tile
	int received_count = 0;
};
}
