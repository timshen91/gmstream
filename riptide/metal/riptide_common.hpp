// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#pragma once

// Internal glue shared by the Metal encoder and decoder: the object behind
// riptide_device, the library/pipeline helpers, and the wavelet coefficient
// pyramid, which both directions allocate identically.
//
// This is not a public header, and it is Objective-C++: the backends are .mm so they
// can use Metal directly while keeping the C++ bitstream layer. Everything here is
// under ARC, so the id<MTL...> members below are strong references and nothing needs
// explicit retain or release.

#import <Metal/Metal.h>

#include "riptide_metal.h"
#include "riptide_bitstream.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

namespace Riptide
{
// Threadgroup sizes are baked into the shaders as local_size_x, except for
// resolve_rate_control, which declares it as a specialization constant.
constexpr uint32_t DequantThreadgroupSize = 128;
constexpr uint32_t IdwtThreadgroupSize = 64;
constexpr uint32_t DwtThreadgroupSize = 64;
constexpr uint32_t QuantThreadgroupSize = 128;
constexpr uint32_t AnalyzeThreadgroupSize = 64;
constexpr uint32_t AnalyzeFinalizeThreadgroupSize = 512;
constexpr uint32_t BlockPackingThreadgroupSize = 64;
// resolve_rate_control needs exactly one SIMD group per threadgroup, and every
// Apple GPU we support is 32 wide.
constexpr uint32_t ResolveThreadgroupSize = 32;

// Same meaning and same env var as the Vulkan build:
//   2  FP32 storage and math
//   1  (default) FP16 storage, FP32 lifting math, half sized threadgroup array
//   0  FP16 throughout
//
// 1 is the default because it is both the fastest and essentially free: on an
// M1 it decodes 1080p roughly 1.4x quicker than FP32 while staying within 1 LSB
// of the Vulkan decoder. The win is the halved memory traffic through the R16F
// wavelet pyramid, which is what the decode is bound by above 480p -- 0 is
// actually slower than 1 despite the half precision arithmetic, and costs
// accuracy, so it is not recommended.
//
// Note 1 is not quite identical to the Vulkan build's: that one additionally
// splits the low frequency bands into a separate FP32 image, which is not
// implemented here.
constexpr int DefaultPrecision = 1;

int requested_precision();
MTLPixelFormat wavelet_format(int precision);

const char *result_string(riptide_result result);

id<MTLLibrary> compile_library(riptide_device device, const char *source, const char *label);

// `constants` may be nil when the shader has no specialization constants to set.
id<MTLComputePipelineState> create_pipeline(riptide_device device, id<MTLLibrary> library,
                                           const char *entry_point, uint32_t required_threads,
                                           MTLFunctionConstantValues *constants = nil);

// Convenience for a single bool function constant. Note Metal refuses to build a
// pipeline from an unspecialized function if the shader declares any function
// constants at all, even ones SPIRV-Cross guarded with
// is_function_constant_defined(), so constants that want their default value
// still have to be set explicitly.
id<MTLComputePipelineState> create_pipeline_bool_constant(riptide_device device, id<MTLLibrary> library,
                                                          const char *entry_point,
                                                          uint32_t required_threads,
                                                          uint32_t index, bool value);

// Inter frames: one prediction weight per 32x32 luma block (R8, shared, so the
// CPU can carry it from encoder to decoder). Initialized to 1: predict everywhere.
id<MTLTexture> create_weight_texture(riptide_device device, int width, int height);
// Inter frames: one quarter pel vector per 16x16 luma block (RG16 signed,
// shared), zero.
id<MTLTexture> create_vector_texture(riptide_device device, int width, int height);
void clear_vector_texture(id<MTLTexture> texture);

// Inter frames: writes the prediction planes (width x height luma, 4:2:0) from
// the reference planes and the block weights (inter_predict.comp), allocating
// them on first use. The image may be a stripe of the reference's frame, from
// its luma row ref_y. Encoder and decoder run the same pass, so they subtract
// and add the same values.
void dispatch_inter_predict(riptide_device device, id<MTLComputeCommandEncoder> enc,
                            id<MTLTexture> const reference[3], int ref_y, int width, int height,
                            id<MTLTexture> weights, id<MTLTexture> vectors,
                            __strong id<MTLTexture> prediction[3]);

// The (i)dwt variants: 0 plain, 1 DCShift, 2 Predict (function constants 0 and 2).
id<MTLComputePipelineState> create_dwt_pipeline(riptide_device device, id<MTLLibrary> library,
                                                const char *entry_point, uint32_t required_threads,
                                                int variant);

class DeviceTimestamps;

// The timestamped passes of one command buffer.
struct TimestampBatch : std::enable_shared_from_this<TimestampBatch>
{
	explicit TimestampBatch(std::shared_ptr<DeviceTimestamps> owner_);

	// A two sample buffer to attach to a pass descriptor, or nil if none could be
	// allocated, leaving that pass untimed. `tag` must outlive the batch.
	id<MTLCounterSampleBuffer> add_pass(const char *tag);

	// Resolves the passes added above once `cmd` completes. Call before commit.
	void submit(id<MTLCommandBuffer> cmd);
	void resolve();

	struct Pass
	{
		id<MTLCounterSampleBuffer> buffer;
		const char *tag;
	};

	std::vector<Pass> passes;
	std::shared_ptr<DeviceTimestamps> owner;

	// Start of the CPU/GPU correlation window; resolve() takes the end pair.
	MTLTimestamp cpu_begin = 0;
	MTLTimestamp gpu_begin = 0;
};

// GPU timing behind riptide_device_report_performance_stats(). Apple GPUs sample
// the timestamp counter at pass boundaries only, so the finest granularity is one
// interval per compute or blit pass.
//
// Held by shared_ptr by the device and by every in-flight batch, so a command
// buffer landing after riptide_device_destroy() still resolves into a live object.
class DeviceTimestamps : public std::enable_shared_from_this<DeviceTimestamps>
{
public:
	explicit DeviceTimestamps(id<MTLDevice> mtl_);

	bool counters_supported() const { return counter_set != nil; }

	// Off unless RIPTIDE_TIMESTAMPS is set or report() has been called once.
	bool collecting() const { return enabled.load(std::memory_order_relaxed); }

	// Null when collection is off or pass timestamps are unavailable, which call
	// sites read as "do not instrument".
	std::shared_ptr<TimestampBatch> begin_batch();

	// Thread safe: completion handlers run on Metal's own threads.
	void accumulate(const char *tag, double seconds);
	void report(riptide_message_cb cb, void *userdata, bool reset);

	// For TimestampBatch.
	id<MTLCounterSampleBuffer> acquire_sample_buffer();
	void recycle_sample_buffer(id<MTLCounterSampleBuffer> buffer);
	void sample_correlation(MTLTimestamp *cpu, MTLTimestamp *gpu) const;

private:
	id<MTLDevice> mtl;
	// nil when the GPU cannot sample at pass boundaries.
	id<MTLCounterSet> counter_set;
	std::atomic<bool> enabled;

	std::mutex lock;
	// Recycled, so a steady stream settles on as many buffers as it keeps in flight.
	std::vector<id<MTLCounterSampleBuffer>> pool;
	struct Entry
	{
		double total_time = 0.0;
		uint64_t iterations = 0;
	};
	// Ordered, so the report comes out the same way every time.
	std::map<std::string, Entry> entries;
};

// Samples GPU timestamps around the pass when `batch` is non-null, otherwise an
// ordinary untimed pass.
id<MTLComputeCommandEncoder> begin_compute_pass(id<MTLCommandBuffer> cmd, MTLDispatchType dispatch_type,
                                                TimestampBatch *batch, const char *tag);
id<MTLBlitCommandEncoder> begin_blit_pass(id<MTLCommandBuffer> cmd, TimestampBatch *batch, const char *tag);

// The wavelet coefficient pyramid. The decoder fills it from the bitstream and
// runs the iDWT out of it; the encoder runs the DWT into it and quantizes out of
// it. Both want exactly the same texture and the same set of views, so this is
// shared verbatim.
struct WaveletPyramid
{
	// 2D array, NumFrequencyBandsPerLevel * NumComponents layers, DecompositionLevels mips.
	id<MTLTexture> texture;
	// 4 layer array views, one per component and level. Written as an image and
	// sampled as an array.
	id<MTLTexture> component_layer_views[NumComponents][DecompositionLevels];
	// Single layer 2D views of band 0 (LL).
	id<MTLTexture> component_ll_views[NumComponents][DecompositionLevels];

	bool init(riptide_device device, const BlockLayout &layout);

	WaveletPyramid() = default;
	WaveletPyramid(const WaveletPyramid &) = delete;
	void operator=(const WaveletPyramid &) = delete;
};
}

struct riptide_device_opaque
{
	id<MTLDevice> mtl;

	// Decode. Compiled by riptide_device_create().
	id<MTLComputePipelineState> dequant_pipeline;
	// Indexed by the DCShift function constant; [2] adds a reference plane
	// instead (Predict, inter frames).
	id<MTLComputePipelineState> idwt_pipeline[3];
	id<MTLComputePipelineState> predict_pipeline; // inter frames: the prediction planes

	// Encode. Compiled on demand by the first riptide_encoder_create(), so that
	// decode-only users do not pay for six extra shader compiles.
	id<MTLComputePipelineState> dwt_pipeline[3]; // as idwt_pipeline
	id<MTLComputePipelineState> decide_pipeline; // inter frames: per block prediction weight
	id<MTLComputePipelineState> search_pipeline; // inter frames: one motion search pass
	id<MTLComputePipelineState> side_cost_pipeline; // inter frames: side information charged to the budget
	id<MTLComputePipelineState> frame_mse_pipeline; // inter frames: last frame's reconstruction error
	id<MTLComputePipelineState> vector_rd_pipeline; // inter frames: whether each tile's vectors pay
	id<MTLComputePipelineState> quant_pipeline;
	id<MTLComputePipelineState> analyze_pipeline;
	id<MTLComputePipelineState> analyze_finalize_pipeline;
	id<MTLComputePipelineState> resolve_pipeline;
	id<MTLComputePipelineState> block_packing_pipeline;
	bool encode_pipelines_ready = false;
	std::mutex encode_pipeline_lock;

	id<MTLSamplerState> mirror_repeat_sampler;
	id<MTLSamplerState> linear_mirror_sampler; // the same, bilinear: the motion search's reference reads
	// Clamp to transparent black. Only the encoder's quantizer wants this, so it
	// is created together with the encode pipelines.
	id<MTLSamplerState> border_sampler;

	// Always created, but idle until collection is switched on.
	std::shared_ptr<Riptide::DeviceTimestamps> timestamps;

	riptide_message_cb message_cb = nullptr;
	void *message_userdata = nullptr;
	int precision = Riptide::DefaultPrecision;

	void log(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));
};
