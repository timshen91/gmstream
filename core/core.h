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
	int bitrate_kbps, int refresh_mhz);
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

/* pyrowave.c: PyroWave encode straight from gamescope's dma-buf; its own
 * worker thread. NULL from create when there is no usable Vulkan device. */
struct gm_pyrowave;
struct gm_pyrowave *pyrowave_create(const struct gm_callbacks *cb);
void pyrowave_destroy(struct gm_pyrowave *pw);
void pyrowave_start(struct gm_pyrowave *pw, int bitrate_kbps, int refresh_mhz);
void pyrowave_stop(struct gm_pyrowave *pw);
void pyrowave_set_rate(struct gm_pyrowave *pw, int bitrate_kbps, int refresh_mhz);
// Compositor thread. False if the buffer cannot be encoded (not a dma-buf).
bool pyrowave_submit(struct gm_pyrowave *pw, struct wlr_buffer *buffer,
	int width, int height, uint64_t slot_ns);
void pyrowave_take_stats(struct gm_pyrowave *pw, unsigned *encoded,
	unsigned *dropped, uint64_t *encode_ns);

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
