// gmstream core: the Wayland host for gamescope plus everything that touches
// the GPU, audio and input devices. The Rust side (network, web client) talks
// to it only through this header.
//
// Threads: gm_run() runs the compositor on the calling thread. All gm_*
// functions below it are thread-safe; they queue a command for the compositor
// thread. Callbacks come from the compositor thread (cursor, pointer_lock,
// rumble), the encoder thread (video) and the PipeWire thread (audio); they
// must not block.
#ifndef GM_H
#define GM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum gm_codec {
	GM_CODEC_H264 = 0,
	GM_CODEC_HEVC = 1,
	GM_CODEC_AV1 = 2,
};

struct gm_config {
	int width, height; // initial screen size
	int refresh_mhz; // initial VRR maximum
	bool stats; // log per-second statistics
	bool debug;
	const char *socket; // Wayland socket name; NULL: first free wayland-N
	char **child_argv; // NULL-terminated; run with WAYLAND_DISPLAY set
};

struct gm_video_packet {
	const uint8_t *data; // Annex B (H.264/HEVC) or low-overhead OBUs (AV1)
	size_t len;
	uint32_t frame_id;
	uint64_t slot_ns; // CLOCK_MONOTONIC time the frame was presented
	uint64_t encoded_ns; // CLOCK_MONOTONIC time the bitstream was ready
	uint16_t width, height;
	bool keyframe;
	enum gm_codec codec;
};

struct gm_cursor {
	const uint8_t *rgba; // straight alpha, width*height*4; NULL: hidden
	uint16_t width, height;
	int16_t hotspot_x, hotspot_y;
};

struct gm_callbacks {
	void *ctx;
	void (*video)(void *ctx, const struct gm_video_packet *packet);
	void (*audio)(void *ctx, const uint8_t *opus, size_t len, uint64_t pts_ns);
	void (*cursor)(void *ctx, const struct gm_cursor *cursor);
	void (*pointer_lock)(void *ctx, bool locked);
	void (*rumble)(void *ctx, uint8_t pad, uint16_t strong, uint16_t weak,
		uint16_t duration_ms);
};

// Blocks until the child exits or gm_quit() is called. Returns an exit code.
int gm_run(const struct gm_config *config, const struct gm_callbacks *cb);
void gm_quit(void);

uint64_t gm_now_ns(void);
// CLOCK_MONOTONIC time the compositor thread last ran its event loop
// (updated twice a second); 0 before gm_run starts it. For a watchdog.
uint64_t gm_heartbeat_ns(void);

// Streaming. Start resizes the screen to the client's mode and encodes every
// frame from then on until stop.
void gm_stream_start(enum gm_codec codec, int width, int height,
	int refresh_mhz, int bitrate_kbps);
void gm_stream_configure(int width, int height, int refresh_mhz);
void gm_stream_stop(void);
void gm_request_keyframe(void);
void gm_set_bitrate(int bitrate_kbps);
// Which codecs this GPU can encode (bit 1 << enum gm_codec). Valid once
// gm_run has started the compositor; 0 before.
uint32_t gm_supported_codecs(void);

// Input. Keys and buttons are Linux evdev codes.
void gm_pointer_motion(double dx, double dy);
void gm_pointer_position(double x, double y); // normalized 0..1
void gm_pointer_button(uint32_t button, bool pressed);
// Scroll in wl_pointer axis units (15 per wheel notch) and value120 steps.
void gm_pointer_axis(double dx, double dy, int32_t v120_x, int32_t v120_y);
void gm_key(uint32_t key, bool pressed);

// Gamepads appear as Xbox 360 controllers. Buttons follow the W3C standard
// gamepad mapping (bit i = buttons[i], 0..16); sticks -32768..32767 (y down
// positive), triggers 0..255.
struct gm_gamepad_state {
	uint32_t buttons;
	int16_t lx, ly, rx, ry;
	uint8_t lt, rt;
};
void gm_gamepad_update(uint8_t pad, const struct gm_gamepad_state *state);
void gm_gamepad_remove(uint8_t pad);

#endif
