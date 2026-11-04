// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT

#ifndef RIPTIDE_METAL_H_
#define RIPTIDE_METAL_H_

// Metal encoder and decoder for Riptide, targeting Apple Silicon (Apple7 GPU family
// and up) on macOS and iOS.
//
// Metal objects cross the ABI as void *. This keeps the header usable from plain C,
// Objective-C and C++ alike, and sidesteps ARC ownership ambiguity at the boundary:
//   - Objective-C / ARC: pass (__bridge void *)mtlObject
//   - Objective-C, no ARC, or plain C: pass the object pointer directly
// Unless stated otherwise, objects passed in are retained for as long as Riptide
// needs them and released on destroy, so the caller may release its own reference.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#define RIPTIDE_API_VERSION_MAJOR 1
#define RIPTIDE_API_VERSION_MINOR 0
#define RIPTIDE_API_VERSION_PATCH 0

#if !defined(RIPTIDE_PUBLIC_API)
#if defined(RIPTIDE_EXPORT_SYMBOLS)
#if defined(__GNUC__)
#define RIPTIDE_PUBLIC_API __attribute__((visibility("default")))
#else
#define RIPTIDE_PUBLIC_API
#endif
#else
#define RIPTIDE_PUBLIC_API
#endif
#endif

// Codes 0 and -1 through -4 carry the same meaning as in riptide.h.
typedef enum riptide_result
{
	RIPTIDE_SUCCESS = 0,
	RIPTIDE_ERROR_GENERIC = -1,
	RIPTIDE_ERROR_INVALID_ARGUMENT = -2,
	RIPTIDE_ERROR_OUT_OF_HOST_MEMORY = -3,
	RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY = -4,
	RIPTIDE_ERROR_UNSUPPORTED_DEVICE = -5,
	RIPTIDE_ERROR_SHADER_COMPILATION = -6,
	RIPTIDE_ERROR_CORRUPT_BITSTREAM = -7,
	RIPTIDE_ERROR_INT_MAX = 0x7fffffff
} riptide_result;

typedef enum riptide_chroma_subsampling
{
	RIPTIDE_CHROMA_SUBSAMPLING_420 = 0,
	RIPTIDE_CHROMA_SUBSAMPLING_444 = 1,
	RIPTIDE_CHROMA_SUBSAMPLING_INT_MAX = 0x7fffffff
} riptide_chroma_subsampling;

// Opaque Metal handles. See the note at the top of this header on bridging.
typedef void *riptide_mtl_device;         // id<MTLDevice>
typedef void *riptide_mtl_command_buffer; // id<MTLCommandBuffer>
typedef void *riptide_mtl_texture;        // id<MTLTexture>
typedef void *riptide_iosurface;          // IOSurfaceRef

typedef struct riptide_device_opaque *riptide_device;
typedef struct riptide_decoder_opaque *riptide_decoder;
typedef struct riptide_encoder_opaque *riptide_encoder;

typedef void (*riptide_message_cb)(void *userdata, const char *msg);

// Used to dynamically detect any API/ABI incompatibility. This entry point is stable.
RIPTIDE_PUBLIC_API void
riptide_get_api_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

// Returns a human readable string for a result code. Never returns NULL.
RIPTIDE_PUBLIC_API const char *
riptide_result_to_string(riptide_result result);

// Device API.

RIPTIDE_PUBLIC_API riptide_result riptide_create_default_device(riptide_device *device);

typedef struct riptide_device_create_info
{
	// The id<MTLDevice> to decode on, or NULL to use the system default device.
	riptide_mtl_device mtl_device;

	// Optional message callback, or NULL to print to stderr.
	riptide_message_cb message_callback;
	void *message_userdata;
} riptide_device_create_info;

// Reports whether an id<MTLDevice> can run the Riptide decoder. Requires a
// 32-wide SIMD group, threadgroups of at least 128 threads and the Apple7 family
// feature set. Intel and AMD Macs are not supported.
RIPTIDE_PUBLIC_API bool
riptide_device_is_supported(riptide_mtl_device mtl_device);

// Creates the shared device object. This compiles the decode pipelines, so it is
// comparatively expensive; create one and share it across decoders.
//
// The RIPTIDE_PRECISION environment variable selects the wavelet precision, as
// on the Vulkan side: 2 is FP32, 1 (the default) keeps FP32 lifting math but
// stores the wavelet pyramid as FP16, and 0 is FP16 throughout. 1 is both the
// fastest and within 1 LSB of the FP32 result; 0 is slower than 1 and less
// accurate, so it exists mainly for comparison.
RIPTIDE_PUBLIC_API riptide_result
riptide_device_create(const riptide_device_create_info *info, riptide_device *device);

// All decoders created from this device must be destroyed first.
RIPTIDE_PUBLIC_API void
riptide_device_destroy(riptide_device device);

// For performance debugging, reports GPU timestamps, then Metal's memory counters.
// Timestamps are per pass rather than per dispatch, since Apple GPUs only sample
// the counter at pass boundaries. Collection is off until RIPTIDE_TIMESTAMPS is
// set or this is called once, so the first call may have nothing to report yet.
// cb may be NULL, in which case the device's message callback is used.
RIPTIDE_PUBLIC_API void
riptide_device_report_performance_stats(riptide_device device, riptide_message_cb cb, void *userdata, bool reset);

// Encoder API
typedef struct riptide_encoder_create_info
{
	riptide_device device;

	// Luma dimensions. For 420 subsampling both must be even.
	// Both must be in the range [1, 16384], as the bitstream encodes them in 14 bits.
	int width;
	int height;

	riptide_chroma_subsampling chroma;
} riptide_encoder_create_info;

typedef struct riptide_packet
{
	size_t offset;
	size_t size;
} riptide_packet;

typedef struct riptide_rate_control
{
	// Very basic, target bitstream for an image must not exceed this size.
	size_t maximum_bitstream_size;
} riptide_rate_control;

typedef enum riptide_cpu_buffer_format
{
	RIPTIDE_CPU_BUFFER_FORMAT_NV12 = 0,    // 2 planes. Y in 8bpp, then CbCr interleaved in 16bpp.
	RIPTIDE_CPU_BUFFER_FORMAT_YUV420P = 1, // 3 planes, half resolution chroma.
	RIPTIDE_CPU_BUFFER_FORMAT_YUV444P = 2, // 3 planes, full resolution chroma.
	RIPTIDE_CPU_BUFFER_FORMAT_INT_MAX = 0x7fffffff
} riptide_cpu_buffer_format;

typedef struct riptide_cpu_buffer
{
	// Written in decoder, read-only in encoder.
	void *data[3];
	// Must be at least width for plane times texel size of the plane.
	size_t row_stride_in_bytes[3];
	// Must be at least row_stride times height of plane.
	size_t plane_size_in_bytes[3];
	// Size of the luma plane. Size of chroma is implied by format.
	// Must be same extent as decoder.
	int width;
	int height;
	riptide_cpu_buffer_format format;
} riptide_cpu_buffer;

// IOSurface input for the encoder. Two layouts are accepted:
//   - one biplanar NV12 or P010 surface in planes[0], with planes[1] and planes[2] NULL.
//     P010 samples are read as 16-bit UNORM, so the 10-bit values are slightly scaled.
//   - three single plane R8 surfaces, one per YUV component
// The library wraps these in MTLTextures itself, so the caller does not have to
// match any particular pixel format or usage. For the NV12 case the chroma plane
// is bound twice with different channel swizzles rather than being deinterleaved,
// so neither layout costs a copy.
typedef struct riptide_gpu_input
{
	riptide_iosurface planes[3];
} riptide_gpu_input;

RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_create(const riptide_encoder_create_info *info, riptide_encoder *encoder);

RIPTIDE_PUBLIC_API void
riptide_encoder_destroy(riptide_encoder encoder);

// Both encode entry points submit GPU work and return without blocking; the
// subsequent packet queries block until it has finished. Encoding again clobbers
// the previous frame's result. The bitstream carries a small sequence counter so
// the decoder can track frame ordering.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_encode_gpu(riptide_encoder encoder,
                            const riptide_gpu_input *input,
                            const riptide_rate_control *rate_control);

RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_encode_cpu(riptide_encoder encoder,
                            const riptide_cpu_buffer *input,
                            const riptide_rate_control *rate_control);

// Inter frames (bitstream/inter.md): later encodes predict the input from these
// planes, the previous decoded frame, and code the difference. The decoder must
// be given the same planes (riptide_decoder_set_reference) for the same
// frames. NULL returns to intra coding. The textures must stay alive and
// unmodified until the encode or decode that uses them completes. An inter
// frame's packets start with its inter packets (the prediction map and motion
// vectors), which the decoder needs before the rest.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_set_reference(riptide_encoder encoder, const struct riptide_gpu_buffers *reference);
// A frame coded in stripes (horizontal bands, each an image of its own, an
// encoder each): the whole previous decoded frame, and this encoder's stripe's
// first row in it (even); see riptide_decoder_set_reference_stripe.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_set_reference_stripe(riptide_encoder encoder, const struct riptide_gpu_buffers *reference, int ref_y);

// Inter frames: the largest inter packet, in bytes (default 1100). Takes effect
// from the next encode, whose budget includes its inter packets.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_set_inter_packet_boundary(riptide_encoder encoder, size_t packet_boundary);

// Inter frames: the vectors the next frame's are coded against, one quarter pel
// (x, y) per 16x16 luma block, ceil(w / 16) x ceil(h / 16) of them, row major.
// Normally the encoder's own last frame's; set it to a decoder's
// (riptide_decoder_get_motion_state) to follow that decoder, e.g. after
// replaying a client's lossy decode.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_set_motion_state(riptide_encoder encoder, const int16_t *vectors, size_t count);

// For tools: the encoded frame's prediction weight per 32x32 luma block
// (ceil(w / 32) x ceil(h / 32) bytes, row major; 0 intra, 255 predict), and its
// vectors (as above). 0 for an intra frame.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_get_prediction_map(riptide_encoder encoder, uint8_t *map, size_t capacity, size_t *size);
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_get_motion(riptide_encoder encoder, int16_t *vectors, size_t capacity, size_t *count);

// Only valid after a successful encode, and only for that frame. Reports how many
// packets the frame needs if each may carry at most packet_boundary bytes.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_compute_num_packets(riptide_encoder encoder, size_t packet_boundary,
                                     size_t *num_packets);
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_compute_num_packets_with_padding(
		riptide_encoder encoder, size_t packet_boundary, size_t padding_size, size_t *num_packets);
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_compute_num_critical_packets(
		riptide_encoder encoder, int bands, size_t packet_boundary, size_t padding_size, size_t *num_packets);

// `packets` must have room for at least the count reported above; the number
// actually written is at most that and is returned in out_packets.
//
// Note packet_boundary is not a hard cap. A coded 32x32 block is the smallest unit
// a packet can carry, so a block that is on its own larger than packet_boundary
// yields one oversized packet rather than being split. Raising the rate control
// target makes individual blocks larger, so a caller sizing packets to an MTU
// should check the sizes it gets back rather than assume they fit.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_packetize(riptide_encoder encoder, riptide_packet *packets,
                           size_t packet_boundary, size_t *out_packets,
                           void *bitstream, size_t size);
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_packetize_with_padding(
		riptide_encoder encoder, riptide_packet *packets, size_t packet_boundary, size_t padding_size,
		size_t *out_packets, void *bitstream, size_t size);

// Special purpose for manual packetization.
// Client is expected to understand the riptide bitstream format.
// Don't use if you don't know what you're doing.
RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_get_mapped_raw_bitstream(
		riptide_encoder encoder, const void **mapped_bitstream, size_t *mapped_bitstream_size,
		const void **mapped_metadata, size_t *mapped_metadata_size);

RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_get_num_active_blocks(riptide_encoder encoder, int bands, size_t *num_active_blocks);

RIPTIDE_PUBLIC_API riptide_result
riptide_encoder_compute_block_active_words(riptide_encoder encoder,
		int bands, uint32_t *words, size_t word_count);

// Decoder API
typedef struct riptide_decoder_create_info
{
	riptide_device device;

	// Luma dimensions. For 420 subsampling both must be even.
	// Both must be in the range [1, 16384], as the bitstream encodes them in 14 bits.
	int width;
	int height;

	riptide_chroma_subsampling chroma;
} riptide_decoder_create_info;

RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_create(const riptide_decoder_create_info *info, riptide_decoder *decoder);

RIPTIDE_PUBLIC_API void
riptide_decoder_destroy(riptide_decoder decoder);

// Throws away all queued packets.
RIPTIDE_PUBLIC_API void
riptide_decoder_clear(riptide_decoder decoder);

// A frame is potentially split into multiple packets.
// If a packet is pushed for a frame that is deemed to arrive earlier, it is dropped.
// A packet pushed for a frame with a higher sequence clears the queued frame and starts a new one.
// Packets are pushed until riptide_decoder_decode_is_ready() says the frame is ready.
// Returns RIPTIDE_ERROR_CORRUPT_BITSTREAM if the data does not parse.
RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_push_packet(riptide_decoder decoder, const void *data, size_t size);

// For error correction purposes, it may be okay to decode a frame which dropped some packets.
RIPTIDE_PUBLIC_API bool
riptide_decoder_decode_is_ready(riptide_decoder decoder, bool allow_partial_frame);

RIPTIDE_PUBLIC_API bool
riptide_decoder_decode_is_ready_with_sideband(riptide_decoder decoder, bool allow_partial_frame,
		int num_pristine_bands, float minimum_packet_ratio,
		const uint32_t *active_block_mask, size_t word_count);

// Y, Cb, Cr as three separate single-channel textures.
//
// Requirements on each texture:
//   - MTLTextureType2D, created with MTLTextureUsageShaderWrite
//   - a single-channel writable format (r8Unorm, r16Unorm and r32Float are the
//     expected choices; the shader writes normalized float)
//   - plane 0 is width x height; for 420 chroma, planes 1 and 2 are
//     (width / 2) x (height / 2), and for 444 they match plane 0
//
// NV12 output is not yet supported.
typedef struct riptide_gpu_buffers
{
	riptide_mtl_texture planes[3];
} riptide_gpu_buffers;

// Encodes the decode work into the supplied id<MTLCommandBuffer>. Nothing is
// committed; the caller owns submission, and may encode MTLSharedEvent waits or
// signals on the same command buffer to synchronize with other work.
//
// Decoding may be requested at any time, producing incomplete results if packets
// are missing; missing wavelet coefficients are treated as 0, which shows up as
// extra blurring. Use riptide_decoder_decode_is_ready() to tell whether the
// result is known to be complete.
//
// The plane textures are written by compute shaders. Their prior contents are not
// read, so no pre-decode synchronization on them is needed beyond ordinary Metal
// command buffer ordering. Work encoded later in the same command buffer observes
// the results without explicit barriers; across command buffers, the usual Metal
// ordering guarantees on a single MTLCommandQueue apply.
//
// Riptide holds a reference to the plane textures only for the duration of this
// call; the command buffer retains them thereafter as Metal normally does.
// Inter frames: the planes the next decodes predict from, the previous decoded
// frame (see riptide_encoder_set_reference). NULL: intra frames. Decode every
// frame of an inter sequence in order, lost packets or not: a frame none of
// whose packets arrived is decoded after riptide_decoder_clear(), from last
// frame's vectors (bitstream/inter.md).
RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_set_reference(riptide_decoder decoder, const riptide_gpu_buffers *reference);

// Inter frames, a frame coded in stripes (horizontal bands, each coded as an
// image of its own, a decoder each): as riptide_decoder_set_reference, the
// whole previous decoded frame, and this decoder's stripe's first row in it
// (even). A stripe predicts from anywhere in the reference.
RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_set_reference_stripe(riptide_decoder decoder, const riptide_gpu_buffers *reference, int ref_y);

// As riptide_decoder_decode_gpu_buffer, but into rows y on (even) of a frame's
// planes, as wide as this decoder's image and at least y rows taller: a
// stripe's decode straight into its frame. y < 0: the planes are exactly this
// image's size, as riptide_decoder_decode_gpu_buffer.
RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_decode_gpu_buffer_rows(riptide_decoder decoder, riptide_mtl_command_buffer command_buffer,
                                        const riptide_gpu_buffers *buffers, int y);

// Inter frames: the vectors the next frame's are coded against (after an intra
// frame, zero), as riptide_encoder_set_motion_state describes. Get after a
// decode; set before one, to start from a state saved earlier.
RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_get_motion_state(riptide_decoder decoder, int16_t *vectors, size_t count);
RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_set_motion_state(riptide_decoder decoder, const int16_t *vectors, size_t count);

// Inter frames: forms the prediction now, into command_buffer, from the inter
// packets pushed so far (call once a frame's inter packets are all in): the GPU
// builds it while the rest of the frame arrives, and the decode then only adds
// the residual. Optional; without it the decode forms the prediction itself.
RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_prepare_prediction(riptide_decoder decoder, riptide_mtl_command_buffer command_buffer);

RIPTIDE_PUBLIC_API riptide_result
riptide_decoder_decode_gpu_buffer(riptide_decoder decoder,
                                   riptide_mtl_command_buffer command_buffer,
                                   const riptide_gpu_buffers *buffers);

#ifdef __cplusplus
}
#endif

#endif
