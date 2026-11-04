// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT

// Metal backend for the Riptide encoder. Mirrors the compute pipeline of
// riptide_encoder.cpp: dwt -> quant -> analyze_rdo -> resolve_rdo ->
// block_packing on the GPU, then packetize on the CPU.
//
// Two things differ structurally from the Vulkan encoder. It owns its command queue
// rather than taking a caller command buffer, because the packet queries have to block
// on completion. And the result buffers are shared storage read directly by the CPU;
// the Vulkan side stages them through device local memory because host visible writes
// are slow on discrete GPUs, which does not apply to unified memory.

#include "riptide_common.hpp"
#include "../riptide_inter.hpp"

#include "shaders/riptide_msl.h"

#import <IOSurface/IOSurfaceRef.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdlib.h>
#include <string.h>

using namespace Riptide;

namespace
{
// Push constant layouts, matching the Registers structs SPIRV-Cross emitted into
// shaders/metal/*.metal. MSL aligns int2 and float2 to 8 bytes, so several need
// explicit tail padding.
struct DwtPush
{
	int32_t resolution[2];
	float inv_resolution[2];
	int32_t aligned_resolution[2];
};
static_assert(sizeof(DwtPush) == 24, "DwtPush layout mismatch.");

struct QuantPush
{
	int32_t resolution[2];
	int32_t resolution_8x8_blocks[2];
	float inv_resolution[2];
	float input_layer;
	float quant_resolution;
	int32_t block_offset;
	int32_t block_stride;
	float rdo_distortion_scale;
	float padding;
};
static_assert(sizeof(QuantPush) == 48, "QuantPush layout mismatch.");

struct AnalyzePush
{
	int32_t resolution[2];
	int32_t resolution_8x8_blocks[2];
	int32_t block_offset_8x8;
	int32_t block_stride_8x8;
	int32_t block_offset_32x32;
	int32_t block_stride_32x32;
	uint32_t total_wg_count;
	uint32_t num_blocks_aligned;
	uint32_t block_index_shamt;
	uint32_t padding;
};
static_assert(sizeof(AnalyzePush) == 48, "AnalyzePush layout mismatch.");

struct ResolvePush
{
	uint32_t target_payload_size;
	uint32_t num_blocks_per_subdivision;
};
static_assert(sizeof(ResolvePush) == 8, "ResolvePush layout mismatch.");

struct BlockPackingPush
{
	int32_t resolution[2];
	int32_t resolution_32x32_blocks[2];
	int32_t resolution_8x8_blocks[2];
	uint32_t quant_resolution_code;
	uint32_t sequence_code;
	int32_t block_offset_32x32;
	int32_t block_stride_32x32;
	int32_t block_offset_8x8;
	int32_t block_stride_8x8;
};
static_assert(sizeof(BlockPackingPush) == 48, "BlockPackingPush layout mismatch.");

//////
// Initial quantization resolution. Pure float math lifted from
// riptide_encoder.cpp; the only change is that the precision and chroma mode are
// passed in rather than read off Configuration and WaveletBuffers.

float get_noise_power_normalized_quant_resolution(int level, int component, int band, int precision)
{
	// The initial quantization resolution aims for a flat spectrum with noise power
	// normalization. The low-pass gain for CDF 9/7 is 6 dB (1 bit). Every
	// decomposition level subtracts 6 dB.
	int bits = precision >= 1 ? 8 : 6;

	if (band == 0)
		bits += 2;
	else if (band < 3)
		bits += 1;

	bits += level;

	// Chroma starts at level 1, subtract one bit.
	if (component != 0)
		bits--;

	return float(1 << bits);
}

float get_quant_resolution(int level, int component, int band, int precision)
{
	// FP16 range is limited, and this is more than a good enough initial estimate.
	return std::min<float>(precision >= 1 ? 4096.0f : 512.0f,
	                       get_noise_power_normalized_quant_resolution(level, component, band, precision));
}

float get_quant_rdo_distortion_scale(int level, int component, int band, int precision,
                                     ChromaSubsampling chroma)
{
	// From my Linelet master thesis. Copy paste 11 years later, ah yes :D
	float horiz_midpoint = (band & 1) ? 0.75f : 0.25f;
	float vert_midpoint = (band & 2) ? 0.75f : 0.25f;

	// Normal PC monitors.
	constexpr float dpi = 96.0f;
	// Compromise between couch gaming and desktop.
	constexpr float viewing_distance = 1.0f;
	constexpr float cpd_nyquist = 0.34f * viewing_distance * dpi;

	float cpd = std::sqrt(horiz_midpoint * horiz_midpoint + vert_midpoint * vert_midpoint) *
	            cpd_nyquist * std::exp2(-float(level));

	// Don't allow a situation where we're quantizing LL band hard.
	cpd = std::max(cpd, 8.0f);

	float csf = 2.6f * (0.0192f + 0.114f * cpd) * std::exp(-std::pow(0.114f * cpd, 1.1f));

	// Heavily discount chroma quality.
	if (component != 0 && level != DecompositionLevels - 1)
	{
		// Consider chroma a little more important if we're not subsampling.
		if (chroma == ChromaSubsampling::Chroma420)
			csf *= 0.6f;
	}

	// Due to filtering, distortion in lower bands will result in more noise power.
	// By scaling the distortion by this factor, we ensure uniform results.
	float resolution = get_noise_power_normalized_quant_resolution(level, component, band, precision);
	float weighted_resolution = csf * resolution;

	// The distortion is scaled in terms of power, not amplitude.
	return weighted_resolution * weighted_resolution;
}

#ifdef RIPTIDE_METAL_BENCH_HOOKS
// Lets tools/metal/bench_encode.mm measure the concurrent encoder against a serial one.
// Re-read per call rather than cached, so the tool can interleave both modes within one
// run -- background GPU load drifts enough between runs to fake a result otherwise.
bool bench_serial_dispatch()
{
	const char *env = getenv("RIPTIDE_BENCH_SERIAL");
	return env && env[0] == '1';
}
#else
constexpr bool bench_serial_dispatch()
{
	return false;
}
#endif

// The explicit barriers are only needed, and only legal, on a concurrent encoder.
// Outside a benchmark build this folds away and the barrier is unconditional.
void stage_barrier(id<MTLComputeCommandEncoder> enc, MTLBarrierScope scope)
{
	if (!bench_serial_dispatch())
		[enc memoryBarrierWithScope:scope];
}

uint32_t floor_log2(uint32_t v)
{
	uint32_t result = 0;
	while (v > 1)
	{
		v >>= 1;
		result++;
	}
	return result;
}

// The three single channel textures the DWT samples at level 0. For NV12 input two are
// swizzled views of one chroma texture, so the backing objects are tracked separately.
struct InputTextures
{
	id<MTLTexture> sampled[NumComponents] = {};
	// Worst case is NV12: the luma plane, the interleaved chroma plane and its two
	// swizzled views. The three plane paths only ever use three.
	static constexpr int MaxOwned = 4;
	id<MTLTexture> owned[MaxOwned] = {};
	int num_owned = 0;

	id<MTLTexture> adopt(id<MTLTexture> texture)
	{
		if (texture)
		{
			assert(num_owned < MaxOwned);
			owned[num_owned++] = texture;
		}
		return texture;
	}

	void release_all()
	{
		// sampled[] aliases owned[] for NV12, but each is an independent strong
		// reference under ARC, so both arrays have to be cleared.
		for (int i = 0; i < num_owned; i++)
			owned[i] = nil;
		num_owned = 0;
		for (int i = 0; i < NumComponents; i++)
			sampled[i] = nil;
	}

	~InputTextures() { release_all(); }
	InputTextures() = default;
	InputTextures(const InputTextures &) = delete;
	void operator=(const InputTextures &) = delete;
};
}

struct riptide_encoder_opaque
{
	riptide_device device = nullptr;

	BlockLayout layout;
	WaveletPyramid wavelet;

	id<MTLCommandQueue> queue;

	// Device local scratch, sized once from the block layout.
	id<MTLBuffer> bucket_buffer;
	id<MTLBuffer> meta_buffer;
	id<MTLBuffer> block_stat_buffer;
	id<MTLBuffer> payload_data;
	id<MTLBuffer> quant_buffer;

	// Results. Shared storage so the packet queries can read them without a copy.
	id<MTLBuffer> bitstream_meta;
	id<MTLBuffer> bitstream;

	// Input textures for the CPU entry point, reused across frames. The GPU entry
	// point wraps the caller's IOSurfaces per call instead.
	InputTextures cpu_input;
	riptide_cpu_buffer_format cpu_input_format = RIPTIDE_CPU_BUFFER_FORMAT_INT_MAX;

	// Inter frames: the planes to predict from (nil: intra), and the per block
	// prediction weights decided for the frame.
	id<MTLTexture> reference[NumComponents] = {};
	int ref_y = 0; // this image's first row in the reference (a stripe of the frame)
	id<MTLTexture> weights;
	id<MTLTexture> prediction[NumComponents] = {};
	// Motion: last frame's field, and the two the search passes alternate
	// between; vectors[1] holds this frame's after an encode.
	id<MTLTexture> vectors_prev, vectors[2];
	uint32_t inter_count = 0; // inter frames encoded, for the refresh rotation
	// The inter packets' size limit (bitstream/inter.md), and whether the last
	// encoded frame was predicted (it then gets inter packets).
	size_t inter_packet_boundary = 1100;
	bool frame_predicted = false;
	std::vector<uint8_t> inter_bytes;
	std::vector<size_t> inter_sizes;
	std::vector<uint8_t> inter_map;
	std::vector<int16_t> inter_mv, inter_mv_prev;
	// Last frame's source luma and the reconstruction error measured from it, the
	// price of a bit for the vector decision.
	id<MTLTexture> last_source;
	id<MTLBuffer> stats;

	// Retained until the next encode replaces it, so the packet queries have
	// something to block on.
	id<MTLCommandBuffer> pending;
	bool have_result = false;

#ifdef RIPTIDE_METAL_BENCH_HOOKS
	double bench_last_gpu_ms = 0.0;
#endif

	uint32_t sequence_count = 0;

	// No destructor: ARC releases every buffer, the queue and any pending command
	// buffer, and Metal keeps whatever an in-flight encode still references alive.
};

namespace
{
size_t bucket_buffer_size(const BlockLayout &layout)
{
	size_t size = RDOBucketOffset;
	size += size_t(NumRDOBuckets) * BlockSpaceSubdivision * sizeof(uint32_t);
	size += size_t(NumRDOBuckets) * compute_block_count_per_subdivision(layout.block_count_32x32) *
	        BlockSpaceSubdivision * sizeof(RDOperation);
	return size;
}

bool ensure_encode_pipelines(riptide_device device)
{
	std::lock_guard<std::mutex> holder{device->encode_pipeline_lock};
	if (device->encode_pipelines_ready)
		return true;

	// Each shader is its own MTLLibrary. They cannot be concatenated: SPIRV-Cross emits
	// its own spv* helpers and a differently shaped Registers into every source.
	const char *dwt_source;
	switch (device->precision)
	{
	case 0: dwt_source = dwt_fp16_msl_source; break;
	case 1: dwt_source = dwt_fp16_storage_msl_source; break;
	default: dwt_source = dwt_msl_source; break;
	}

	char dwt_label[32];
	snprintf(dwt_label, sizeof(dwt_label), "dwt (precision %d)", device->precision);

	if (auto *library = compile_library(device, dwt_source, dwt_label))
	{
		// Indexed by DCShift, which converts the unorm input planes into the signed
		// range the wavelet transform works in.
		for (int i = 0; i < 3; i++)
			device->dwt_pipeline[i] = create_dwt_pipeline(device, library, "riptide_dwt", DwtThreadgroupSize, i);
	}

	// SkipQuantScale is function constant 1 and wants its default of false, but Metal
	// will not build a pipeline from an unspecialized function that declares any
	// constants, so it has to be set explicitly.
	if (auto *library = compile_library(device, inter_decide_msl_source, "inter_decide"))
		device->decide_pipeline = create_pipeline(device, library, "riptide_inter_decide", 64);
	if (auto *library = compile_library(device, inter_search_msl_source, "inter_search"))
		device->search_pipeline = create_pipeline(device, library, "riptide_inter_search", 72);
	if (auto *library = compile_library(device, inter_frame_mse_msl_source, "inter_frame_mse"))
		device->frame_mse_pipeline = create_pipeline(device, library, "riptide_inter_frame_mse", 64);
	if (auto *library = compile_library(device, inter_vector_rd_msl_source, "inter_vector_rd"))
		device->vector_rd_pipeline = create_pipeline(device, library, "riptide_inter_vector_rd", 256);
	if (auto *library = compile_library(device, inter_side_cost_msl_source, "inter_side_cost"))
		device->side_cost_pipeline = create_pipeline_bool_constant(device, library, "riptide_inter_side_cost", 256, 0,
		                                                           getenv("RT_KEEP_INTRA_VECTORS") == nullptr);

	if (auto *library = compile_library(device, wavelet_quant_msl_source, "wavelet_quant"))
	{
		device->quant_pipeline = create_pipeline_bool_constant(
				device, library, "riptide_wavelet_quant", QuantThreadgroupSize, 1, false);
	}

	struct
	{
		__strong id<MTLComputePipelineState> *pipeline;
		const char *source;
		const char *entry_point;
		uint32_t threads;
	} const simple[] = {
		{ &device->analyze_pipeline, analyze_rate_control_msl_source,
		  "riptide_analyze_rate_control", AnalyzeThreadgroupSize },
		{ &device->analyze_finalize_pipeline, analyze_rate_control_finalize_msl_source,
		  "riptide_analyze_rate_control_finalize", AnalyzeFinalizeThreadgroupSize },
		{ &device->block_packing_pipeline, block_packing_msl_source,
		  "riptide_block_packing", BlockPackingThreadgroupSize },
	};

	for (auto &shader : simple)
	{
		if (auto *library = compile_library(device, shader.source, shader.entry_point))
		{
			*shader.pipeline = create_pipeline(device, library, shader.entry_point, shader.threads);
		}
	}

	// resolve_rate_control declares its workgroup size as a specialization
	// constant and requires it to equal the SIMD width, so the whole threadgroup
	// is one SIMD group and its subgroup scan needs no cross-group communication.
	if (auto *library = compile_library(device, resolve_rate_control_msl_source, "resolve_rate_control"))
	{
		uint32_t threadgroup_size = ResolveThreadgroupSize;
		auto constants = [MTLFunctionConstantValues new];
		[constants setConstantValue:&threadgroup_size type:MTLDataTypeUInt atIndex:0];
		device->resolve_pipeline = create_pipeline(device, library, "riptide_resolve_rate_control",
		                                           ResolveThreadgroupSize, constants);
	}

	id<MTLComputePipelineState> const required[] = {
		device->dwt_pipeline[0], device->dwt_pipeline[1], device->dwt_pipeline[2], device->decide_pipeline, device->search_pipeline,
		device->side_cost_pipeline, device->frame_mse_pipeline, device->vector_rd_pipeline,
		device->quant_pipeline,
		device->analyze_pipeline, device->analyze_finalize_pipeline,
		device->resolve_pipeline, device->block_packing_pipeline,
	};

	for (auto *pipeline : required)
		if (!pipeline)
			return false;

	if (device->resolve_pipeline.threadExecutionWidth != ResolveThreadgroupSize)
	{
		device->log("resolve_rate_control needs a SIMD width of %u, but the device reports %u.",
		            ResolveThreadgroupSize,
		            unsigned(device->resolve_pipeline.threadExecutionWidth));
		return false;
	}

	// The quantizer samples with a border so coefficients past the edge of a band
	// read as zero, rather than the DWT's mirror repeat.
	auto sampler_desc = [MTLSamplerDescriptor new];
	sampler_desc.minFilter = MTLSamplerMinMagFilterNearest;
	sampler_desc.magFilter = MTLSamplerMinMagFilterNearest;
	sampler_desc.mipFilter = MTLSamplerMipFilterNearest;
	sampler_desc.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
	sampler_desc.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
	sampler_desc.rAddressMode = MTLSamplerAddressModeClampToBorderColor;
	sampler_desc.borderColor = MTLSamplerBorderColorTransparentBlack;
	device->border_sampler = [device->mtl newSamplerStateWithDescriptor:sampler_desc];

	if (!device->border_sampler)
		return false;

	device->encode_pipelines_ready = true;
	return true;
}

id<MTLBuffer> create_scratch_buffer(riptide_device device, size_t size, MTLResourceOptions options,
                                   const char *label)
{
	auto buffer = [device->mtl newBufferWithLength:size options:options];
	if (!buffer)
	{
		device->log("Failed to allocate a %zu byte %s buffer.", size, label);
		return nullptr;
	}
	buffer.label = @(label);
	return buffer;
}

bool create_encode_buffers(riptide_encoder encoder)
{
	auto *device = encoder->device;
	const auto &layout = encoder->layout;

	struct
	{
		__strong id<MTLBuffer> *buffer;
		size_t size;
		const char *label;
	} const scratch[] = {
		{ &encoder->block_stat_buffer,
		  size_t(layout.block_count_8x8) * sizeof(BlockStatsBlock), "riptide-block-stats" },
		{ &encoder->meta_buffer,
		  size_t(layout.block_count_8x8) * sizeof(BlockMeta), "riptide-block-meta" },
		// Worst case estimate, same as the Vulkan encoder's. The first two words are
		// allocation counters and the coefficient payload starts at byte 8.
		{ &encoder->payload_data,
		  size_t(layout.aligned_width) * size_t(layout.aligned_height) * 2, "riptide-payload" },
		{ &encoder->quant_buffer,
		  size_t(layout.block_count_32x32) * sizeof(uint32_t), "riptide-quant" },
		{ &encoder->bucket_buffer, bucket_buffer_size(layout), "riptide-buckets" },
	};

	for (auto &entry : scratch)
	{
		*entry.buffer = create_scratch_buffer(device, entry.size,
		                                      MTLResourceStorageModePrivate, entry.label);
		if (!*entry.buffer)
			return false;
	}

	encoder->bitstream_meta = create_scratch_buffer(
			device, size_t(layout.block_count_32x32) * sizeof(BitstreamPacket),
			MTLResourceStorageModeShared, "riptide-bitstream-meta");

	return encoder->bitstream_meta != nullptr;
}

// Sized per encode, since the rate control target can change per frame. Releasing the
// old one mid-flight is safe: Metal keeps it alive while a command buffer references it.
bool ensure_bitstream_buffer(riptide_encoder encoder, size_t size)
{
	if (encoder->bitstream && encoder->bitstream.length >= size)
		return true;


	encoder->bitstream = create_scratch_buffer(encoder->device, size,
	                                           MTLResourceStorageModeShared, "riptide-bitstream");
	return encoder->bitstream != nullptr;
}

id<MTLTexture> create_input_texture(riptide_device device, MTLPixelFormat format,
                                   int width, int height, bool needs_view)
{
	auto desc = [MTLTextureDescriptor new];
	desc.textureType = MTLTextureType2D;
	desc.pixelFormat = format;
	desc.width = width;
	desc.height = height;
	desc.usage = MTLTextureUsageShaderRead |
	               (needs_view ? MTLTextureUsagePixelFormatView : 0);
	// Written by the CPU, and IOSurface backed textures cannot be private anyway.
	desc.storageMode = MTLStorageModeShared;
	desc.mipmapLevelCount = 1;

	auto texture = [device->mtl newTextureWithDescriptor:desc];

	if (!texture)
		device->log("Failed to allocate a %dx%d input texture.", width, height);

	return texture;
}

id<MTLTexture> wrap_surface_plane(riptide_device device, IOSurfaceRef surface, int plane,
                                 MTLPixelFormat format, int width, int height, bool needs_view)
{
	auto desc = [MTLTextureDescriptor new];
	desc.textureType = MTLTextureType2D;
	desc.pixelFormat = format;
	desc.width = width;
	desc.height = height;
	desc.usage = MTLTextureUsageShaderRead |
	               (needs_view ? MTLTextureUsagePixelFormatView : 0);
	desc.storageMode = MTLStorageModeShared;
	desc.mipmapLevelCount = 1;

	id<MTLTexture> texture = [device->mtl newTextureWithDescriptor:desc iosurface:surface plane:plane];

	if (!texture)
		device->log("Failed to wrap plane %d of the input IOSurface.", plane);

	return texture;
}

// dwt gathers the red channel of whatever it is handed, so an interleaved chroma plane
// is bound twice with the wanted component broadcast into red -- what the Vulkan API
// documents for NV12 too ("pass in the same plane for Cb and Cr, but use swizzle"). An
// R8 format view of an RG8 texture is impossible, 8 versus 16 bpp, so only the swizzle
// changes. The same goes for RG16 chroma from P010 surfaces.
bool make_interleaved_chroma_views(riptide_device device, InputTextures &input, id<MTLTexture> chroma)
{
	static const MTLTextureSwizzle components[2] = { MTLTextureSwizzleRed, MTLTextureSwizzleGreen };

	for (int i = 0; i < 2; i++)
	{
		const auto c = components[i];
		input.sampled[i + 1] = input.adopt(
				[chroma newTextureViewWithPixelFormat:chroma.pixelFormat
				                         textureType:MTLTextureType2D
				                              levels:NSMakeRange(0, 1)
				                              slices:NSMakeRange(0, 1)
				                             swizzle:MTLTextureSwizzleChannelsMake(c, c, c, c)]);

		if (!input.sampled[i + 1])
		{
			device->log("Failed to create a swizzled view of the interleaved chroma plane.");
			return false;
		}
	}

	return true;
}

bool ensure_cpu_input(riptide_encoder encoder, riptide_cpu_buffer_format format)
{
	if (encoder->cpu_input_format == format && encoder->cpu_input.sampled[0])
		return true;

	encoder->cpu_input.release_all();
	encoder->cpu_input_format = RIPTIDE_CPU_BUFFER_FORMAT_INT_MAX;

	auto *device = encoder->device;
	auto &input = encoder->cpu_input;
	const auto &layout = encoder->layout;
	const bool chroma_420 = layout.chroma == ChromaSubsampling::Chroma420;
	const int chroma_width = chroma_420 ? layout.width / 2 : layout.width;
	const int chroma_height = chroma_420 ? layout.height / 2 : layout.height;

	input.sampled[0] = input.adopt(create_input_texture(device, MTLPixelFormatR8Unorm,
	                                                    layout.width, layout.height, false));
	if (!input.sampled[0])
		return false;

	if (format == RIPTIDE_CPU_BUFFER_FORMAT_NV12)
	{
		auto *chroma = input.adopt(create_input_texture(device, MTLPixelFormatRG8Unorm,
		                                                chroma_width, chroma_height, true));
		if (!chroma || !make_interleaved_chroma_views(device, input, chroma))
			return false;
	}
	else
	{
		for (int i = 1; i < NumComponents; i++)
		{
			input.sampled[i] = input.adopt(create_input_texture(device, MTLPixelFormatR8Unorm,
			                                                    chroma_width, chroma_height, false));
			if (!input.sampled[i])
				return false;
		}
	}

	encoder->cpu_input_format = format;
	return true;
}

//////
// GPU dispatch, one function per stage of riptide_encoder.cpp's pipeline.

void dispatch_dwt(riptide_encoder encoder, id<MTLComputeCommandEncoder> enc,
                id<MTLTexture> const planes[NumComponents])
{
	const auto &layout = encoder->layout;
	auto &wavelet = encoder->wavelet;
	const bool chroma_420 = layout.chroma == ChromaSubsampling::Chroma420;

	[enc setSamplerState:encoder->device->mirror_repeat_sampler atIndex:0];

	for (int output_level = 0; output_level < DecompositionLevels; output_level++)
	{
		// Each level transforms the LL band the previous one produced, so the levels
		// are a dependent chain. The components within a level are independent.
		if (output_level != 0)
			stage_barrier(enc, MTLBarrierScopeTextures);

		DwtPush level_push = {};
		if (output_level == 0)
		{
			// The input planes are only `width` wide, but the transform covers the
			// aligned extent and mirrors the source to fill it.
			level_push.resolution[0] = layout.width;
			level_push.resolution[1] = layout.height;
			level_push.aligned_resolution[0] = layout.aligned_width;
			level_push.aligned_resolution[1] = layout.aligned_height;
		}
		else
		{
			level_push.resolution[0] = layout.level_width(output_level - 1);
			level_push.resolution[1] = layout.level_height(output_level - 1);
			level_push.aligned_resolution[0] = level_push.resolution[0];
			level_push.aligned_resolution[1] = level_push.resolution[1];
		}

		// Under 420, chroma is half resolution and so enters the pyramid at level 1
		// rather than level 0.
		const int components = (output_level == 0 && chroma_420) ? 1 : NumComponents;

		for (int component = 0; component < components; component++)
		{
			DwtPush push = level_push;
			id<MTLTexture> input;
			// DCShift subtracts 0.5 to centre unorm input on zero, so it applies
			// exactly when the source is an input plane rather than a previous level.
			bool reads_input_plane = output_level == 0;

			if (output_level == 0)
			{
				input = planes[component];
			}
			else if (chroma_420 && component != 0 && output_level == 1)
			{
				input = planes[component];
				reads_input_plane = true;
				push.resolution[0] = layout.width / 2;
				push.resolution[1] = layout.height / 2;
				push.aligned_resolution[0] = layout.aligned_width >> output_level;
				push.aligned_resolution[1] = layout.aligned_height >> output_level;
			}
			else
			{
				input = wavelet.component_ll_views[component][output_level - 1];
			}

			push.inv_resolution[0] = 1.0f / float(push.resolution[0]);
			push.inv_resolution[1] = 1.0f / float(push.resolution[1]);

			const bool predict = reads_input_plane && encoder->reference[component];
			[enc setComputePipelineState:encoder->device->dwt_pipeline[predict ? 2 : reads_input_plane ? 1 : 0]];
			[enc setBytes:&push length:sizeof(push) atIndex:0];
			[enc setTexture:input atIndex:0];
			if (predict)
			{
				[enc setTexture:encoder->prediction[component] atIndex:2];
				[enc setSamplerState:encoder->device->mirror_repeat_sampler atIndex:2];
			}
			[enc setTexture:wavelet.component_layer_views[component][output_level] atIndex:1];

			// Each threadgroup consumes a 32x32 source tile.
			[enc dispatchThreadgroups:MTLSizeMake((push.aligned_resolution[0] + 31) / 32,
					          (push.aligned_resolution[1] + 31) / 32, 1) threadsPerThreadgroup:MTLSizeMake(DwtThreadgroupSize, 1, 1)];
		}
	}
}

// The remaining stages all walk the same (level, component, band) space as the
// decoder's dequant pass.
template <typename Op>
void for_each_band(const BlockLayout &layout, Op op)
{
	for (int level = 0; level < DecompositionLevels; level++)
	{
		for (int component = 0; component < NumComponents; component++)
		{
			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0 && layout.chroma == ChromaSubsampling::Chroma420)
				continue;

			for (int band = (level == DecompositionLevels - 1 ? 0 : 1); band < 4; band++)
				op(level, component, band);
		}
	}
}

void dispatch_quant(riptide_encoder encoder, id<MTLComputeCommandEncoder> enc)
{
	const auto &layout = encoder->layout;
	const int precision = encoder->device->precision;

	[enc setComputePipelineState:encoder->device->quant_pipeline];
	[enc setBuffer:encoder->meta_buffer offset:0 atIndex:1];
	[enc setBuffer:encoder->block_stat_buffer offset:0 atIndex:2];
	[enc setBuffer:encoder->payload_data offset:0 atIndex:3];
	[enc setSamplerState:encoder->device->border_sampler atIndex:0];

	for_each_band(layout, [&](int level, int component, int band) {
		QuantPush push = {};
		push.resolution[0] = layout.level_width(level);
		push.resolution[1] = layout.level_height(level);
		push.resolution_8x8_blocks[0] = (push.resolution[0] + 7) / 8;
		push.resolution_8x8_blocks[1] = (push.resolution[1] + 7) / 8;
		push.inv_resolution[0] = 1.0f / float(push.resolution[0]);
		push.inv_resolution[1] = 1.0f / float(push.resolution[1]);
		push.input_layer = float(band);

		// The round trip through the 8 bit quant code is deliberate: the decoder
		// only ever sees the code, so quantize against the value it will dequantize
		// with rather than the ideal one.
		const float quant_res = get_quant_resolution(level, component, band, precision);
		push.quant_resolution = 1.0f / decode_quant(encode_quant(1.0f / quant_res));
		push.rdo_distortion_scale =
				get_quant_rdo_distortion_scale(level, component, band, precision, layout.chroma) *
				(1.0f / 256.0f);

		push.block_offset = layout.block_meta[component][level][band].block_offset_8x8;
		push.block_stride = layout.block_meta[component][level][band].block_stride_8x8;

		[enc setTexture:encoder->wavelet.component_layer_views[component][level] atIndex:0];
		[enc setBytes:&push length:sizeof(push) atIndex:0];
		[enc dispatchThreadgroups:MTLSizeMake((push.resolution[0] + 31) / 32,
		                                     (push.resolution[1] + 31) / 32, 1)
		      threadsPerThreadgroup:MTLSizeMake(QuantThreadgroupSize, 1, 1)];
	});
}

void dispatch_analyze_rdo(riptide_encoder encoder, id<MTLComputeCommandEncoder> enc)
{
	const auto &layout = encoder->layout;
	const int per_subdivision = compute_block_count_per_subdivision(layout.block_count_32x32);

	[enc setComputePipelineState:encoder->device->analyze_pipeline];
	[enc setBuffer:encoder->bucket_buffer offset:0 atIndex:0];
	[enc setBuffer:encoder->block_stat_buffer offset:0 atIndex:2];

	for_each_band(layout, [&](int level, int component, int band) {
		AnalyzePush push = {};
		push.resolution[0] = layout.level_width(level);
		push.resolution[1] = layout.level_height(level);
		push.resolution_8x8_blocks[0] = (push.resolution[0] + 7) / 8;
		push.resolution_8x8_blocks[1] = (push.resolution[1] + 7) / 8;

		const auto &meta = layout.block_meta[component][level][band];
		push.block_offset_8x8 = meta.block_offset_8x8;
		push.block_stride_8x8 = meta.block_stride_8x8;
		push.block_offset_32x32 = meta.block_offset_32x32;
		push.block_stride_32x32 = meta.block_stride_32x32;
		push.total_wg_count = uint32_t(layout.block_count_32x32);
		push.num_blocks_aligned = uint32_t(per_subdivision * BlockSpaceSubdivision);
		push.block_index_shamt = floor_log2(uint32_t(per_subdivision));

		[enc setBytes:&push length:sizeof(push) atIndex:1];
		[enc dispatchThreadgroups:MTLSizeMake((push.resolution[0] + 31) / 32,
		                                     (push.resolution[1] + 31) / 32, 1)
		      threadsPerThreadgroup:MTLSizeMake(AnalyzeThreadgroupSize, 1, 1)];
	});

	// The finalize pass prefix sums the buckets the dispatches above filled.
	stage_barrier(enc, MTLBarrierScopeBuffers);
	[enc setComputePipelineState:encoder->device->analyze_finalize_pipeline];
	[enc setBuffer:encoder->bucket_buffer offset:0 atIndex:0];
	[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(AnalyzeFinalizeThreadgroupSize, 1, 1)];
}

void dispatch_resolve_rdo(riptide_encoder encoder, id<MTLComputeCommandEncoder> enc, size_t target_size)
{
	const auto &layout = encoder->layout;

	// The sequence header is part of the frame's budget, so the payload target is
	// what is left after it. Guard the subtraction for absurdly small targets.
	if (target_size >= sizeof(BitstreamSequenceHeader))
		target_size -= sizeof(BitstreamSequenceHeader);

	ResolvePush push = {};
	push.target_payload_size = uint32_t(target_size / sizeof(uint32_t));
	push.num_blocks_per_subdivision = uint32_t(compute_block_count_per_subdivision(layout.block_count_32x32));

	[enc setComputePipelineState:encoder->device->resolve_pipeline];
	[enc setBuffer:encoder->bucket_buffer offset:0 atIndex:0];
	[enc setBytes:&push length:sizeof(push) atIndex:1];
	[enc setBuffer:encoder->quant_buffer offset:0 atIndex:2];
	[enc dispatchThreadgroups:MTLSizeMake(NumRDOBuckets * BlockSpaceSubdivision, 1, 1)
	      threadsPerThreadgroup:MTLSizeMake(ResolveThreadgroupSize, 1, 1)];
}

void dispatch_block_packing(riptide_encoder encoder, id<MTLComputeCommandEncoder> enc)
{
	const auto &layout = encoder->layout;
	const int precision = encoder->device->precision;

	// SPIRV-Cross renumbered these; the GLSL binding order is 1, 6, 4, 0, 5, 3.
	[enc setComputePipelineState:encoder->device->block_packing_pipeline];
	[enc setBuffer:encoder->payload_data offset:0 atIndex:0];
	[enc setBuffer:encoder->bitstream offset:0 atIndex:1];
	[enc setBuffer:encoder->quant_buffer offset:0 atIndex:3];
	[enc setBuffer:encoder->meta_buffer offset:0 atIndex:4];
	[enc setBuffer:encoder->block_stat_buffer offset:0 atIndex:5];
	[enc setBuffer:encoder->bitstream_meta offset:0 atIndex:6];

	for_each_band(layout, [&](int level, int component, int band) {
		BlockPackingPush push = {};
		push.resolution[0] = layout.level_width(level);
		push.resolution[1] = layout.level_height(level);
		push.resolution_32x32_blocks[0] = (push.resolution[0] + 31) / 32;
		push.resolution_32x32_blocks[1] = (push.resolution[1] + 31) / 32;
		push.resolution_8x8_blocks[0] = (push.resolution[0] + 7) / 8;
		push.resolution_8x8_blocks[1] = (push.resolution[1] + 7) / 8;

		// The code itself here, not the reciprocal the quantizer scales by.
		const float quant_res = get_quant_resolution(level, component, band, precision);
		push.quant_resolution_code = encode_quant(1.0f / quant_res);
		push.sequence_code = encoder->sequence_count;

		const auto &meta = layout.block_meta[component][level][band];
		push.block_offset_32x32 = meta.block_offset_32x32;
		push.block_stride_32x32 = meta.block_stride_32x32;
		push.block_offset_8x8 = meta.block_offset_8x8;
		push.block_stride_8x8 = meta.block_stride_8x8;

		[enc setBytes:&push length:sizeof(push) atIndex:2];
		// Note the /2: unlike the other stages a threadgroup covers 2x2 of the
		// 32x32 blocks, one per 16 lanes.
		[enc dispatchThreadgroups:MTLSizeMake((push.resolution_32x32_blocks[0] + 1) / 2,
		                                     (push.resolution_32x32_blocks[1] + 1) / 2, 1)
		      threadsPerThreadgroup:MTLSizeMake(BlockPackingThreadgroupSize, 1, 1)];
	});
}

riptide_result encode_frame(riptide_encoder encoder, id<MTLTexture> const planes[NumComponents],
                             const riptide_rate_control *rate_control)
{
	// Block packing writes u32 words, so round the budget down like the Vulkan
	// encoder does.
	const size_t target_size = rate_control->maximum_bitstream_size & ~size_t(3);
	if (target_size == 0 || target_size > UINT32_MAX)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	// Same slack the Vulkan encoder leaves for the packer to overshoot into.
	const size_t meta_size = size_t(encoder->layout.block_count_32x32) * sizeof(BitstreamPacket);
	if (!ensure_bitstream_buffer(encoder, target_size + meta_size))
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	encoder->sequence_count = (encoder->sequence_count + 1) & SequenceCountMask;
	encoder->frame_predicted = encoder->reference[0] != nil;
	encoder->inter_bytes.clear();
	encoder->inter_sizes.clear();

	id<MTLCommandBuffer> cmd = [encoder->queue commandBuffer];
	if (!cmd)
		return RIPTIDE_ERROR_GENERIC;
	cmd.label = @("riptide encode");

	// Per pass timestamps, null unless collection is on.
	auto batch = encoder->device->timestamps->begin_batch();

	auto blit = begin_blit_pass(cmd, batch.get(), "encode clear");
	if (!blit)
		return RIPTIDE_ERROR_GENERIC;
	// Accumulated into, so they have to start from zero. The rest of the payload buffer
	// is fully overwritten by whatever is coded.
	[blit fillBuffer:encoder->payload_data range:NSMakeRange(0, 2 * sizeof(uint32_t)) value:0];
	[blit fillBuffer:encoder->bucket_buffer range:NSMakeRange(0, encoder->bucket_buffer.length) value:0];
	[blit fillBuffer:encoder->quant_buffer range:NSMakeRange(0, encoder->quant_buffer.length) value:0];
	// A block's payload rarely ends on a word boundary and the packer never writes the
	// leftover bytes of its final word, which still go out on the wire.
	//
	// This does NOT make the bitstream byte for byte reproducible, so never diff two
	// bitstreams to check a change -- diff the decoded output, which is deterministic.
	// The unused bits of a block's last sign byte come from shared_sign_bank in
	// block_packing.comp, which is never initialized and is updated with atomicAnd/Or
	// that preserve bits outside the write mask, so they are leftover threadgroup
	// memory. Decoders read only as many sign bits as there are significant
	// coefficients; the Vulkan encoder behaves the same way.
	[blit fillBuffer:encoder->bitstream range:NSMakeRange(0, target_size + meta_size) value:0];
	if (!encoder->stats)
	{
		encoder->stats = [encoder->device->mtl newBufferWithLength:16 options:MTLResourceStorageModePrivate];
		auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
		                                                               width:encoder->layout.width
		                                                              height:encoder->layout.height mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModePrivate;
		encoder->last_source = [encoder->device->mtl newTextureWithDescriptor:desc];
	}
	[blit fillBuffer:encoder->stats range:NSMakeRange(0, 16) value:0];
	[blit endEncoding];

	// One concurrent encoder for the whole pipeline, with explicit barriers exactly where
	// the Vulkan encoder has them. Everything between two barriers is independent -- each
	// dispatch owns a distinct (component, level, band) region -- and a serial encoder
	// would barrier between all ~170.
	// Profiling knob RT_SPLIT: one timed pass per stage instead of one for all.
	static const bool split_passes = getenv("RT_SPLIT") != nullptr;
	const MTLDispatchType dispatch_type = bench_serial_dispatch() ? MTLDispatchTypeSerial : MTLDispatchTypeConcurrent;
	id<MTLComputeCommandEncoder> enc = begin_compute_pass(
			cmd, dispatch_type, batch.get(),
			split_passes ? (encoder->reference[0] ? "encode search" : "encode dwt") : "encode compute");
	if (!enc)
		return RIPTIDE_ERROR_GENERIC;
	enc.label = @("riptide encode");
	auto split = [&](const char *next) {
		if (!split_passes)
			return;
		[enc endEncoding];
		enc = begin_compute_pass(cmd, dispatch_type, batch.get(), next);
	};

	if (encoder->reference[0])
	{
		auto *device = encoder->device;
		const int w = encoder->layout.width, h = encoder->layout.height;
		if (!encoder->weights)
			encoder->weights = create_weight_texture(device, w, h);
		if (!encoder->vectors_prev)
		{
			encoder->vectors_prev = create_vector_texture(device, w, h);
			encoder->vectors[0] = create_vector_texture(device, w, h);
			encoder->vectors[1] = create_vector_texture(device, w, h);
		}
		else
			std::swap(encoder->vectors_prev, encoder->vectors[1]); // last frame's field

		// Motion search on luma: six passes, the nudge halving from 8 pixels to a
		// quarter, the first starting from last frame's field.
		// Experiment knob RT_SEARCH_STEPS: the nudge per pass, quarter pel
		// (default 32,16,8,4,2,1).
		static const std::vector<int> steps = [] {
			std::vector<int> v;
			const char *e = getenv("RT_SEARCH_STEPS");
			std::string str = e ? e : "32,16,8,4,2,1";
			for (size_t i = 0; i < str.size();) {
				size_t j = str.find(',', i);
				v.push_back(atoi(str.substr(i, j - i).c_str()));
				i = j == std::string::npos ? str.size() : j + 1;
			}
			return v;
		}();
		const int passes = (int)steps.size();

		// Last frame's reconstruction error, alongside the search.
		[enc setComputePipelineState:device->frame_mse_pipeline];
		[enc setBuffer:encoder->stats offset:0 atIndex:0];
		[enc setTexture:encoder->reference[0] atIndex:1];
		[enc setTexture:encoder->last_source atIndex:2];
		[enc setSamplerState:device->mirror_repeat_sampler atIndex:1];
		[enc setSamplerState:device->mirror_repeat_sampler atIndex:2];
		[enc dispatchThreadgroups:MTLSizeMake((w + 31) / 32, (h + 31) / 32, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];

		[enc setComputePipelineState:device->search_pipeline];
		[enc setTexture:planes[0] atIndex:0];
		[enc setTexture:encoder->reference[0] atIndex:2];
		[enc setTexture:encoder->vectors_prev atIndex:4];
		for (int i : {0, 3, 4})
			[enc setSamplerState:device->mirror_repeat_sampler atIndex:i];
		[enc setSamplerState:device->linear_mirror_sampler atIndex:2];
		static const float lambda = getenv("RT_LAMBDA") ? (float)atof(getenv("RT_LAMBDA")) : 0.02f;
		// The last pass writes vectors[1].
		for (int it = 0; it < passes; it++)
		{
			const int out = (passes - 1 - it) & 1 ? 0 : 1;
			static const float zero_bias = getenv("RT_ZBIAS") ? (float)atof(getenv("RT_ZBIAS")) : 0.1f;
			struct { int32_t step, iteration; float lambda, zero_bias; int32_t ref_y; } push = {steps[it], it, lambda, zero_bias, encoder->ref_y};
			[enc setBytes:&push length:sizeof(push) atIndex:0];
			[enc setTexture:it == 0 ? encoder->vectors_prev : encoder->vectors[out ^ 1] atIndex:3];
			[enc setTexture:encoder->vectors[out] atIndex:1];
			[enc dispatchThreadgroups:MTLSizeMake((encoder->vectors[0].width + 7) / 8, encoder->vectors[0].height, 1)
			    threadsPerThreadgroup:MTLSizeMake(72, 1, 1)];
			stage_barrier(enc, MTLBarrierScopeTextures);
		}

		split("encode decide");
		// Then each block's prediction weight, against the motion compensated reference.
		[enc setComputePipelineState:device->decide_pipeline];
		// Experiment knob RT_REFRESH=N: each block coded intra once every N frames.
		static const int refresh = getenv("RT_REFRESH") ? atoi(getenv("RT_REFRESH")) : 0;
		struct { int32_t period, phase, ref_y; } refresh_push = {refresh, refresh ? int32_t(encoder->inter_count++ % refresh) : 0, encoder->ref_y};
		[enc setBytes:&refresh_push length:sizeof(refresh_push) atIndex:0];
		[enc setTexture:planes[0] atIndex:0];
		[enc setTexture:encoder->weights atIndex:1];
		[enc setTexture:encoder->reference[0] atIndex:2];
		[enc setTexture:encoder->vectors[1] atIndex:3];
		[enc setSamplerState:encoder->device->mirror_repeat_sampler atIndex:0];
		[enc setSamplerState:encoder->device->mirror_repeat_sampler atIndex:2];
		[enc dispatchThreadgroups:MTLSizeMake(encoder->weights.width, encoder->weights.height, 1)
		    threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
		stage_barrier(enc, MTLBarrierScopeTextures);
		// Each 4x4 tile of vectors kept only if worth its bits. Experiment knob
		// RT_VRD: the price's calibration (0: off).
		static const float vrd = getenv("RT_VRD") ? (float)atof(getenv("RT_VRD")) : 0.0f;
		if (vrd > 0.0f)
		{
			[enc setComputePipelineState:device->vector_rd_pipeline];
			[enc setBytes:&vrd length:sizeof(vrd) atIndex:0];
			[enc setBuffer:encoder->stats offset:0 atIndex:6];
			[enc setTexture:encoder->vectors[1] atIndex:1];
			[enc setTexture:encoder->vectors_prev atIndex:2];
			[enc setTexture:planes[0] atIndex:3];
			[enc setTexture:encoder->reference[0] atIndex:4];
			[enc setTexture:encoder->weights atIndex:5];
			for (int i : {2, 3, 5})
				[enc setSamplerState:device->mirror_repeat_sampler atIndex:i];
			[enc setSamplerState:device->linear_mirror_sampler atIndex:4];
			[enc dispatchThreadgroups:MTLSizeMake((encoder->vectors[1].width + 3) / 4, (encoder->vectors[1].height + 3) / 4, 1)
			    threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
			stage_barrier(enc, MTLBarrierScopeTextures | MTLBarrierScopeBuffers);
		}
		// After the decision: free the intra blocks' vectors, then charge the vectors'
		// and the map's size to the rate control (it only adds to the payload
		// analyze_rate_control accumulates, so nothing waits for it but resolve).
		[enc setComputePipelineState:device->side_cost_pipeline];
		const uint32_t inter_boundary = uint32_t(encoder->inter_packet_boundary);
		[enc setBytes:&inter_boundary length:sizeof(inter_boundary) atIndex:0];
		[enc setBuffer:encoder->bucket_buffer offset:0 atIndex:4];
		[enc setTexture:encoder->vectors[1] atIndex:1];
		[enc setTexture:encoder->vectors_prev atIndex:2];
		[enc setTexture:encoder->weights atIndex:3];
		[enc setSamplerState:device->mirror_repeat_sampler atIndex:2];
		[enc setSamplerState:device->mirror_repeat_sampler atIndex:3];
		[enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];

		stage_barrier(enc, MTLBarrierScopeTextures);
		split("encode predict");
		dispatch_inter_predict(encoder->device, enc, encoder->reference, encoder->ref_y, encoder->layout.width, encoder->layout.height,
		                       encoder->weights, encoder->vectors[1], encoder->prediction);
		stage_barrier(enc, MTLBarrierScopeTextures);
		split("encode dwt");
	}
	dispatch_dwt(encoder, enc, planes);
	// The quantizer samples the coefficients the DWT just wrote.
	stage_barrier(enc, MTLBarrierScopeTextures);
	split("encode quant");
	dispatch_quant(encoder, enc);
	// Rate control analysis reads the per block statistics and payload sizes.
	stage_barrier(enc, MTLBarrierScopeBuffers);
	split("encode rate control+pack");
	dispatch_analyze_rdo(encoder, enc);
	stage_barrier(enc, MTLBarrierScopeBuffers);
	dispatch_resolve_rdo(encoder, enc, target_size);
	// Packing needs the quant decisions resolve just made.
	stage_barrier(enc, MTLBarrierScopeBuffers);
	dispatch_block_packing(encoder, enc);

	[enc endEncoding];

	// This frame's source luma, for the next frame's reconstruction error (8-bit
	// sources only, for now).
	if (planes[0].pixelFormat == MTLPixelFormatR8Unorm)
		if (auto copy = [cmd blitCommandEncoder])
	{
		[copy copyFromTexture:planes[0] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		           sourceSize:MTLSizeMake(encoder->layout.width, encoder->layout.height, 1)
		            toTexture:encoder->last_source destinationSlice:0 destinationLevel:0
		    destinationOrigin:MTLOriginMake(0, 0, 0)];
		[copy endEncoding];
	}

	if (batch)
		batch->submit(cmd);

	// Whole command buffer timing. Unlike the pass breakdown it needs no counter
	// support, and this buffer is ours alone, so nothing foreign is counted.
	if (encoder->device->timestamps->collecting())
	{
		auto timestamps = encoder->device->timestamps;
		[cmd addCompletedHandler:^(id<MTLCommandBuffer> completed) {
			// Both are zero if the command buffer never ran.
			const double seconds = completed.GPUEndTime - completed.GPUStartTime;
			if (seconds > 0.0)
				timestamps->accumulate("encode command buffer", seconds);
		}];
	}

	[cmd commit];

	encoder->pending = cmd;
	encoder->have_result = true;

	return RIPTIDE_SUCCESS;
}

// Blocks until the last encode has landed, so the result buffers can be read.
riptide_result wait_for_result(riptide_encoder encoder)
{
	if (!encoder->have_result)
	{
		encoder->device->log("No frame has been encoded yet.");
		return RIPTIDE_ERROR_GENERIC;
	}

	if (encoder->pending)
	{
		[encoder->pending waitUntilCompleted];

#ifdef RIPTIDE_METAL_BENCH_HOOKS
		encoder->bench_last_gpu_ms =
				(encoder->pending.GPUEndTime - encoder->pending.GPUStartTime) * 1000.0;
#endif

		if (encoder->pending.status == MTLCommandBufferStatusError)
		{
			auto *error = encoder->pending.error;
			encoder->device->log("Encode command buffer failed: %s",
			                     error && error.localizedDescription ?
			                     error.localizedDescription.UTF8String : "unknown error");
			encoder->have_result = false;
			return RIPTIDE_ERROR_GENERIC;
		}

		encoder->pending = nullptr;
	}

	return RIPTIDE_SUCCESS;
}

// Validates the caller's CPU buffer and copies it into the input textures.
riptide_result upload_cpu_input(riptide_encoder encoder, const riptide_cpu_buffer *input)
{
	auto *device = encoder->device;
	const auto &layout = encoder->layout;
	const bool chroma_420 = layout.chroma == ChromaSubsampling::Chroma420;
	const int num_planes = input->format == RIPTIDE_CPU_BUFFER_FORMAT_NV12 ? 2 : 3;

	if (input->width != layout.width || input->height != layout.height)
	{
		device->log("Input is %dx%d, but the encoder was created for %dx%d.",
		            input->width, input->height, layout.width, layout.height);
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	if (chroma_420 == (input->format == RIPTIDE_CPU_BUFFER_FORMAT_YUV444P))
	{
		device->log("Input format %d does not match the encoder's chroma subsampling.",
		            int(input->format));
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	for (int plane = 0; plane < num_planes; plane++)
	{
		const int plane_width = plane != 0 && chroma_420 ? layout.width / 2 : layout.width;
		const int plane_height = plane != 0 && chroma_420 ? layout.height / 2 : layout.height;
		// NV12 packs Cb and Cr into one 16bpp plane.
		const size_t bytes_per_pixel = num_planes == 2 && plane == 1 ? 2 : 1;

		if (!input->data[plane] ||
		    input->row_stride_in_bytes[plane] < size_t(plane_width) * bytes_per_pixel ||
		    input->row_stride_in_bytes[plane] * size_t(plane_height) > input->plane_size_in_bytes[plane])
		{
			device->log("Input plane %d is NULL or has an inconsistent stride and size.", plane);
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}
	}

	if (!ensure_cpu_input(encoder, input->format))
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	// The textures are reused across frames, so the previous encode has to be done
	// reading them. In practice this is free: the packet queries have already
	// waited by the time a caller asks for another frame.
	if (encoder->pending)
		[encoder->pending waitUntilCompleted];

	// Both NV12 chroma views alias owned[1], so upload through the backing objects
	// rather than the sampled views.
	for (int plane = 0; plane < num_planes; plane++)
	{
		auto *texture = encoder->cpu_input.owned[plane];
		[texture replaceRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
		           mipmapLevel:0
		             withBytes:input->data[plane]
		           bytesPerRow:input->row_stride_in_bytes[plane]];
	}

	return RIPTIDE_SUCCESS;
}

// Wraps the caller's IOSurfaces: either one biplanar NV12 or P010 surface, or three
// single plane R8 surfaces.
riptide_result wrap_gpu_input(riptide_encoder encoder, const riptide_gpu_input *input,
                               InputTextures &wrapped)
{
	auto *device = encoder->device;
	const auto &layout = encoder->layout;
	const bool chroma_420 = layout.chroma == ChromaSubsampling::Chroma420;
	const int chroma_width = chroma_420 ? layout.width / 2 : layout.width;
	const int chroma_height = chroma_420 ? layout.height / 2 : layout.height;

	auto surface = [&](int i) { return static_cast<IOSurfaceRef>(input->planes[i]); };

	if (!surface(0))
	{
		device->log("Input planes[0] is NULL.");
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	const bool biplanar = !surface(1) && !surface(2);
	if (!biplanar && (!surface(1) || !surface(2)))
	{
		device->log("Expected either one biplanar surface or three single plane surfaces.");
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	// Checks a surface plane against the geometry the shaders will read.
	auto plane_matches = [&](IOSurfaceRef surf, int plane, int width, int height) {
		if (int(IOSurfaceGetPlaneCount(surf)) <= plane && !(plane == 0 && IOSurfaceGetPlaneCount(surf) == 0))
		{
			device->log("Input surface has %zu planes, needs at least %d.",
			            size_t(IOSurfaceGetPlaneCount(surf)), plane + 1);
			return false;
		}

		// A non-planar surface reports 0 planes but still answers plane 0 queries.
		const int surface_width = int(IOSurfaceGetWidthOfPlane(surf, plane));
		const int surface_height = int(IOSurfaceGetHeightOfPlane(surf, plane));

		if (surface_width != width || surface_height != height)
		{
			device->log("Input surface plane %d is %dx%d, expected %dx%d.",
			            plane, surface_width, surface_height, width, height);
			return false;
		}

		return true;
	};

	if (biplanar)
	{
		if (!chroma_420)
		{
			device->log("Biplanar NV12 input has half resolution chroma, "
			            "so it cannot feed a 4:4:4 encoder.");
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}

		if (IOSurfaceGetPlaneCount(surface(0)) != 2)
		{
			device->log("Expected a biplanar surface in planes[0], but it has %zu planes.",
			            size_t(IOSurfaceGetPlaneCount(surface(0))));
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}

		if (!plane_matches(surface(0), 0, layout.width, layout.height) ||
		    !plane_matches(surface(0), 1, chroma_width, chroma_height))
			return RIPTIDE_ERROR_INVALID_ARGUMENT;

		// P010 style surfaces (e.g. 'x420' and 'xf20') store each sample in 16 bits
		const size_t luma_bytes = IOSurfaceGetBytesPerElementOfPlane(surface(0), 0);
		if (luma_bytes != 1 && luma_bytes != 2)
		{
			device->log("Biplanar input has %zu bytes per luma sample, expected 1 or 2.", luma_bytes);
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}
		const bool is_16bit = luma_bytes == 2;

		wrapped.sampled[0] = wrapped.adopt(wrap_surface_plane(
				device, surface(0), 0, is_16bit ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm,
				layout.width, layout.height, false));

		auto *chroma = wrapped.adopt(wrap_surface_plane(
				device, surface(0), 1, is_16bit ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm,
				chroma_width, chroma_height, true));

		if (!wrapped.sampled[0] || !chroma)
			return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

		if (!make_interleaved_chroma_views(device, wrapped, chroma))
			return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	else
	{
		for (int i = 0; i < NumComponents; i++)
		{
			const int width = i == 0 ? layout.width : chroma_width;
			const int height = i == 0 ? layout.height : chroma_height;

			if (!plane_matches(surface(i), 0, width, height))
				return RIPTIDE_ERROR_INVALID_ARGUMENT;

			wrapped.sampled[i] = wrapped.adopt(wrap_surface_plane(
					device, surface(i), 0, MTLPixelFormatR8Unorm, width, height, false));

			if (!wrapped.sampled[i])
				return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
		}
	}

	return RIPTIDE_SUCCESS;
}

}

//////
// Public API

riptide_result riptide_encoder_create(const riptide_encoder_create_info *info,
                                        riptide_encoder *encoder)
{
	if (!info || !encoder || !info->device)
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


	// Deferred rather than done in riptide_device_create(), so that decode-only
	// users do not pay for six extra shader compiles.
	if (!ensure_encode_pipelines(info->device))
		return RIPTIDE_ERROR_SHADER_COMPILATION;

	auto created = std::unique_ptr<riptide_encoder_opaque>(new (std::nothrow) riptide_encoder_opaque);
	if (!created)
		return RIPTIDE_ERROR_OUT_OF_HOST_MEMORY;

	created->device = info->device;

	if (!created->layout.init(info->width, info->height,
	                          chroma_420 ? ChromaSubsampling::Chroma420 : ChromaSubsampling::Chroma444))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	created->queue = [created->device->mtl newCommandQueue];
	if (!created->queue)
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	if (!created->wavelet.init(created->device, created->layout) ||
	    !create_encode_buffers(created.get()))
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	*encoder = created.release();
	return RIPTIDE_SUCCESS;
}

void riptide_encoder_destroy(riptide_encoder encoder)
{
	delete encoder;
}

riptide_result riptide_encoder_encode_gpu(riptide_encoder encoder,
                                            const riptide_gpu_input *input,
                                            const riptide_rate_control *rate_control)
{
	if (!encoder || !input || !rate_control)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;


	InputTextures wrapped;
	auto result = wrap_gpu_input(encoder, input, wrapped);
	if (result != RIPTIDE_SUCCESS)
		return result;

	// The command buffer retains the wrapping textures, so letting them go here is
	// safe even though the GPU has not run yet.
	return encode_frame(encoder, wrapped.sampled, rate_control);
}

riptide_result riptide_encoder_get_prediction_map(riptide_encoder encoder, uint8_t *map, size_t capacity,
                                                   size_t *size)
{
	if (!encoder || !size)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (!encoder->weights || !encoder->reference[0])
	{
		*size = 0;
		return RIPTIDE_SUCCESS;
	}
	const size_t gw = encoder->weights.width, gh = encoder->weights.height;
	*size = gw * gh;
	if (!map || capacity < gw * gh)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (encoder->pending)
		[encoder->pending waitUntilCompleted];
	[encoder->weights getBytes:map bytesPerRow:gw fromRegion:MTLRegionMake2D(0, 0, gw, gh) mipmapLevel:0];
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_get_motion(riptide_encoder encoder, int16_t *vectors, size_t capacity,
                                            size_t *count)
{
	if (!encoder || !count)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (!encoder->vectors[1] || !encoder->reference[0])
	{
		*count = 0;
		return RIPTIDE_SUCCESS;
	}
	id<MTLTexture> v = encoder->vectors[1];
	*count = v.width * v.height;
	if (!vectors || capacity < 2 * *count)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (encoder->pending)
		[encoder->pending waitUntilCompleted];
	[v getBytes:vectors bytesPerRow:v.width * 4 fromRegion:MTLRegionMake2D(0, 0, v.width, v.height) mipmapLevel:0];
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_set_reference(riptide_encoder encoder, const riptide_gpu_buffers *reference)
{
	return riptide_encoder_set_reference_stripe(encoder, reference, 0);
}

riptide_result riptide_encoder_set_reference_stripe(riptide_encoder encoder, const riptide_gpu_buffers *reference,
                                                      int ref_y)
{
	if (!encoder || ref_y < 0 || (ref_y & 1))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	encoder->ref_y = ref_y;
	// Back to intra: the next inter frame's search starts from zero motion.
	if (!reference && encoder->vectors[1])
		clear_vector_texture(encoder->vectors[1]);
	for (int i = 0; i < NumComponents; i++)
		encoder->reference[i] = reference ? (__bridge id<MTLTexture>)(reference->planes[i]) : nil;
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_encode_cpu(riptide_encoder encoder,
                                            const riptide_cpu_buffer *input,
                                            const riptide_rate_control *rate_control)
{
	if (!encoder || !input || !rate_control)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (input->format != RIPTIDE_CPU_BUFFER_FORMAT_NV12 &&
	    input->format != RIPTIDE_CPU_BUFFER_FORMAT_YUV420P &&
	    input->format != RIPTIDE_CPU_BUFFER_FORMAT_YUV444P)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;


	auto result = upload_cpu_input(encoder, input);
	if (result != RIPTIDE_SUCCESS)
		return result;

	return encode_frame(encoder, encoder->cpu_input.sampled, rate_control);
}

// Inter frames: the frame's inter packets (bitstream/inter.md), built once.
static void build_inter_packets(riptide_encoder encoder)
{
	if (!encoder->frame_predicted || !encoder->inter_sizes.empty())
		return;
	InterGeometry g(encoder->layout.width, encoder->layout.height);
	encoder->inter_map.resize(size_t(g.mw) * g.mh);
	encoder->inter_mv.resize(2 * size_t(g.vw) * g.vh);
	encoder->inter_mv_prev.resize(encoder->inter_mv.size());
	[encoder->weights getBytes:encoder->inter_map.data() bytesPerRow:g.mw
	                fromRegion:MTLRegionMake2D(0, 0, g.mw, g.mh) mipmapLevel:0];
	[encoder->vectors[1] getBytes:encoder->inter_mv.data() bytesPerRow:g.vw * 4
	                   fromRegion:MTLRegionMake2D(0, 0, g.vw, g.vh) mipmapLevel:0];
	[encoder->vectors_prev getBytes:encoder->inter_mv_prev.data() bytesPerRow:g.vw * 4
	                     fromRegion:MTLRegionMake2D(0, 0, g.vw, g.vh) mipmapLevel:0];
	inter_packetize(g, encoder->inter_map.data(), encoder->inter_mv.data(), encoder->inter_mv_prev.data(),
	                encoder->sequence_count, encoder->inter_packet_boundary,
	                encoder->inter_bytes, encoder->inter_sizes);
}

riptide_result riptide_encoder_compute_num_packets_with_padding(riptide_encoder encoder,
                                                                  size_t packet_boundary,
                                                                  size_t padding_size,
                                                                  size_t *num_packets)
{
	if (!encoder || !num_packets)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto result = wait_for_result(encoder);
	if (result != RIPTIDE_SUCCESS)
		return result;

	build_inter_packets(encoder);
	*num_packets = compute_num_packets(encoder->layout, encoder->bitstream_meta.contents,
	                                   packet_boundary, padding_size) + encoder->inter_sizes.size();
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_compute_num_packets(riptide_encoder encoder, size_t packet_boundary,
                                                     size_t *num_packets)
{
	return riptide_encoder_compute_num_packets_with_padding(encoder, packet_boundary, 0, num_packets);
}

riptide_result riptide_encoder_compute_num_critical_packets(riptide_encoder encoder, int bands,
                                                              size_t packet_boundary, size_t padding_size,
                                                              size_t *num_packets)
{
	// Internal assertion.
	if (!encoder || !num_packets || bands < 0 || bands > 4)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto result = wait_for_result(encoder);
	if (result != RIPTIDE_SUCCESS)
		return result;

	*num_packets = compute_num_critical_packets(encoder->layout, bands, encoder->bitstream_meta.contents,
	                                            packet_boundary, padding_size);
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_packetize_with_padding(riptide_encoder encoder, riptide_packet *packets,
                                                        size_t packet_boundary, size_t padding_size,
                                                        size_t *out_packets,
                                                        void *bitstream, size_t size)
{
	if (!encoder || !packets || !out_packets || !bitstream)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto result = wait_for_result(encoder);
	if (result != RIPTIDE_SUCCESS)
		return result;

	// An inter frame's inter packets come first: the decoder can build the
	// prediction from them while the coefficients are still arriving.
	build_inter_packets(encoder);
	const size_t inter = encoder->inter_bytes.size();
	if (inter > size)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	memcpy(bitstream, encoder->inter_bytes.data(), inter);
	size_t offset = 0;
	for (size_t i = 0; i < encoder->inter_sizes.size(); i++)
	{
		packets[i] = {offset, encoder->inter_sizes[i]};
		offset += encoder->inter_sizes[i];
	}
	const size_t n_inter = encoder->inter_sizes.size();

	static_assert(sizeof(riptide_packet) == sizeof(Packet), "riptide_packet layout mismatch.");
	*out_packets = packetize(encoder->layout, reinterpret_cast<Packet *>(packets + n_inter), packet_boundary,
	                         static_cast<uint8_t *>(bitstream) + inter, size - inter,
	                         encoder->bitstream_meta.contents, encoder->bitstream.contents,
	                         padding_size);
	for (size_t i = 0; i < *out_packets; i++)
		packets[n_inter + i].offset += inter;
	*out_packets += n_inter;
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_packetize(riptide_encoder encoder, riptide_packet *packets,
                                           size_t packet_boundary, size_t *out_packets,
                                           void *bitstream, size_t size)
{
	return riptide_encoder_packetize_with_padding(encoder, packets, packet_boundary, 0,
	                                               out_packets, bitstream, size);
}

riptide_result riptide_encoder_set_inter_packet_boundary(riptide_encoder encoder, size_t packet_boundary)
{
	if (!encoder || packet_boundary < 128 || packet_boundary > 16384 * 4)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	encoder->inter_packet_boundary = packet_boundary;
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_set_motion_state(riptide_encoder encoder, const int16_t *vectors, size_t count)
{
	if (!encoder || !vectors)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	InterGeometry g(encoder->layout.width, encoder->layout.height);
	if (count != size_t(g.vw) * g.vh)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (!encoder->vectors[1])
	{
		encoder->vectors_prev = create_vector_texture(encoder->device, encoder->layout.width, encoder->layout.height);
		encoder->vectors[0] = create_vector_texture(encoder->device, encoder->layout.width, encoder->layout.height);
		encoder->vectors[1] = create_vector_texture(encoder->device, encoder->layout.width, encoder->layout.height);
	}
	if (encoder->pending)
		[encoder->pending waitUntilCompleted];
	// The next encode takes vectors[1] as its previous field.
	[encoder->vectors[1] replaceRegion:MTLRegionMake2D(0, 0, g.vw, g.vh) mipmapLevel:0
	                         withBytes:vectors bytesPerRow:g.vw * 4];
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_get_mapped_raw_bitstream(riptide_encoder encoder,
                                                          const void **mapped_bitstream,
                                                          size_t *mapped_bitstream_size,
                                                          const void **mapped_metadata,
                                                          size_t *mapped_metadata_size)
{
	if (!encoder || !mapped_bitstream || !mapped_bitstream_size ||
	    !mapped_metadata || !mapped_metadata_size)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto result = wait_for_result(encoder);
	if (result != RIPTIDE_SUCCESS)
		return result;

	// Both are MTLStorageModeShared, so this is just the existing CPU pointer.
	*mapped_bitstream = encoder->bitstream.contents;
	*mapped_bitstream_size = encoder->bitstream.length;
	*mapped_metadata = encoder->bitstream_meta.contents;
	*mapped_metadata_size = encoder->bitstream_meta.length;
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_get_num_active_blocks(riptide_encoder encoder, int bands,
                                                       size_t *num_active_blocks)
{
	if (!encoder || !num_active_blocks || bands < 0 || bands > 4)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	*num_active_blocks = get_num_active_blocks(encoder->layout, bands);
	return RIPTIDE_SUCCESS;
}

riptide_result riptide_encoder_compute_block_active_words(riptide_encoder encoder, int bands,
                                                            uint32_t *words, size_t word_count)
{
	// Internal assertion.
	if (!encoder || !words || bands < 0 || bands > 4)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	// An undersized word_count is only caught by an assert in compute_block_active_words(),
	// matching the Vulkan C API. The loop there runs to get_num_active_blocks(bands)
	// regardless, so with NDEBUG this writes past the end of `words`. The caller is
	// responsible for the size, as documented in riptide_metal.h.
	auto result = wait_for_result(encoder);
	if (result != RIPTIDE_SUCCESS)
		return result;

	compute_block_active_words(encoder->layout, bands, words, word_count,
	                           encoder->bitstream_meta.contents);
	return RIPTIDE_SUCCESS;
}

#ifdef RIPTIDE_METAL_BENCH_HOOKS
// Not in riptide_metal.h: the encoder owns its command buffer and deliberately does not
// hand it out, so a caller cannot time the GPU work itself. tools/metal/bench_encode.mm
// declares this extern directly.
extern "C" double riptide_bench_last_gpu_ms(riptide_encoder encoder)
{
	return encoder ? encoder->bench_last_gpu_ms : -1.0;
}
#endif

// For tools/rtrd's exactness check only, not exported: the prediction plane the
// last inter frame's forward transform subtracted.
id<MTLTexture> riptide_encoder_debug_prediction(riptide_encoder encoder, int component)
{
	return encoder->prediction[component];
}
