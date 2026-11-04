// gmstream core: the Wayland host for gamescope plus everything that touches
// the GPU, audio and input devices. The Rust side (network, web client) talks
// to it only through this header.
//
// Threads: gm_run() runs the compositor on the calling thread. All gm_*
// functions below it are thread-safe; they queue a command for the compositor
// thread. Callbacks come from the compositor thread (cursor, relative, cursor_clip,
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
	GM_CODEC_PYROWAVE = 3, // GPU wavelet, intra-only, fixed bytes per frame
	GM_CODEC_RIPTIDE = 4, // ours (riptide/): GPU wavelet, predicted, in stripes, fixed bytes per frame
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

// One piece of a frame's bitstream, sent while the rest is still being
// encoded (DESIGN.md §12). The pieces of a frame carry its id and arrive in
// `index` order; the client lays each at `offset` in a `total`-byte buffer and
// has the whole frame, byte for byte as the encoder produced it, once it has
// all `count` of them. `count`, `total` and `encoded_ns` are only known in the
// last piece, and are 0 before it.
struct gm_video_chunk {
	const uint8_t *data;
	size_t len;
	uint32_t frame_id;
	uint32_t offset;
	uint32_t total;
	uint16_t index, count;
	uint64_t slot_ns;
	uint64_t encoded_ns;
	// When this frame's first piece went out. The network's share of the
	// latency starts there, not at the end of the encode: with a frame sent
	// in pieces, most of it is on the wire before the encode finishes. Only
	// the frame's last piece carries it.
	uint64_t first_ns;
	uint16_t width, height;
	bool keyframe;
	bool last;
	enum gm_codec codec;
};

// Riptide (PROTOCOL.md, "Riptide"): one stripe of a frame (a horizontal band
// coded as an image of its own), handed over as soon as the GPU has finished
// it, its packets each after a u16 length.
struct gm_riptide_packets {
	const uint8_t *data;
	size_t len;
	uint32_t frame_id;
	uint16_t first; // the stripe's first packet's number in the frame (all stripes' count in order)
	uint16_t count; // the stripe's packets
	uint8_t stripe, stripes;
	uint16_t first_rows; // the first stripe's rows
	uint16_t stripe_rows; // every other stripe's but the last's, which takes the rest
	uint8_t inter; // the stripe's first packets that are inter packets
	bool predicted; // from the previous frame; else intra (the client starts over)
	uint32_t repaired; // the newest frame whose loss report the encoder has taken in
	uint64_t slot_ns;
	uint64_t encoded_ns; // when these packets were ready
	uint16_t width, height;
};

struct gm_cursor {
	const uint8_t *rgba; // straight alpha, width*height*4; NULL: hidden
	uint16_t width, height;
	int16_t hotspot_x, hotspot_y;
};

struct gm_callbacks {
	void *ctx;
	void (*video)(void *ctx, const struct gm_video_packet *packet);
	// A frame in pieces, as the encoder writes it. A frame comes either as
	// pieces or whole, never both. NULL: the client cannot take pieces.
	void (*video_chunk)(void *ctx, const struct gm_video_chunk *chunk);
	void (*audio)(void *ctx, const uint8_t *opus, size_t len, uint64_t pts_ns);
	void (*cursor)(void *ctx, const struct gm_cursor *cursor);
	// Each from gamescope's pointer constraint, and independent of each other and
	// of the cursor image: whether the game wants motion, and where it keeps the
	// cursor in stream pixels (all 0: nowhere in particular).
	void (*relative)(void *ctx, bool relative);
	void (*cursor_clip)(void *ctx, uint16_t left, uint16_t top, uint16_t right,
		uint16_t bottom);
	void (*rumble)(void *ctx, uint8_t pad, uint16_t strong, uint16_t weak,
		uint16_t duration_ms);
	// What the encoder is taking in, in words, whenever it changes (the
	// buffer gamescope hands over and what the encoder makes of it). For the
	// client's stats line; nothing depends on the wording.
	void (*encode_format)(void *ctx, const char *text);
	// Riptide packets, a stripe at a time.
	void (*riptide)(void *ctx, const struct gm_riptide_packets *packets);
};

// Blocks until the child exits or gm_quit() is called. Returns an exit code.
int gm_run(const struct gm_config *config, const struct gm_callbacks *cb);
void gm_quit(void);

uint64_t gm_now_ns(void);
// CLOCK_MONOTONIC time the compositor thread last ran its event loop
// (updated twice a second); 0 before gm_run starts it. For a watchdog.
uint64_t gm_heartbeat_ns(void);

// What a stream does beyond encoding every frame.
enum gm_stream_flags {
	// Send each frame in pieces as the encoder writes it, through
	// gm_callbacks.video_chunk, instead of whole (AV1 only).
	GM_STREAM_CHUNKED = 1 << 0,
	// The client can show HDR10 (PQ, BT.2020, ten-bit). This says what the
	// client is able to display, not what it wants: whether the session is
	// HDR at all stays SteamOS's decision (Steam's display setting, which
	// drives gamescope's hdr_enabled). The host only needs to know not to
	// send HDR to a client that cannot show it.
	GM_STREAM_HDR = 1 << 1,
};

// Streaming. Start resizes the screen to the client's mode and encodes every
// frame from then on until stop. `packet_bytes`: the most PyroWave or video
// chunk payload per datagram (0: the default, which fits a WebTransport
// datagram). `flags`: enum gm_stream_flags.
void gm_stream_start(enum gm_codec codec, int width, int height,
	int refresh_mhz, int bitrate_kbps, int packet_bytes, int flags);
void gm_stream_configure(int width, int height, int refresh_mhz);
void gm_stream_stop(void);
// The client's display refresh grid in host CLOCK_MONOTONIC ns: a vblank and
// the interval. gamescope then presents on it (fixed refresh) instead of as
// on a VRR display. Cleared when a stream starts or stops.
void gm_set_display_grid(uint64_t anchor_ns, uint64_t interval_ns);
// What the client wants of Steam's VRR: 0 leave it, 1 off (report the grid as
// a fixed refresh: gamescope calls the display VRR-incapable and Steam greys
// the toggle), 2 on (the caller sets gamescope's adaptive_sync). Cleared when
// a stream starts or stops.
void gm_set_vrr_policy(int policy);
void gm_request_keyframe(void);
// Riptide: the client missed packets of a frame. `count` is the frame's
// packets as the client knows them (0: it never learned the total); ranges are
// (first, length) pairs of missing packet indices, a length past the end
// meaning the rest. Any thread.
void gm_riptide_loss(uint32_t frame_id, uint16_t count, const uint16_t *ranges, size_t n_ranges);
void gm_set_bitrate(int bitrate_kbps);
// How many parts the encoder cuts a frame into: tiles (AV1), slice segments
// (HEVC) or stripes (Riptide); 0 for the codec's default.
void gm_set_parallelism(unsigned parts);
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
// A touchscreen: touch `id` went down, moved or up at (x, y), normalized 0..1
// over the screen. Cancel ends every touch.
enum gm_touch_phase { GM_TOUCH_DOWN = 0, GM_TOUCH_MOTION = 1, GM_TOUCH_UP = 2, GM_TOUCH_CANCEL = 3 };
void gm_touch(int32_t id, int phase, double x, double y);

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
