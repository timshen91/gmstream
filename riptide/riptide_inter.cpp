// Inter frames: the side information's bitstream (bitstream/inter.md).

#include "riptide_inter.hpp"
#include <algorithm>
#include <stdlib.h>
#include <string.h>

namespace Riptide
{
InterGeometry::InterGeometry(int width, int height)
{
	vw = (width + 15) / 16;
	vh = (height + 15) / 16;
	mw = (width + 31) / 32;
	mh = (height + 31) / 32;
	tw = (vw + 3) / 4;
	th = (vh + 3) / 4;
}

uint32_t inter_side_cap_bits(size_t target_bytes)
{
	static const double share = getenv("RT_SIDE_CAP") ? atof(getenv("RT_SIDE_CAP")) : 0.5;
	if (target_bytes == 0 || share <= 0.0)
		return UINT32_MAX;
	double bits = double(target_bytes) * 8.0 * share;
	return bits >= double(UINT32_MAX) ? UINT32_MAX : uint32_t(bits);
}

static uint32_t zigzag(int d)
{
	return d >= 0 ? uint32_t(2 * d) : uint32_t(-2 * d - 1);
}

static int width_of(uint32_t v)
{
	int b = 0;
	while (v >> b)
		b++;
	return b;
}

// The tile's map bits, and per component its width.
static void tile_shape(const InterGeometry &g, int tile, const int16_t *mv, const int16_t *mv_prev,
                       int &map_bits, int &n, int width[2])
{
	int tx = tile % g.tw, ty = tile / g.tw;
	map_bits = std::min(2, g.mw - 2 * tx) * std::min(2, g.mh - 2 * ty);
	uint32_t mx[2] = {};
	n = 0;
	for (int y = 4 * ty; y < std::min(4 * ty + 4, g.vh); y++)
		for (int x = 4 * tx; x < std::min(4 * tx + 4, g.vw); x++, n++)
			for (int c = 0; c < 2; c++)
			{
				size_t i = 2 * (size_t(y) * g.vw + x) + c;
				mx[c] = std::max(mx[c], zigzag(mv[i] - mv_prev[i]));
			}
	width[0] = width_of(mx[0]);
	width[1] = width_of(mx[1]);
}

uint32_t inter_tile_bits(const InterGeometry &g, int tile, const int16_t *mv, const int16_t *mv_prev)
{
	int map_bits, n, width[2];
	tile_shape(g, tile, mv, mv_prev, map_bits, n, width);
	return uint32_t(map_bits + 8 + n * (width[0] + width[1]));
}

static size_t packet_bytes(uint32_t bits)
{
	return sizeof(BitstreamInterHeader) + 4 * ((bits + 31) / 32);
}

size_t inter_layout(const InterGeometry &g, const uint32_t *tile_bits, size_t packet_boundary,
                    std::vector<int> *first_tiles)
{
	if (first_tiles)
		first_tiles->clear();
	size_t total = 0;
	uint32_t bits = 0;
	bool open = false;
	for (int t = 0; t < g.tiles(); t++)
	{
		if (open && packet_bytes(bits + tile_bits[t]) > packet_boundary)
		{
			total += packet_bytes(bits);
			open = false;
		}
		if (!open)
		{
			if (first_tiles)
				first_tiles->push_back(t);
			bits = 0;
			open = true;
		}
		bits += tile_bits[t];
	}
	if (open)
		total += packet_bytes(bits);
	return total;
}

namespace
{
struct BitWriter
{
	std::vector<uint32_t> words;
	uint32_t bit = 0;
	void put(uint32_t v, int n)
	{
		for (int i = 0; i < n; i++, bit++)
		{
			if (bit / 32 >= words.size())
				words.push_back(0);
			words[bit / 32] |= ((v >> i) & 1u) << (bit % 32);
		}
	}
};

struct BitReader
{
	const uint32_t *words;
	uint32_t count, bit = 0;
	bool get(int n, uint32_t &v)
	{
		v = 0;
		if (bit + uint32_t(n) > 32 * count)
			return false;
		for (int i = 0; i < n; i++, bit++)
			v |= ((words[bit / 32] >> (bit % 32)) & 1u) << i;
		return true;
	}
};
}

size_t inter_packetize(const InterGeometry &g, const uint8_t *map, const int16_t *mv, const int16_t *mv_prev,
                       uint32_t sequence, size_t packet_boundary,
                       std::vector<uint8_t> &out, std::vector<size_t> &sizes)
{
	std::vector<uint32_t> bits(g.tiles());
	for (int t = 0; t < g.tiles(); t++)
		bits[t] = inter_tile_bits(g, t, mv, mv_prev);
	std::vector<int> first;
	size_t total = inter_layout(g, bits.data(), packet_boundary, &first);
	out.clear();
	sizes.clear();
	for (size_t p = 0; p < first.size(); p++)
	{
		int t0 = first[p], t1 = p + 1 < first.size() ? first[p + 1] : g.tiles();
		BitWriter w;
		for (int t = t0; t < t1; t++)
		{
			int tx = t % g.tw, ty = t / g.tw;
			for (int y = 2 * ty; y < std::min(2 * ty + 2, g.mh); y++)
				for (int x = 2 * tx; x < std::min(2 * tx + 2, g.mw); x++)
					w.put(map[size_t(y) * g.mw + x] ? 1 : 0, 1);
			int map_bits, n, width[2];
			tile_shape(g, t, mv, mv_prev, map_bits, n, width);
			w.put(uint32_t(width[0]), 4);
			w.put(uint32_t(width[1]), 4);
			for (int c = 0; c < 2; c++)
				for (int y = 4 * ty; y < std::min(4 * ty + 4, g.vh); y++)
					for (int x = 4 * tx; x < std::min(4 * tx + 4, g.vw); x++)
					{
						size_t i = 2 * (size_t(y) * g.vw + x) + c;
						w.put(zigzag(mv[i] - mv_prev[i]), width[c]);
					}
		}
		BitstreamInterHeader h = {};
		h.payload_words = uint32_t(2 + (w.bit + 31) / 32);
		h.sequence = sequence;
		h.extended = 1;
		h.first_tile = uint32_t(t0);
		h.tile_count = uint32_t(t1 - t0);
		h.code = BITSTREAM_EXTENDED_CODE_INTER;
		size_t at = out.size();
		out.resize(at + 4 * h.payload_words);
		memcpy(out.data() + at, &h, sizeof(h));
		if (!w.words.empty())
			memcpy(out.data() + at + sizeof(h), w.words.data(), 4 * w.words.size());
		sizes.push_back(4 * h.payload_words);
	}
	return total;
}

void InterParser::init(const InterGeometry &g_)
{
	g = g_;
	received.assign(g.tiles(), 0);
	clear();
}

void InterParser::clear()
{
	packets.clear();
	std::fill(received.begin(), received.end(), 0);
	received_count = 0;
}

bool InterParser::push(const void *data, size_t size)
{
	if (size < sizeof(BitstreamInterHeader))
		return false;
	BitstreamInterHeader h;
	memcpy(&h, data, sizeof(h));
	if (h.code != BITSTREAM_EXTENDED_CODE_INTER || size_t(h.payload_words) * 4 > size || h.payload_words < 2 ||
	    h.first_tile + h.tile_count > uint32_t(g.tiles()))
		return false;
	for (uint32_t t = h.first_tile; t < h.first_tile + h.tile_count; t++)
		if (received[t])
			return true; // a duplicate
	std::vector<uint32_t> words(h.payload_words);
	memcpy(words.data(), data, 4 * words.size());
	packets.push_back(std::move(words));
	for (uint32_t t = h.first_tile; t < h.first_tile + h.tile_count; t++)
		received[t] = 1;
	received_count += int(h.tile_count);
	return true;
}

void InterParser::resolve(const int16_t *mv_prev, uint8_t *map, int16_t *mv) const
{
	memcpy(mv, mv_prev, sizeof(int16_t) * 2 * size_t(g.vw) * g.vh);
	memset(map, 255, size_t(g.mw) * g.mh);
	for (auto &p : packets)
	{
		BitstreamInterHeader h;
		memcpy(&h, p.data(), sizeof(h));
		BitReader r = {p.data() + 2, uint32_t(p.size() - 2)};
		for (uint32_t t = h.first_tile; t < h.first_tile + h.tile_count; t++)
		{
			int tx = int(t) % g.tw, ty = int(t) / g.tw;
			uint32_t v;
			for (int y = 2 * ty; y < std::min(2 * ty + 2, g.mh); y++)
				for (int x = 2 * tx; x < std::min(2 * tx + 2, g.mw); x++)
				{
					if (!r.get(1, v))
						return;
					map[size_t(y) * g.mw + x] = v ? 255 : 0;
				}
			uint32_t width[2];
			if (!r.get(4, width[0]) || !r.get(4, width[1]))
				return;
			for (int c = 0; c < 2; c++)
				for (int y = 4 * ty; y < std::min(4 * ty + 4, g.vh); y++)
					for (int x = 4 * tx; x < std::min(4 * tx + 4, g.vw); x++)
					{
						if (!r.get(int(width[c]), v))
							return;
						int d = (v & 1) ? -int((v + 1) >> 1) : int(v >> 1);
						size_t i = 2 * (size_t(y) * g.vw + x) + c;
						mv[i] = int16_t(mv_prev[i] + d);
					}
		}
	}
}
}
