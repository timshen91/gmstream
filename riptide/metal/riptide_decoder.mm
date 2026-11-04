// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT

// Metal backend for the Riptide decoder. Mirrors the compute path of
// riptide_decoder.cpp; the fragment iDWT path is not ported. The device object
// and the wavelet pyramid it shares with the encoder live in
// riptide_common.mm.

#include "riptide_common.hpp"

#include <memory>
#include <string.h>
#include <vector>

using namespace Riptide;

namespace
{
// Push constant layouts. These must match the Registers structs SPIRV-Cross
// emitted into shaders/metal/*.metal. MSL gives int2 an 8 byte alignment, so
// the dequant struct is padded out to 24 bytes even though only 20 are used.
struct DequantPush
{
	int32_t resolution[2];
	int32_t output_layer;
	int32_t block_offset_32x32;
	int32_t block_stride_32x32;
	int32_t padding;
};
static_assert(sizeof(DequantPush) == 24, "DequantPush layout mismatch.");

struct IdwtPush
{
	int32_t resolution[2];
	float inv_resolution[2];
	int32_t output_offset[2]; // where an output plane is written (a stripe into its frame's rows)
};
static_assert(sizeof(IdwtPush) == 24, "IdwtPush layout mismatch.");

// How many decodes a caller may have in flight before acquiring a slot waits for the
// oldest. Two would do for the intended display loop; four is headroom at ~1 MB per
// slot at 4K.
constexpr size_t UploadSlotCount = 4;

struct UploadSlot
{
	id<MTLBuffer> offsets;
	id<MTLBuffer> payload;
	// Inter frames: the frame's weights and vectors, written by the CPU, so per
	// slot like the buffers.
	id<MTLTexture> weights, vectors;
	id<MTLCommandBuffer> consumer;

	void reclaim()
	{
		if (!consumer)
			return;
		// Returns immediately unless the caller really is UploadSlotCount ahead.
		[consumer waitUntilCompleted];
		consumer = nil;
	}

	// No destructor: ARC releases both buffers, and Metal keeps anything a command
	// buffer still references alive on its own.
};
}

struct riptide_decoder_opaque
{
	riptide_device device = nullptr;

	BlockLayout layout;
	BitstreamParser parser;
	WaveletPyramid wavelet;

	// Fixed size on purpose: a vector that appended whenever no slot was free grew
	// without bound when a caller submitted faster than the GPU drained.
	UploadSlot upload_slots[UploadSlotCount];
	size_t next_upload_slot = 0;

	// Inter frames: the reference planes predicted from (nil: intra), the
	// prediction they give, the vectors the next frame's are coded against, and
	// this frame's map and vectors as resolved from its inter packets.
	id<MTLTexture> reference[3] = {};
	int ref_y = 0; // this image's first row in the reference (a stripe of the frame)
	id<MTLTexture> prediction[3] = {};
	std::vector<int16_t> mv_prev, mv;
	std::vector<uint8_t> map;
	// The prediction is already on its way (riptide_decoder_prepare_prediction).
	bool prediction_prepared = false;
};

namespace
{
// Overallocates so that a steadily sized stream stops reallocating.
bool ensure_buffer(riptide_device device, __strong id<MTLBuffer> *buffer, size_t size)
{
	if (*buffer && (*buffer).length >= size)
		return true;

	size_t allocate = size * 2;
	if (allocate < 64 * 1024)
		allocate = 64 * 1024;

	*buffer = [device->mtl newBufferWithLength:allocate options:MTLResourceStorageModeShared];
	if (!*buffer)
	{
		device->log("Failed to allocate a %zu byte upload buffer.", allocate);
		return false;
	}

	return true;
}

UploadSlot *acquire_upload_slot(riptide_decoder decoder, size_t offsets_size, size_t payload_size)
{
	UploadSlot *slot = &decoder->upload_slots[decoder->next_upload_slot];
	decoder->next_upload_slot = (decoder->next_upload_slot + 1) % UploadSlotCount;

	// Applies back pressure rather than allocating another slot.
	slot->reclaim();

	if (!ensure_buffer(decoder->device, &slot->offsets, offsets_size) ||
	    !ensure_buffer(decoder->device, &slot->payload, payload_size))
		return nullptr;

	return slot;
}

void encode_dequant(riptide_decoder decoder, id<MTLComputeCommandEncoder> enc, UploadSlot *slot)
{
	auto &layout = decoder->layout;

	[enc setComputePipelineState:decoder->device->dequant_pipeline];
	// The u8/u16/u32 aliases of the payload collapse into a single binding in MSL.
	[enc setBuffer:slot->payload offset:0 atIndex:0];
	[enc setBuffer:slot->offsets offset:0 atIndex:2];

	for (int level = 0; level < DecompositionLevels; level++)
	{
		for (int component = 0; component < NumComponents; component++)
		{
			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0 && layout.chroma == ChromaSubsampling::Chroma420)
				continue;

			[enc setTexture:decoder->wavelet.component_layer_views[component][level] atIndex:0];

			for (int band = (level == DecompositionLevels - 1 ? 0 : 1); band < 4; band++)
			{
				DequantPush push = {};
				push.resolution[0] = layout.level_width(level);
				push.resolution[1] = layout.level_height(level);
				push.output_layer = band;
				push.block_offset_32x32 = layout.block_meta[component][level][band].block_offset_32x32;
				push.block_stride_32x32 = layout.block_meta[component][level][band].block_stride_32x32;
				[enc setBytes:&push length:sizeof(push) atIndex:1];

				[enc dispatchThreadgroups:MTLSizeMake((push.resolution[0] + 31) / 32,
				                                     (push.resolution[1] + 31) / 32, 1)
				      threadsPerThreadgroup:MTLSizeMake(DequantThreadgroupSize, 1, 1)];
			}
		}
	}
}

void encode_idwt_dispatch(riptide_decoder decoder, id<MTLComputeCommandEncoder> enc,
                          const IdwtPush &push, id<MTLTexture> input, id<MTLTexture> output,
                          bool dc_shift, id<MTLTexture> reference = nil)
{
	[enc setComputePipelineState:decoder->device->idwt_pipeline[reference ? 2 : dc_shift ? 1 : 0]];
	[enc setBytes:&push length:sizeof(push) atIndex:0];
	[enc setTexture:input atIndex:0];
	[enc setTexture:output atIndex:1];
	if (reference)
	{
		[enc setTexture:reference atIndex:2];
		[enc setSamplerState:decoder->device->mirror_repeat_sampler atIndex:2];
	}
	[enc setSamplerState:decoder->device->mirror_repeat_sampler atIndex:0];
	[enc dispatchThreadgroups:MTLSizeMake((push.resolution[0] + 15) / 16,
	                                     (push.resolution[1] + 15) / 16, 1)
	      threadsPerThreadgroup:MTLSizeMake(IdwtThreadgroupSize, 1, 1)];
}

void encode_idwt(riptide_decoder decoder, id<MTLComputeCommandEncoder> enc,
                 id<MTLTexture> const planes[3], int output_y)
{
	auto &layout = decoder->layout;
	const bool chroma_420 = layout.chroma == ChromaSubsampling::Chroma420;

	for (int input_level = DecompositionLevels - 1; input_level >= 0; input_level--)
	{
		// Levels are a dependent chain: this one reads the LL band the previous
		// one produced. Within a level the three components are independent, so
		// the encoder runs concurrently and only the level boundaries barrier.
		if (input_level != DecompositionLevels - 1)
			[enc memoryBarrierWithScope:MTLBarrierScopeTextures];

		IdwtPush push = {};
		// The shader transposes on load, so resolution is swapped here.
		push.resolution[0] = layout.level_height(input_level);
		push.resolution[1] = layout.level_width(input_level);
		push.inv_resolution[0] = 1.0f / float(push.resolution[0]);
		push.inv_resolution[1] = 1.0f / float(push.resolution[1]);

		if (input_level == 0)
		{
			// Final level writes the output planes directly. Under 420 the chroma
			// planes were already finished one level earlier.
			const int components = chroma_420 ? 1 : NumComponents;
			push.output_offset[1] = output_y;
			for (int c = 0; c < components; c++)
			{
				encode_idwt_dispatch(decoder, enc, push,
				                     decoder->wavelet.component_layer_views[c][input_level],
				                     planes[c], true, decoder->reference[0] ? decoder->prediction[c] : nil);
			}
		}
		else
		{
			for (int c = 0; c < NumComponents; c++)
			{
				const bool final_chroma = chroma_420 && c != 0 && input_level == 1;
				id<MTLTexture> output = final_chroma ?
				                       planes[c] :
				                       decoder->wavelet.component_ll_views[c][input_level - 1];
				push.output_offset[1] = final_chroma ? output_y / 2 : 0;

				encode_idwt_dispatch(decoder, enc, push,
				                     decoder->wavelet.component_layer_views[c][input_level],
				                     output, final_chroma,
				                     final_chroma && decoder->reference[0] ? decoder->prediction[c] : nil);
			}
		}
	}
}

bool validate_plane(riptide_device device, id<MTLTexture> texture, int index, int width, int height)
{
	if (!texture)
	{
		device->log("Output plane %d is NULL.", index);
		return false;
	}

	if (texture.textureType != MTLTextureType2D)
	{
		device->log("Output plane %d must be MTLTextureType2D.", index);
		return false;
	}

	if (int(texture.width) != width || int(texture.height) != height)
	{
		device->log("Output plane %d is %ux%u, expected %dx%d.",
		            index, unsigned(texture.width), unsigned(texture.height), width, height);
		return false;
	}

	if ((texture.usage & MTLTextureUsageShaderWrite) == 0)
	{
		device->log("Output plane %d was not created with MTLTextureUsageShaderWrite.", index);
		return false;
	}

	return true;
}
}

//////
// Public API

riptide_result riptide_decoder_create(const riptide_decoder_create_info *info, riptide_decoder *decoder)
{
	if (!info || !decoder || !info->device)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (info->chroma != RIPTIDE_CHROMA_SUBSAMPLING_420 &&
	    info->chroma != RIPTIDE_CHROMA_SUBSAMPLING_444)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	const bool chroma_420 = info->chroma == RIPTIDE_CHROMA_SUBSAMPLING_420;
	if (chroma_420 && ((info->width & 1) != 0 || (info->height & 1) != 0))
	{
		info->device->log("420 subsampling requires even dimensions, got %dx%d.",
		                  info->width, info->height);
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	auto created = std::unique_ptr<riptide_decoder_opaque>(new (std::nothrow) riptide_decoder_opaque);
	if (!created)
		return RIPTIDE_ERROR_OUT_OF_HOST_MEMORY;

	created->device = info->device;

	if (!created->layout.init(info->width, info->height,
	                          chroma_420 ? ChromaSubsampling::Chroma420 : ChromaSubsampling::Chroma444))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	created->parser.init(&created->layout);
	{
		InterGeometry g(created->layout.width, created->layout.height);
		created->mv_prev.assign(2 * size_t(g.vw) * g.vh, 0);
		created->mv.assign(created->mv_prev.size(), 0);
		created->map.assign(size_t(g.mw) * g.mh, 255);
	}

	if (!created->wavelet.init(created->device, created->layout))
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	*decoder = created.release();
	return RIPTIDE_SUCCESS;
}

void riptide_decoder_destroy(riptide_decoder decoder)
{
	delete decoder;
}

void riptide_decoder_clear(riptide_decoder decoder)
{
	if (decoder)
		decoder->parser.clear();
}

riptide_result riptide_decoder_push_packet(riptide_decoder decoder, const void *data, size_t size)
{
	if (!decoder || (!data && size != 0))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (!decoder->parser.push_packet(data, size))
		return RIPTIDE_ERROR_CORRUPT_BITSTREAM;

	return RIPTIDE_SUCCESS;
}

bool riptide_decoder_decode_is_ready(riptide_decoder decoder, bool allow_partial_frame)
{
	return decoder && decoder->parser.decode_is_ready(allow_partial_frame);
}

bool riptide_decoder_decode_is_ready_with_sideband(riptide_decoder decoder, bool allow_partial_frame,
                                                    int num_pristine_bands, float minimum_packet_ratio,
                                                    const uint32_t *active_block_mask, size_t word_count)
{
	// num_pristine_bands is range checked only by an assert in has_pristine_bands(),
	// matching the Vulkan C API. It indexes block_meta[..][DecompositionLevels - band][..],
	// so a large enough value walks off the front of the array with NDEBUG.
	if (!decoder)
		return false;

	return decoder->parser.decode_is_ready(allow_partial_frame, num_pristine_bands, minimum_packet_ratio,
	                                       active_block_mask, word_count);
}

riptide_result riptide_decoder_get_motion_state(riptide_decoder decoder, int16_t *vectors, size_t count)
{
	if (!decoder || !vectors || count != decoder->mv_prev.size() / 2)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	memcpy(vectors, decoder->mv_prev.data(), decoder->mv_prev.size() * sizeof(int16_t));
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_decoder_set_motion_state(riptide_decoder decoder, const int16_t *vectors, size_t count)
{
	if (!decoder || !vectors || count != decoder->mv_prev.size() / 2)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	memcpy(decoder->mv_prev.data(), vectors, decoder->mv_prev.size() * sizeof(int16_t));
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_decoder_set_reference(riptide_decoder decoder, const riptide_gpu_buffers *reference)
{
	return riptide_decoder_set_reference_stripe(decoder, reference, 0);
}

riptide_result riptide_decoder_set_reference_stripe(riptide_decoder decoder, const riptide_gpu_buffers *reference,
                                                      int ref_y)
{
	if (!decoder || ref_y < 0 || (ref_y & 1))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	decoder->ref_y = ref_y;
	for (int i = 0; i < 3; i++)
		decoder->reference[i] = reference ? (__bridge id<MTLTexture>)(reference->planes[i]) : nil;
	return RIPTIDE_SUCCESS;
}

// Inter frames: the frame's map and vectors from its inter packets (tiles whose
// packets were lost keep last frame's vectors and predict, bitstream/inter.md),
// and the prediction from them.
static void encode_inter_prediction(riptide_decoder decoder, id<MTLComputeCommandEncoder> enc, UploadSlot *slot)
{
	auto &layout = decoder->layout;
	decoder->parser.inter().resolve(decoder->mv_prev.data(), decoder->map.data(), decoder->mv.data());
	InterGeometry g(layout.width, layout.height);
	if (!slot->weights)
	{
		slot->weights = create_weight_texture(decoder->device, layout.width, layout.height);
		slot->vectors = create_vector_texture(decoder->device, layout.width, layout.height);
	}
	[slot->weights replaceRegion:MTLRegionMake2D(0, 0, g.mw, g.mh) mipmapLevel:0
	                   withBytes:decoder->map.data() bytesPerRow:g.mw];
	[slot->vectors replaceRegion:MTLRegionMake2D(0, 0, g.vw, g.vh) mipmapLevel:0
	                   withBytes:decoder->mv.data() bytesPerRow:g.vw * 4];
	decoder->mv_prev.swap(decoder->mv);
	dispatch_inter_predict(decoder->device, enc, decoder->reference, decoder->ref_y, layout.width, layout.height,
	                       slot->weights, slot->vectors, decoder->prediction);
}

riptide_result riptide_decoder_prepare_prediction(riptide_decoder decoder,
                                                    riptide_mtl_command_buffer command_buffer)
{
	if (!decoder || !command_buffer || !decoder->reference[0] || decoder->prediction_prepared)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	auto *slot = acquire_upload_slot(decoder, 16, 16);
	if (!slot)
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
	auto *cmd = (__bridge id<MTLCommandBuffer>)(command_buffer);
	auto batch = decoder->device->timestamps->begin_batch();
	auto enc = begin_compute_pass(cmd, MTLDispatchTypeConcurrent, batch.get(), "decode prediction");
	if (!enc)
		return RIPTIDE_ERROR_GENERIC;
	encode_inter_prediction(decoder, enc, slot);
	[enc endEncoding];
	if (batch)
		batch->submit(cmd);
	slot->consumer = cmd;
	decoder->prediction_prepared = true;
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_decoder_decode_gpu_buffer(riptide_decoder decoder,
                                                   riptide_mtl_command_buffer command_buffer,
                                                   const riptide_gpu_buffers *buffers)
{
	return riptide_decoder_decode_gpu_buffer_rows(decoder, command_buffer, buffers, -1);
}

riptide_result riptide_decoder_decode_gpu_buffer_rows(riptide_decoder decoder,
                                                        riptide_mtl_command_buffer command_buffer,
                                                        const riptide_gpu_buffers *buffers, int y)
{
	if (!decoder || !command_buffer || !buffers || (y > 0 && (y & 1)))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto &layout = decoder->layout;
	auto *device = decoder->device;

	id<MTLTexture> planes[3];
	const int chroma_width = layout.chroma == ChromaSubsampling::Chroma420 ?
	                         layout.width / 2 : layout.width;
	const int chroma_height = layout.chroma == ChromaSubsampling::Chroma420 ?
	                          layout.height / 2 : layout.height;

	for (int i = 0; i < 3; i++)
	{
		planes[i] = (__bridge id<MTLTexture>)(buffers->planes[i]);
		const int expected_width = i == 0 ? layout.width : chroma_width;
		const int expected_height = i == 0 ? layout.height : chroma_height;
		if (y >= 0)
		{
			// Into rows y on of a frame's planes (y / 2 for 4:2:0 chroma).
			const int plane_y = i != 0 && layout.chroma == ChromaSubsampling::Chroma420 ? y / 2 : y;
			if (planes[i] == nil || int(planes[i].width) != expected_width ||
			    int(planes[i].height) < plane_y + expected_height)
				return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}
		else if (!validate_plane(device, planes[i], i, expected_width, expected_height))
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	const auto &offsets = decoder->parser.dequant_offsets();
	const auto &payload = decoder->parser.payload();

	const size_t offsets_size = offsets.size() * sizeof(uint32_t);
	// The dequant shader can read slightly past the end of the payload, so pad.
	const size_t payload_size = payload.size() * sizeof(uint32_t) + 16;

	auto *slot = acquire_upload_slot(decoder, offsets_size, payload_size);
	if (!slot)
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	if (offsets_size)
		memcpy(slot->offsets.contents, offsets.data(), offsets_size);
	if (!payload.empty())
		memcpy(slot->payload.contents, payload.data(), payload.size() * sizeof(uint32_t));

	auto *cmd = (__bridge id<MTLCommandBuffer>)(command_buffer);

	// Per pass timestamps, null unless collection is on. Timing the command buffer
	// as a whole would be meaningless: it is the caller's and may carry other work.
	auto batch = device->timestamps->begin_batch();

	// Every dequant dispatch writes a distinct (component, level, band) region of
	// the pyramid and none reads another's output, so they can all run at once. A
	// serial encoder would barrier between all ~42, but the cost is underutilization
	// rather than barrier latency: each dispatch is far too small to fill the GPU on
	// its own, which is why this pays at low resolution and not at 1080p 4:4:4.
	auto dequant_enc = begin_compute_pass(cmd, MTLDispatchTypeConcurrent, batch.get(), "decode dequant");
	if (!dequant_enc)
		return RIPTIDE_ERROR_GENERIC;

	dequant_enc.label = @("riptide dequant");
	encode_dequant(decoder, dequant_enc, slot);
	if (decoder->reference[0])
	{
		// Independent of the coefficients, so it runs alongside dequant.
		if (!decoder->prediction_prepared)
			encode_inter_prediction(decoder, dequant_enc, slot);
	}
	else
		std::fill(decoder->mv_prev.begin(), decoder->mv_prev.end(), 0); // intra: the next frame's vectors start from zero
	decoder->prediction_prepared = false;
	[dequant_enc endEncoding];

	// The iDWT is a dependent chain across levels, but the three components within
	// a level are independent, so this is also concurrent with explicit barriers
	// at the level boundaries only. Ordering against the dequant work above comes
	// from the encoder boundary, which Metal tracks automatically.
	auto idwt_enc = begin_compute_pass(cmd, MTLDispatchTypeConcurrent, batch.get(), "decode idwt");
	if (!idwt_enc)
		return RIPTIDE_ERROR_GENERIC;

	idwt_enc.label = @("riptide idwt");
	encode_idwt(decoder, idwt_enc, planes, y > 0 ? y : 0);
	[idwt_enc endEncoding];

	if (batch)
		batch->submit(cmd);

	// Retained until this slot comes round again, so its buffers cannot be rewritten
	// while the GPU is still reading them.
	slot->consumer = cmd;

	decoder->parser.mark_frame_decoded();
	return RIPTIDE_SUCCESS;
}
