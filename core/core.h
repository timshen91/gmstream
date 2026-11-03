// Internal interfaces between the parts of the C core.
#ifndef GM_CORE_H
#define GM_CORE_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>

#include "gm.h"

/* encode.c: GPU copy + VA-API encode; runs its own worker thread. */
struct gm_encoder;
struct gm_encoder *encoder_create(int drm_fd, const struct gm_callbacks *cb);
void encoder_destroy(struct gm_encoder *enc);
uint32_t encoder_supported_codecs(struct gm_encoder *enc);
void encoder_start(struct gm_encoder *enc, enum gm_codec codec,
	int bitrate_kbps, int refresh_mhz, int packet_bytes, int flags);
void encoder_stop(struct gm_encoder *enc);
void encoder_set_bitrate(struct gm_encoder *enc, int bitrate_kbps);
void encoder_set_refresh(struct gm_encoder *enc, int refresh_mhz);
void encoder_request_keyframe(struct gm_encoder *enc);
// Copies `buffer` (gamescope's composited frame) into a width x height
// encoder surface on the GPU, scaling if needed (a single-pixel fill), and
// queues it for encoding. Compositor thread only.
void encoder_submit(struct gm_encoder *enc, struct wlr_buffer *buffer,
	int width, int height, uint64_t slot_ns);
// Per-second counters for -S, reset on read.
void encoder_take_stats(struct gm_encoder *enc, unsigned *encoded,
	unsigned *dropped, double *avg_encode_ms);
// The streamed-in-pieces counters of whichever encoder is running, read right
// after encoder_take_stats.
void encoder_take_chunk_stats(struct gm_encoder *enc, unsigned *frames,
	unsigned *chunks, unsigned *patched, double *first_ms, double *last_ms);

/* pyrowave.c: PyroWave encode straight from gamescope's dma-buf; its own
 * worker thread. NULL from create when there is no usable Vulkan device. */
struct gm_pyrowave;
struct gm_pyrowave *pyrowave_create(const struct gm_callbacks *cb);
void pyrowave_destroy(struct gm_pyrowave *pw);
void pyrowave_start(struct gm_pyrowave *pw, int bitrate_kbps, int refresh_mhz,
	int packet_bytes);
void pyrowave_stop(struct gm_pyrowave *pw);
void pyrowave_set_rate(struct gm_pyrowave *pw, int bitrate_kbps, int refresh_mhz);
// Compositor thread. False if the buffer cannot be encoded (not a dma-buf).
bool pyrowave_submit(struct gm_pyrowave *pw, struct wlr_buffer *buffer,
	int width, int height, uint64_t slot_ns);
void pyrowave_take_stats(struct gm_pyrowave *pw, unsigned *encoded,
	unsigned *dropped, uint64_t *encode_ns);

/* vkencode.c: AV1 on the Vulkan video encode queue, straight from gamescope's
 * dma-buf, each piece of the bitstream sent as the GPU writes it; its own
 * worker thread. NULL from create when the GPU has no AV1 encode queue (or
 * GM_AV1_VAAPI is set, which keeps the VA-API path as the reference). */
struct gm_vkencode;
struct gm_vkencode *vkencode_create(const struct gm_callbacks *cb);
void vkencode_destroy(struct gm_vkencode *vk);
void vkencode_start(struct gm_vkencode *vk, int bitrate_kbps, int refresh_mhz,
	int packet_bytes, int flags);
void vkencode_stop(struct gm_vkencode *vk);
void vkencode_set_rate(struct gm_vkencode *vk, int bitrate_kbps, int refresh_mhz);
void vkencode_request_keyframe(struct gm_vkencode *vk);
// Compositor thread. False if the buffer cannot be encoded (not a dma-buf).
bool vkencode_submit(struct gm_vkencode *vk, struct wlr_buffer *buffer,
	int width, int height, uint64_t slot_ns);
void vkencode_take_stats(struct gm_vkencode *vk, unsigned *encoded,
	unsigned *dropped, uint64_t *encode_ns, unsigned *chunks,
	unsigned *patched, double *first_ms);

/* vastream.c: sends a frame's bitstream out of the VA-API coded buffer while
 * the GPU is still writing it. Encoder thread only; it wraps libva's calls
 * (build.rs passes --wrap), so it also runs inside FFmpeg's own wait. */
void vastream_configure(const struct gm_callbacks *cb, bool on,
	unsigned chunk_bytes);
bool vastream_enabled(void);
void vastream_begin(uint32_t frame_id, uint64_t slot_ns, uint16_t width,
	uint16_t height, enum gm_codec codec);
// The encoder's own bitstream, once FFmpeg has it: sends the few bytes the GPU
// went back and changed, and the held-back start of the frame, which carries
// whether the frame turned out to be a keyframe. False if the frame was not
// streamed and must be sent whole.
bool vastream_end(const uint8_t *data, size_t len, uint64_t encoded_ns,
	bool keyframe);
// Per-second counters for -S, reset on read.
void vastream_take_stats(unsigned *frames, unsigned *chunks, unsigned *patched,
	double *first_ms, double *last_ms);

/* audio.c: PipeWire capture of the default sink → Opus. */
struct gm_audio;
struct gm_audio *audio_create(const struct gm_callbacks *cb);
void audio_destroy(struct gm_audio *audio);
void audio_start(struct gm_audio *audio);
void audio_stop(struct gm_audio *audio);

/* gamepad.c: uinput Xbox 360 controllers. */
struct gm_gamepads;
struct gm_gamepads *gamepads_create(struct wl_event_loop *loop,
	const struct gm_callbacks *cb);
void gamepads_destroy(struct gm_gamepads *pads);
void gamepads_update(struct gm_gamepads *pads, uint8_t pad,
	const struct gm_gamepad_state *state);
void gamepads_remove(struct gm_gamepads *pads, uint8_t pad);
void gamepads_remove_all(struct gm_gamepads *pads);

#endif
