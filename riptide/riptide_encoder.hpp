// Copyright (c) 2025 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "riptide_config.hpp"

namespace Vulkan
{
class Device;
class Buffer;
class ImageView;
class CommandBuffer;
}

namespace Riptide
{
class Encoder
{
public:
	Encoder();
	~Encoder();

	struct BitstreamBuffers
	{
		struct
		{
			const Vulkan::Buffer *buffer;
			uint64_t offset;
			uint64_t size;
		} meta, bitstream;
		size_t target_size;
	};

	bool init(Vulkan::Device *device, int width, int height, ChromaSubsampling chroma);
	bool encode(Vulkan::CommandBuffer &cmd, const ViewBuffers &views, const BitstreamBuffers &buffers);

	// Inter frames (bitstream/inter.md). With a reference (the previous decoded
	// frame's planes; nullptr: intra) an encode predicts from it and codes the
	// difference. The encode is two parts, which may be separate submissions:
	// the front decides the frame's map and vectors (read back when its GPU work
	// completes, for build_inter_packets()), the back codes the rest. encode()
	// does both.
	// The previous decoded frame to predict from (nullptr: intra). This image may
	// be a stripe of that frame, from its row ref_y (a multiple of 64): it is
	// coded as an image of its own, but predicts from the whole reference.
	void set_reference(const ViewBuffers *reference, int ref_y = 0);
	// Stripes: w x h texels of one plane from row src_y of src to row dst_y of dst.
	void copy_rows(Vulkan::CommandBuffer &cmd, const Vulkan::ImageView &src, const Vulkan::ImageView &dst,
	               int w, int h, int src_y, int dst_y);
	void set_inter_packet_boundary(size_t packet_boundary);

	// Rate control across images (a frame's stripes, each an Encoder): each
	// encode copies its rate statistics into buffer (RateStatsSize bytes, the
	// buffer needs TRANSFER_DST), as u32: [1] the payload at the finest
	// quantizer, words; [16 + i] the words that operation bucket i of
	// RateStatBuckets saves, in the order the rate control applies them.
	static constexpr size_t RateStatBuckets = 128 * 16;
	static constexpr size_t RateStatsSize = 64 + 4 * RateStatBuckets;
	void set_rate_stats_readback(const Vulkan::Buffer *buffer);
	// The vectors the next frame's are coded against (ceil(w / 16) x ceil(h / 16)
	// quarter-pel pairs), e.g. a decoder's after replaying a client's decode.
	bool set_motion_state(const int16_t *vectors, size_t count);
	bool frame_predicted() const;
	bool encode_front(Vulkan::CommandBuffer &cmd, const ViewBuffers &views);
	bool encode_back(Vulkan::CommandBuffer &cmd, const ViewBuffers &views, const BitstreamBuffers &buffers);
	// After the front's GPU work: the frame's inter packets (none for intra).
	void build_inter_packets(std::vector<uint8_t> &out, std::vector<size_t> &sizes);

	// Debug hackery
	const Vulkan::ImageView &get_wavelet_band(int component, int level);
	bool encode_pre_transformed(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);
	//

	uint64_t get_meta_required_size() const;

	struct Packet
	{
		size_t offset;
		size_t size;
	};

	// Padding size is used to reserve a certain number of bytes in the first packet for application defined scratch
	// space. It only affects the first split point for packets.
	// bitstream should still point to the start of data that is written by encoder,
	// and size is the valid size of bitstream.
	size_t compute_num_packets(const void *mapped_meta, size_t packet_boundary, size_t padding_size = 0) const;
	size_t packetize(Packet *packets, size_t packet_boundary,
					 void *bitstream, size_t size,
					 const void *mapped_meta, const void *mapped_bitstream,
					 size_t padding_size = 0) const;

	// Debug
	void report_stats(const void *mapped_meta, const void *mapped_bitstream) const;

	// For advanced error correction purposes.
	// If bands = 1, blocks for the lowest resolution band is included.
	// For bands = 2, HL/LH/HH for last decomposition is included to reconstruct next resolution LL band, etc.
	// Returns number of blocks which are possibly included in this range.
	// bands = 2 or 3 is a good default if using the more advanced model.
	// bands = 1 will return some redundant information due to the nature of the bitstream.
	size_t get_num_active_blocks(int bands) const;

	// Size of words should be round_up(get_num_active_blocks(bands) / 32), and word_count should be at least this large.
	// The resulting stream can be sent as side-band data to the decoder if desired.
	// For low values of bands, like bands = 1, it is expected that all bits are set, and it's not necessary to send
	// the side-band data in that case.
	void compute_block_active_words(int bands, uint32_t *words, size_t word_count, const void *mapped_meta) const;

	// When using advanced model for error correction, queries the number of network packets which will contain
	// "critically" encoded data. The first number of returned packets will correspond to these "critical" packets.
	// It may be advantageous to add some FEC to these packets only, rather than error correcting the entire bitstream.
	size_t compute_num_critical_packets(int bands, const void *mapped_meta, size_t packet_boundary, size_t padding_size = 0) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
}