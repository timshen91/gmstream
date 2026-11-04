// Copyright (c) 2025 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include "riptide_config.hpp"

namespace Vulkan
{
class Device;
class ImageView;
class CommandBuffer;
}

namespace Riptide
{
class Decoder
{
public:
	Decoder();
	~Decoder();

	// Fragment path is optimized for typical mobile GPUs which have weak compute support.
	// iDWT is instead computed entirely in traditional render passes and fragment shaders.
	// This path is *not* recommended for desktop-class chips.
	bool init(Vulkan::Device *device, int width, int height,
	          ChromaSubsampling chroma, bool fragment_path = false);

	static bool device_prefers_fragment_path(Vulkan::Device &device);

	void clear();
	bool push_packet(const void *data, size_t size);

	// If fragment path is enabled, the command buffer must support graphics operations.
	// To synchronize, synchronize with COLOR_OUTPUT / COLOR_ATTACHMENT_WRITE / COLOR_ATTACHMENT_OPTIMAL.
	// Views must be created with VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT.
	bool decode(Vulkan::CommandBuffer &cmd, const ViewBuffers &views);

	// Inter frames (bitstream/inter.md): the planes the next decodes predict from,
	// the previous decoded frame (nullptr: intra). Decode every frame of an inter
	// sequence in order, lost packets or not (clear() before a frame none of whose
	// packets arrived). The motion state is the vectors the next frame's are
	// coded against.
	// As Encoder::set_reference: the reference, and this image's first row in it.
	void set_reference(const ViewBuffers *reference, int ref_y = 0);
	// Stripes: w x h texels of one plane from row src_y of src to row dst_y of dst.
	void copy_rows(Vulkan::CommandBuffer &cmd, const Vulkan::ImageView &src, const Vulkan::ImageView &dst,
	               int w, int h, int src_y, int dst_y);
	bool get_motion_state(int16_t *vectors, size_t count) const;
	bool set_motion_state(const int16_t *vectors, size_t count);

	bool decode_is_ready(bool allow_partial_frame) const;

	// A more refined version of decode_is_ready() that allows a bit more control.
	// The default is num_pristine_bands = 2 (the next-to-final LL band) and minimum_packet_ratio = 0.9.
	// active_block_mask is an optional pointer.
	// If bit i % 32 of active_block_mask[i / 32] is not set, then a missing block for that block index
	// is ignored. This is intended to cover advanced cases for error correction.
	// If the pointer is null, it is implied that all blocks are active for purposes of this call.
	bool decode_is_ready(bool allow_partial_frame, int num_pristine_bands, float minimum_packet_ratio,
	                     const uint32_t *active_block_mask, size_t word_count) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
}