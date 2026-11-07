// A single-client Wayland "display" for gamescope's Wayland backend.
//
// gamescope (run with --backend wayland --force-composition) commits one
// fully composited buffer per frame to its toplevel. We treat that toplevel as
// the screen: each commit gets a present slot (VRR emulation, capped at the
// client's refresh rate), gamescope immediately receives presentation
// feedback with refresh=0 for that slot, and at the slot time the frame goes
// to the encoder. There is no compositor tick anywhere.
//
// Input arrives from the network side through gm_* calls and enters through
// our wl_seat; gamescope's cursor image is forwarded so the client can draw
// it natively.
//
// Panel mode: with no client, the frames go to this machine's own screen
// instead (see "panel" below), as native Game Mode's gamescope would show
// them, and its keyboard, mouse, touchpad and touchscreen drive the same
// seat. A client connecting is a switch to another display: the screen turns
// off and gamescope's output takes the client's mode; when it leaves, back.

#include <assert.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <drm.h>

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "core.h"
#include "keymap.h" // gm_keymap: a compiled US keymap (generated at build)

// A committed client buffer on its way to the encoder.
struct vd_frame {
	struct vd_server *server;
	struct wlr_buffer *buffer; // locked
	int width, height; // surface size (a single-pixel buffer is scaled up)
	uint64_t present_ns; // CLOCK_MONOTONIC time it is due on the output
	uint64_t release_ns; // not encoded before this (on a client grid), or 0
	struct wl_event_source *fence_source; // NULL once the GPU work is done
	int fence_fd;
};

// A screen of this machine (a connected DRM connector), for panel mode.
struct vd_screen {
	struct vd_server *server;
	struct wlr_output *output;
	struct wl_list link; // vd_server.screens
	struct wl_listener present;
	struct wl_listener needs_frame;
	struct wl_listener destroy;
};

// One of this machine's input devices (libinput), driving our seat as the
// network's input does.
struct vd_input {
	struct vd_server *server;
	struct wlr_input_device *device;
	struct wl_list link; // vd_server.inputs
	struct wl_listener events[4]; // by device type: see handle_new_input
	struct wl_listener destroy;
};

struct vd_view {
	struct vd_server *server;
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;

	struct wl_list link; // vd_server.views
	int trace_commits; // log this many more commits (after a reset)

	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
};

struct vd_server {
	const struct gm_callbacks *cb;
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend; // multi: headless (streams), DRM and libinput
	struct wlr_backend *headless;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output *output; // the stream's (headless)
	struct wlr_output_layout *layout;
	struct wlr_presentation *presentation;

	struct wlr_seat *seat;
	struct wlr_keyboard keyboard;
	double cursor_x, cursor_y;
	uint32_t buttons_down[8]; // pressed BTN_* codes, to release on stop
	int n_buttons_down;
	int32_t touches_down[10]; // touch ids down, likewise
	int n_touches_down;
	struct wlr_relative_pointer_manager_v1 *relative_pointer;
	struct wlr_pointer_constraints_v1 *constraints;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wl_event_source *pointer_report;
	bool relative_sent;
	uint16_t clip_sent[4];
	struct wlr_xdg_decoration_manager_v1 *decoration_manager;

	// gamescope's cursor: the client draws it.
	struct wlr_surface *cursor_surface;
	int cursor_hotspot_x, cursor_hotspot_y;
	uint8_t *cursor_sent; // last image sent, to skip repeats
	size_t cursor_sent_len;
	// Hiding is sent only if no new image follows within 50 ms: gamescope
	// sets an empty cursor between images (e.g. arrow to hand in Steam), which
	// a browser would show as a blink.
	struct wl_event_source *cursor_hide_timer;

	// gamescope opens one toplevel per virtual connector; in Steam mode
	// Steam decides which one is in use (e.g. one per focused app). All are
	// configured as full screens; the screen shows, and input goes to, the one
	// that last committed a frame.
	struct wl_list views;
	struct vd_view *view;

	// Requested screen size; refresh (mHz) is the VRR maximum rate.
	int width, height, refresh;

	// The newest commit, waiting for its fence or its present slot.
	struct vd_frame *waiting;
	// The last frame shown, kept so a joining client or a keyframe request
	// gets a picture even when gamescope has nothing new to draw. gamescope
	// only reuses it after two more composites, which reach us first.
	struct wlr_buffer *last_buffer; // locked
	int last_width, last_height;

	// VRR emulation. A frame is shown when it arrives, but no sooner than
	// 1/refresh after the previous one; `last_slot_ns` is the latest such
	// time handed out. A frame due in the future waits on `pace_fd`.
	int pace_fd;
	struct wl_event_source *pace_source;
	bool pace_armed;
	uint64_t last_slot_ns;
	// The client's display grid (host clock), when it reports one: gamescope
	// then gets fixed-refresh presentation feedback on it instead of VRR, and
	// its own just-in-time scheduler lines frames up with the client's
	// refreshes. 0: none.
	uint64_t grid_anchor_ns, grid_interval_ns, grid_last_vblank_ns;
	// The client's say over Steam's VRR (gm_set_vrr_policy): 1 reports its
	// grid as a fixed refresh (gamescope then calls the display VRR-incapable:
	// Steam greys its VRR toggle, and gamescope paces to the grid whatever the
	// toggle says). 0, 2: reported as VRR.
	int vrr_policy;
	// How long before its vblank gamescope committed each recent frame, in
	// GRID_LEAD_BIN_NS bins: a ring of the last GRID_LEADS and their counts.
	uint16_t grid_leads[1024], grid_lead_counts[1024];
	unsigned grid_lead_next, grid_lead_n;

	struct gm_encoder *encoder;
	struct gm_audio *audio;
	struct gm_gamepads *gamepads;
	bool streaming;

	// Panel mode (no client): frames go to `screen` (an external screen if
	// one is connected, else the built-in panel) through KMS, one flip at a
	// time as on native gamescope's DRM backend; gamescope's presentation
	// feedback is answered when its frame is actually on screen. NULL
	// session: no seat to take the screen from (no panel mode).
	struct wlr_session *session;
	struct wlr_backend *drm_backend, *libinput_backend; // panel mode's
	struct wl_event_source *panel_timer; // takes the seat session, retries
	struct wl_listener session_active; // the seat session's
	unsigned panel_fails; // commits refused in a row
	bool panel_broken; // the session went away: drop it and take a fresh one
	bool panel_complained; // the first failure is logged, not every retry
	struct wl_list screens; // vd_screen
	struct vd_screen *screen; // in use, NULL: none
	bool panel; // frames go to `screen`
	// The cursor on the screen (panel mode). wlr_cursor, not a bare
	// wlr_output_cursor: a client's cursor is a surface, and only wlr_cursor
	// takes one (wlr_output_cursor_set_buffer wants a buffer it can import,
	// which a wlr_client_buffer whose source is gone is not). It also pads
	// the image to a size the cursor plane takes -- this one wants 256x256 --
	// and follows the screens in the layout by itself.
	struct wlr_cursor *cursor;
	bool flipping; // a commit is on its way to the screen
	bool cursor_dirty; // the cursor moved during that commit
	struct wlr_presentation_feedback *flip_feedback; // its frame's
	// The newest frame committed during a flip, shown when it is done.
	struct wlr_buffer *next_buffer; // locked
	struct wlr_presentation_feedback *next_feedback;
	struct wl_list inputs; // vd_input
	struct wl_listener new_input;

	pid_t child;

	// -S: per-second delivery statistics.
	struct wl_event_source *stats_timer;
	unsigned stats_presented, stats_superseded, stats_fence_waits;
	unsigned stats_copied; // panel mode: frames copied, not scanned out

	struct wl_listener new_output;
	struct wl_listener new_toplevel;
	struct wl_listener new_decoration;
	struct wl_listener new_constraint;
	struct wl_listener constraint_destroy;
	struct wl_listener keyboard_key;
	struct wl_listener keyboard_modifiers;
	struct wl_listener request_set_cursor;
	struct wl_listener cursor_commit;
	struct wl_listener cursor_destroy;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
};

static void try_present(struct vd_server *server);
static void panel_commit(struct vd_server *server, struct wlr_surface *surface);
static void panel_lost(struct vd_server *server);
static void view_outputs(struct vd_server *server, struct vd_view *view);

uint64_t gm_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static uint32_t now_msec(void) {
	return (uint32_t)(gm_now_ns() / 1000000);
}

/* ---- frames ------------------------------------------------------------ */

static void frame_destroy(struct vd_frame *frame) {
	if (frame == NULL) {
		return;
	}
	if (frame->fence_source != NULL) {
		wl_event_source_remove(frame->fence_source);
		close(frame->fence_fd);
	}
	wlr_buffer_unlock(frame->buffer);
	free(frame);
}

static int handle_fence(int fd, uint32_t mask, void *data) {
	struct vd_frame *frame = data;
	wl_event_source_remove(frame->fence_source);
	frame->fence_source = NULL;
	close(frame->fence_fd);
	frame->server->stats_fence_waits++;
	try_present(frame->server);
	return 0;
}

static struct vd_frame *frame_create(struct vd_server *server,
		struct wlr_surface *surface) {
	struct vd_frame *frame = calloc(1, sizeof(*frame));
	if (frame == NULL) {
		return NULL;
	}
	frame->server = server;
	frame->buffer = wlr_buffer_lock(surface->current.buffer);
	frame->width = surface->current.width;
	frame->height = surface->current.height;

	// A dma-buf polls readable once all pending GPU writes to it are done,
	// the same implicit fence a KMS flip would wait on.
	struct wlr_dmabuf_attributes dmabuf;
	if (wlr_buffer_get_dmabuf(frame->buffer, &dmabuf)) {
		frame->fence_fd = dup(dmabuf.fd[0]);
		if (frame->fence_fd >= 0) {
			struct pollfd pfd = { .fd = frame->fence_fd, .events = POLLIN };
			if (poll(&pfd, 1, 0) == 1) {
				// Already signaled: no need to wait a loop turn.
				close(frame->fence_fd);
			} else {
				frame->fence_source = wl_event_loop_add_fd(server->loop,
					frame->fence_fd, WL_EVENT_READABLE, handle_fence, frame);
				if (frame->fence_source == NULL) {
					close(frame->fence_fd);
				}
			}
		}
	}
	return frame;
}

static void send_frame_done(struct wlr_surface *surface, int sx, int sy,
		void *data) {
	wlr_surface_send_frame_done(surface, data);
}

static int handle_pace(int fd, uint32_t mask, void *data) {
	struct vd_server *server = data;
	uint64_t expirations;
	if (read(fd, &expirations, sizeof(expirations)) < 0) {
		// Spurious wakeup; the timer is still armed.
		return 0;
	}
	server->pace_armed = false;
	try_present(server);
	return 0;
}

static void commit_output_mode(struct vd_server *server, int width,
		int height) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, width, height, server->refresh);
	if (!wlr_output_commit_state(server->output, &state)) {
		wlr_log(WLR_ERROR, "Output mode %dx%d failed", width, height);
	}
	wlr_output_state_finish(&state);
}

static void try_present(struct vd_server *server) {
	struct vd_frame *frame = server->waiting;
	if (frame == NULL || frame->fence_source != NULL || server->pace_armed) {
		return;
	}

	uint64_t due = frame->release_ns > frame->present_ns ? frame->release_ns : frame->present_ns;
	if (gm_now_ns() < due) {
		struct itimerspec its = {
			.it_value = {
				.tv_sec = due / 1000000000,
				.tv_nsec = due % 1000000000,
			},
		};
		timerfd_settime(server->pace_fd, TFD_TIMER_ABSTIME, &its, NULL);
		server->pace_armed = true;
		return;
	}
	server->waiting = NULL;

	if (server->encoder != NULL && server->streaming) {
		encoder_submit(server->encoder, frame->buffer, frame->width,
			frame->height, frame->present_ns);
	}
	wlr_buffer_unlock(server->last_buffer);
	server->last_buffer = wlr_buffer_lock(frame->buffer);
	server->last_width = frame->width;
	server->last_height = frame->height;
	if (frame->width != server->output->width ||
			frame->height != server->output->height) {
		// The client caught up with a resize (or hasn't yet): the output
		// always matches what is actually being shown.
		commit_output_mode(server, frame->width, frame->height);
	}
	server->stats_presented++;

	if (server->view != NULL) {
		struct timespec when = {
			.tv_sec = frame->present_ns / 1000000000,
			.tv_nsec = frame->present_ns % 1000000000,
		};
		wlr_xdg_surface_for_each_surface(server->view->toplevel->base,
			send_frame_done, &when);
	}
	frame_destroy(frame);
}

/* ---- output ------------------------------------------------------------ */

static void configure_view(struct vd_server *server, struct vd_view *view) {
	if (!view->toplevel->base->initialized) {
		return;
	}
	wlr_xdg_toplevel_set_size(view->toplevel, server->width, server->height);
	wlr_xdg_toplevel_set_fullscreen(view->toplevel, true);
	wlr_xdg_toplevel_set_activated(view->toplevel, true);
	if (view->decoration != NULL) {
		wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void set_screen_size(struct vd_server *server, int width, int height,
		int refresh) {
	if (width <= 0 || height <= 0) {
		return;
	}
	// YUV 4:2:0 needs even dimensions.
	width &= ~1;
	height &= ~1;
	if (refresh <= 0) {
		refresh = server->refresh;
	}
	wlr_log(WLR_INFO, "Screen size %dx%d@%d.%03dHz", width, height,
		refresh / 1000, refresh % 1000);
	bool refresh_changed = refresh != server->refresh;
	server->width = width;
	server->height = height;
	server->refresh = refresh;
	if (refresh_changed && server->output != NULL) {
		// gamescope reads the VRR maximum from the output's mode right away;
		// the size follows the client's buffers (see try_present).
		commit_output_mode(server, server->output->width,
			server->output->height);
	}
	struct vd_view *view;
	wl_list_for_each(view, &server->views, link) {
		configure_view(server, view);
	}
}

static void screen_added(struct vd_server *server, struct wlr_output *output);

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct vd_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *output = data;
	if (wlr_output_is_drm(output)) {
		screen_added(server, output);
		return;
	}
	if (server->output != NULL) {
		return;
	}
	wlr_output_init_render(output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, server->width, server->height,
		server->refresh);
	if (!wlr_output_commit_state(output, &state)) {
		wlr_log(WLR_ERROR, "Failed to enable output");
	}
	wlr_output_state_finish(&state);

	server->output = output;
	wlr_output_layout_add_auto(server->layout, output);
	wlr_output_create_global(output, server->display);
}

/* ---- toplevel ---------------------------------------------------------- */

static void focus_view(struct vd_server *server) {
	struct vd_view *view = server->view;
	struct wlr_surface *surface = view->toplevel->base->surface;

	wlr_surface_send_enter(surface, server->output);
	wlr_seat_keyboard_notify_enter(server->seat, surface,
		server->keyboard.keycodes, server->keyboard.num_keycodes,
		&server->keyboard.modifiers);
	wlr_seat_pointer_notify_enter(server->seat, surface,
		server->cursor_x, server->cursor_y);
}

// Measuring (GM_DEBUG_GRID=<file>): every grid update ("g now anchor
// interval"), every commit's feedback on a grid ("c now vblank frame
// release") and every frame's commit and slot ("s now slot"), host ns.
static FILE *grid_trace(void) {
	static FILE *f;
	static bool opened;
	if (!opened) {
		opened = true;
		const char *path = getenv("GM_DEBUG_GRID");
		f = path != NULL ? fopen(path, "w") : NULL;
		if (f != NULL) {
			setvbuf(f, NULL, _IOLBF, 0);
		}
	}
	return f;
}

#define GRID_LEADS 1024
#define GRID_LEAD_BIN_NS 25000

// When a frame committed `lead_ns` before its grid vblank leaves for the
// encoder: no earlier than the 10th percentile of recent leads before that
// vblank. gamescope wakes for each vblank early by its own draw-time
// estimate, which it raises for a while after a slow composite: on
// the test machine every ~8 s it committed 0.5 ms earlier for ~2 s (3.75 instead of
// 3.25 ms before the vblank). Passed on, that moved every frame 0.5 ms
// earlier on the client's screen, past the edge its phase lock had set
// them a margin from: a burst of dropped frames each time. Holding the
// early ones to the usual time keeps the stream on one phase; frames
// committed as late as usual or later are not held (about 0.1 ms median).
static uint64_t grid_release(struct vd_server *server, uint64_t now,
		uint64_t vblank) {
	uint64_t lead = vblank > now ? vblank - now : 0;
	unsigned bin = lead / GRID_LEAD_BIN_NS;
	if (bin >= GRID_LEADS) {
		bin = GRID_LEADS - 1;
	}
	if (server->grid_lead_n == GRID_LEADS) {
		server->grid_lead_counts[server->grid_leads[server->grid_lead_next]]--;
	} else {
		server->grid_lead_n++;
	}
	server->grid_leads[server->grid_lead_next] = bin;
	server->grid_lead_counts[bin]++;
	server->grid_lead_next = (server->grid_lead_next + 1) % GRID_LEADS;
	if (server->grid_lead_n < 120) {
		return 0; // not enough to tell the usual lead yet
	}
	unsigned seen = 0, p10 = 0;
	while (p10 < GRID_LEADS - 1 &&
			(seen += server->grid_lead_counts[p10]) * 10 < server->grid_lead_n) {
		p10++;
	}
	uint64_t hold = (uint64_t)p10 * GRID_LEAD_BIN_NS;
	return vblank > hold && vblank - hold > now ? vblank - hold : 0;
}

static void grid_leads_reset(struct vd_server *server) {
	memset(server->grid_lead_counts, 0, sizeof(server->grid_lead_counts));
	server->grid_lead_next = server->grid_lead_n = 0;
}

// The vblank of the client's display grid a new frame flips on: the next
// grid point, one later than the last one reported. A commit without a new
// frame gets the last one again, so reported times never run ahead.
static uint64_t grid_vblank(struct vd_server *server, uint64_t now, bool frame) {
	if (!frame && server->grid_last_vblank_ns != 0) {
		return server->grid_last_vblank_ns;
	}
	uint64_t a = server->grid_anchor_ns, i = server->grid_interval_ns;
	uint64_t vb = now <= a ? a : a + (now - a + i - 1) / i * i;
	while (vb > i && vb - i >= now) {
		vb -= i; // an anchor in the future
	}
	if (vb <= server->grid_last_vblank_ns) {
		vb = server->grid_last_vblank_ns + i;
	}
	server->grid_last_vblank_ns = vb;
	return vb;
}

// Answers the commit's presentation feedback, if it asked for any: shown at
// `when_ns`, or with a client display grid, at that grid's next vblank.
// As a VRR display (refresh 0), unless the client asked for a fixed refresh
// (vrr_policy 1): gamescope's Wayland backend reports VRR support only for
// refresh 0, and Steam hides its VRR toggle otherwise. With Steam's toggle off
// gamescope still paces to these timestamps (its vblank timer, at the output
// mode's refresh), so a client grid still sets where frames land; with it on,
// gamescope presents as soon as frames are ready. Reported as a fixed refresh,
// gamescope takes its refresh from the grid's interval, or with no grid the
// stream's rate, and is never VRR.
// Returns when a frame on a client grid may leave for the encoder (0: now).
static uint64_t send_feedback(struct vd_server *server, struct wlr_surface *surface,
		uint64_t when_ns, bool frame) {
	struct wlr_presentation_feedback *feedback =
		wlr_presentation_surface_sampled(surface);
	if (feedback == NULL) {
		return 0;
	}
	uint64_t release = 0;
	if (server->grid_interval_ns != 0) {
		uint64_t now = gm_now_ns();
		when_ns = grid_vblank(server, now, frame);
		if (frame) {
			release = grid_release(server, now, when_ns);
		}
		if (grid_trace() != NULL) {
			fprintf(grid_trace(), "c %" PRIu64 " %" PRIu64 " %d %" PRIu64 "\n",
				now, when_ns, frame, release);
		}
	}
	// The refresh gamescope is told: the mode's, unless the client's grid is
	// really another rate. The grid's interval is the client's measurement and
	// moves by nanoseconds each time it is sent; passed on as it is, gamescope
	// read 120.000 / 120.001 Hz turn about and updated Xwayland's mode (and
	// reallocated its buffers) on every change, ~5 times a minute, which the
	// game sees. Frames are still placed on the grid itself (grid_vblank).
	uint64_t mode_ns = 1000000000000ull / (uint64_t)server->refresh;
	uint64_t fixed_refresh_ns = mode_ns;
	if (server->grid_interval_ns != 0) {
		uint64_t g = server->grid_interval_ns;
		uint64_t off = g > mode_ns ? g - mode_ns : mode_ns - g;
		if (off * 200 > mode_ns) { // more than 0.5% away
			fixed_refresh_ns = g / 1000 * 1000; // to the microsecond: steady
		}
	}
	struct wlr_presentation_event event = {
		.output = server->output,
		.tv_sec = when_ns / 1000000000,
		.tv_nsec = when_ns % 1000000000,
		.refresh = server->vrr_policy == 1 ? (uint32_t)fixed_refresh_ns : 0,
	};
	wlr_presentation_feedback_send_presented(feedback, &event);
	wlr_presentation_feedback_destroy(feedback);
	return release;
}

static void handle_view_commit(struct wl_listener *listener, void *data) {
	struct vd_view *view = wl_container_of(listener, view, commit);
	struct vd_server *server = view->server;
	struct wlr_surface *surface = view->toplevel->base->surface;

	// Every commit that shows nothing still gets its presentation feedback
	// answered right away: under VRR gamescope paints nothing while a present
	// is unanswered, and it would not commit again to let wlroots discard it.
	if (view->trace_commits > 0) {
		view->trace_commits--;
		wlr_log(WLR_INFO, "toplevel %p commit: initial=%d configured=%d "
			"mapped=%d buffer=%s %dx%d", (void *)view,
			view->toplevel->base->initial_commit,
			view->toplevel->base->configured, surface->mapped,
			!(surface->current.committed & WLR_SURFACE_STATE_BUFFER) ? "unchanged" :
				surface->current.buffer != NULL ? "new" : "null",
			surface->current.width, surface->current.height);
	}
	if (view->toplevel->base->initial_commit) {
		view->trace_commits = 20;
		send_feedback(server, surface, gm_now_ns(), false);
		wlr_log(WLR_INFO, "Configuring toplevel %p", (void *)view);
		configure_view(server, view);
		// The client waits for a configure before it maps. When a toplevel
		// starts over (gamescope unmaps it when a game exits), our state
		// equals what wlroots last sent, so the setters above schedule
		// nothing.
		wlr_xdg_surface_schedule_configure(view->toplevel->base);
		return;
	}
	if (server->output == NULL ||
			!(surface->current.committed & WLR_SURFACE_STATE_BUFFER) ||
			surface->current.buffer == NULL) {
		send_feedback(server, surface, gm_now_ns(), false);
		return;
	}
	if (server->view != view) {
		wlr_log(WLR_INFO, "Showing toplevel '%s'",
			view->toplevel->title ? view->toplevel->title : "");
		server->view = view;
		focus_view(server);
	}
	if (server->panel) {
		panel_commit(server, surface);
		return;
	}

	// Decide when a VRR panel would show this frame: now, or one maximum-
	// refresh interval after the previous flip; a frame replacing one still
	// queued takes over its slot.
	uint64_t commit = gm_now_ns(), slot;
	if (server->waiting != NULL) {
		server->stats_superseded++;
		slot = server->waiting->present_ns;
	} else {
		uint64_t period = 1000000000000ull / (uint64_t)server->refresh;
		slot = commit;
		// On a client grid gamescope paces itself; no VRR cap.
		if (server->grid_interval_ns == 0 && server->last_slot_ns != 0 &&
				slot < server->last_slot_ns + period) {
			slot = server->last_slot_ns + period;
		}
		server->last_slot_ns = slot;
	}

	// Tell the client right away. gamescope decides when it may flip again
	// from the last presentation time alone (on DRM a pending flip blocks it
	// instead), and it composites into a fixed ring of three images without
	// waiting for releases: a late timestamp would let it composite again
	// and again into the frame we are holding.
	uint64_t release = send_feedback(server, surface, slot, true);
	if (grid_trace() != NULL) {
		// Every frame: when gamescope committed it and its slot (on a client
		// grid, the commit time; else the VRR emulation's, maybe later).
		fprintf(grid_trace(), "s %" PRIu64 " %" PRIu64 "\n", commit, slot);
	}

	// gamescope sends the whole composited frame on its root surface when
	// run with --force-composition; anything on subsurfaces is not shown.
	frame_destroy(server->waiting);
	server->waiting = frame_create(server, surface);
	if (server->waiting != NULL) {
		server->waiting->present_ns = slot;
		server->waiting->release_ns = release;
		// A client that holds frames itself to a fixed delay after this time
		// (VRR on, vrr_policy 2, no grid) gets each frame at once, stamped
		// with its commit: gamescope (VRR, refresh = the client's cap) already
		// keeps to the rate, and holding to the VRR emulation's slot here
		// added 0–2.7 ms that varied with the game's frame times, the very
		// spacing the client's display is to keep.
		if (server->vrr_policy == 2 && server->grid_interval_ns == 0) {
			server->waiting->present_ns = commit;
		}
	}
	try_present(server);
}

static void handle_view_map(struct wl_listener *listener, void *data) {
	struct vd_view *view = wl_container_of(listener, view, map);
	struct vd_server *server = view->server;
	wlr_log(WLR_INFO, "Mapped toplevel %p '%s'", (void *)view,
		view->toplevel->title ? view->toplevel->title : "");
	view_outputs(server, view);
	if (server->view == NULL) {
		server->view = view;
		focus_view(server);
	}
}

// Encodes the last frame again if no newer one is on its way.
static void repeat_last_frame(struct vd_server *server) {
	uint64_t period = 1000000000000ull / (uint64_t)server->refresh;
	if (server->encoder != NULL && server->streaming &&
			server->waiting == NULL && server->last_buffer != NULL &&
			gm_now_ns() > server->last_slot_ns + period) {
		encoder_submit(server->encoder, server->last_buffer,
			server->last_width, server->last_height, gm_now_ns());
	}
}

static void handle_view_unmap(struct wl_listener *listener, void *data) {
	struct vd_view *view = wl_container_of(listener, view, unmap);
	struct vd_server *server = view->server;
	wlr_log(WLR_INFO, "Unmapped toplevel %p", (void *)view);
	view->trace_commits = 20;
	if (server->view != view) {
		return;
	}
	frame_destroy(server->waiting);
	server->waiting = NULL;
	server->view = NULL;
	wlr_buffer_unlock(server->last_buffer);
	server->last_buffer = NULL;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	wlr_seat_pointer_notify_clear_focus(server->seat);
}

static void handle_view_destroy(struct wl_listener *listener, void *data) {
	struct vd_view *view = wl_container_of(listener, view, destroy);
	if (view->server->view == view) {
		view->server->view = NULL;
	}
	wl_list_remove(&view->link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	free(view);
}

static void handle_new_toplevel(struct wl_listener *listener, void *data) {
	struct vd_server *server = wl_container_of(listener, server, new_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct vd_view *view = calloc(1, sizeof(*view));
	view->server = server;
	view->toplevel = toplevel;
	toplevel->base->data = view;

	view->commit.notify = handle_view_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
	view->map.notify = handle_view_map;
	wl_signal_add(&toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = handle_view_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
	view->destroy.notify = handle_view_destroy;
	wl_signal_add(&toplevel->events.destroy, &view->destroy);
	wl_list_insert(&server->views, &view->link);
	wlr_log(WLR_INFO, "New toplevel %p", (void *)view);
}

static void handle_new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct vd_view *view = decoration->toplevel->base->data;
	if (view != NULL) {
		view->decoration = decoration;
	}
	if (decoration->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

/* ---- cursor ------------------------------------------------------------ */

static void emit_cursor(struct vd_server *server, const struct gm_cursor *c);

static void send_cursor(struct vd_server *server) {
	struct gm_cursor cursor = {0};
	uint8_t *pixels = NULL;
	struct wlr_texture *texture = server->cursor_surface != NULL ?
		wlr_surface_get_texture(server->cursor_surface) : NULL;
	if (server->cursor_surface != NULL && texture == NULL) {
		// gamescope points the cursor at a new surface before committing its
		// image; wait for that commit rather than flashing "hidden".
		return;
	}
	if (texture != NULL && texture->width <= 256 && texture->height <= 256) {
		size_t len = (size_t)texture->width * texture->height * 4;
		pixels = malloc(len);
		bool ok = wlr_texture_read_pixels(texture,
			&(struct wlr_texture_read_pixels_options){
				.data = pixels,
				.format = DRM_FORMAT_ABGR8888, // bytes R, G, B, A
				.stride = texture->width * 4,
			});
		if (ok) {
			// Wayland buffers are premultiplied; the client wants straight
			// alpha.
			for (size_t i = 0; i < len; i += 4) {
				unsigned a = pixels[i + 3];
				if (a != 0 && a != 255) {
					for (int c = 0; c < 3; c++) {
						unsigned v = pixels[i + c] * 255u / a;
						pixels[i + c] = (uint8_t)(v > 255 ? 255 : v);
					}
				}
			}
			cursor = (struct gm_cursor){
				.rgba = pixels,
				.width = (uint16_t)texture->width,
				.height = (uint16_t)texture->height,
				.hotspot_x = (int16_t)server->cursor_hotspot_x,
				.hotspot_y = (int16_t)server->cursor_hotspot_y,
			};
		}
	}

	if (cursor.rgba == NULL) {
		if (server->cursor_hide_timer != NULL) {
			wl_event_source_timer_update(server->cursor_hide_timer, 50);
		}
		free(pixels);
		return;
	}
	if (server->cursor_hide_timer != NULL) {
		wl_event_source_timer_update(server->cursor_hide_timer, 0);
	}
	emit_cursor(server, &cursor);
	free(pixels);
}

static int handle_cursor_hide(void *data) {
	struct gm_cursor hidden = {0};
	emit_cursor(data, &hidden);
	return 0;
}

// Sends `cursor` to the client unless it is what was sent last.
static void emit_cursor(struct vd_server *server, const struct gm_cursor *c) {
	struct gm_cursor cursor = *c;
	// Skip repeats (gamescope re-sets its cursor on every pointer enter).
	size_t len = cursor.rgba != NULL ? (size_t)cursor.width * cursor.height * 4 : 0;
	size_t sent_len = 8 + len;
	uint8_t *sent = malloc(sent_len);
	int16_t meta[4] = { (int16_t)cursor.width, (int16_t)cursor.height,
		cursor.hotspot_x, cursor.hotspot_y };
	memcpy(sent, meta, 8);
	if (len > 0) {
		memcpy(sent + 8, cursor.rgba, len);
	}
	if (server->cursor_sent != NULL && server->cursor_sent_len == sent_len &&
			memcmp(server->cursor_sent, sent, sent_len) == 0) {
		free(sent);
	} else {
		free(server->cursor_sent);
		server->cursor_sent = sent;
		server->cursor_sent_len = sent_len;
		server->cb->cursor(server->cb->ctx, &cursor);
	}
}

static void clear_cursor_surface(struct vd_server *server) {
	if (server->cursor_surface != NULL) {
		wl_list_remove(&server->cursor_commit.link);
		wl_list_remove(&server->cursor_destroy.link);
		server->cursor_surface = NULL;
	}
}

static void screen_cursor_update(struct vd_server *server);
static void cursor_sync(struct vd_server *server);

// GM_DEBUG_CURSOR=1: what the client offers as a cursor, and what the panel's
// cursor plane does with it.
static bool gm_debug_cursor(void) {
	static int on = -1;
	if (on < 0) {
		on = getenv("GM_DEBUG_CURSOR") != NULL;
	}
	return on != 0;
}

static void handle_cursor_commit(struct wl_listener *listener, void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, cursor_commit);
	server->cursor_hotspot_x -= server->cursor_surface->current.dx;
	server->cursor_hotspot_y -= server->cursor_surface->current.dy;
	send_cursor(server);
	screen_cursor_update(server);
}

static void handle_cursor_destroy(struct wl_listener *listener, void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, cursor_destroy);
	clear_cursor_surface(server);
	send_cursor(server);
	screen_cursor_update(server);
}

static void handle_request_set_cursor(struct wl_listener *listener,
		void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (event->seat_client != server->seat->pointer_state.focused_client) {
		return;
	}
	if (gm_debug_cursor()) {
		wlr_log(WLR_INFO, "Cursor: client set %s, hotspot %d,%d",
			event->surface != NULL ? "a surface" : "NOTHING (hidden)",
			event->hotspot_x, event->hotspot_y);
	}
	clear_cursor_surface(server);
	server->cursor_surface = event->surface;
	server->cursor_hotspot_x = event->hotspot_x;
	server->cursor_hotspot_y = event->hotspot_y;
	if (event->surface != NULL) {
		server->cursor_commit.notify = handle_cursor_commit;
		wl_signal_add(&event->surface->events.commit, &server->cursor_commit);
		server->cursor_destroy.notify = handle_cursor_destroy;
		wl_signal_add(&event->surface->events.destroy, &server->cursor_destroy);
	}
	send_cursor(server);
	screen_cursor_update(server);
}

/* ---- input ------------------------------------------------------------- */

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	struct vd_server *server = wl_container_of(listener, server, keyboard_key);
	struct wlr_keyboard_key_event *event = data;
	wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
		event->keycode, event->state);
}

static void handle_keyboard_modifiers(struct wl_listener *listener,
		void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, keyboard_modifiers);
	wlr_seat_keyboard_notify_modifiers(server->seat,
		&server->keyboard.modifiers);
}

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "gmstream-keyboard",
};

static bool init_keyboard(struct vd_server *server) {
	wlr_keyboard_init(&server->keyboard, &keyboard_impl, "gmstream-keyboard");
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES |
		XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(ctx, gm_keymap,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(ctx);
	if (keymap == NULL) {
		wlr_log(WLR_ERROR, "Compiling the built-in keymap failed");
		return false;
	}
	wlr_keyboard_set_keymap(&server->keyboard, keymap);
	xkb_keymap_unref(keymap);

	server->keyboard_key.notify = handle_keyboard_key;
	wl_signal_add(&server->keyboard.events.key, &server->keyboard_key);
	server->keyboard_modifiers.notify = handle_keyboard_modifiers;
	wl_signal_add(&server->keyboard.events.modifiers,
		&server->keyboard_modifiers);
	wlr_seat_set_keyboard(server->seat, &server->keyboard);
	return true;
}

static void key(struct vd_server *server, uint32_t code, bool pressed) {
	struct wlr_keyboard_key_event event = {
		.time_msec = now_msec(),
		.keycode = code,
		.update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED :
			WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&server->keyboard, &event);
}

// The size of what gamescope shows, in surface coordinates (= pixels).
static void screen_size(struct vd_server *server, int *w, int *h) {
	if (server->panel) {
		*w = server->screen->output->width;
		*h = server->screen->output->height;
	} else {
		*w = server->output != NULL ? server->output->width : server->width;
		*h = server->output != NULL ? server->output->height : server->height;
	}
}

static bool pointer_locked(struct vd_server *server) {
	return server->active_constraint != NULL &&
		server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

// A client's mouse is passed on as it came, a position as a position and motion
// as motion, whatever the game wants. gamescope takes in both; the client chooses
// (protocol Position, Motion).
static void client_motion(struct vd_server *server, double dx, double dy) {
	wlr_relative_pointer_manager_v1_send_relative_motion(
		server->relative_pointer, server->seat, (uint64_t)now_msec() * 1000,
		dx, dy, dx, dy);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void client_position(struct vd_server *server, double x, double y) {
	int w, h;
	screen_size(server, &w, &h);
	server->cursor_x = x < 0 ? 0 : x > w - 1 ? w - 1 : x;
	server->cursor_y = y < 0 ? 0 : y > h - 1 ? h - 1 : y;
	// The view is the whole screen at (0, 0): layout == surface coords.
	wlr_seat_pointer_notify_motion(server->seat, now_msec(),
		server->cursor_x, server->cursor_y);
	wlr_seat_pointer_notify_frame(server->seat);
}

// The mouse on the panel (panel mode), which has no client to keep its position.
static void pointer_moved(struct vd_server *server, double dx, double dy,
		bool absolute, double x, double y) {
	uint32_t time = now_msec();
	if (absolute) {
		dx = x - server->cursor_x;
		dy = y - server->cursor_y;
	}
	if (pointer_locked(server)) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
			server->relative_pointer, server->seat, (uint64_t)time * 1000,
			dx, dy, dx, dy);
	}
	if (absolute || !pointer_locked(server)) {
		int w, h;
		screen_size(server, &w, &h);
		x = absolute ? x : server->cursor_x + dx;
		y = absolute ? y : server->cursor_y + dy;
		server->cursor_x = x < 0 ? 0 : x > w - 1 ? w - 1 : x;
		server->cursor_y = y < 0 ? 0 : y > h - 1 ? h - 1 : y;
	}
	if (!pointer_locked(server)) {
		// The view is the whole screen at (0, 0): layout == surface coords.
		wlr_seat_pointer_notify_motion(server->seat, time,
			server->cursor_x, server->cursor_y);
		cursor_sync(server);
	}
	wlr_seat_pointer_notify_frame(server->seat);
}

static void pointer_button(struct vd_server *server, uint32_t button,
		bool pressed) {
	int found = -1;
	for (int i = 0; i < server->n_buttons_down; i++) {
		if (server->buttons_down[i] == button) {
			found = i;
		}
	}
	if (pressed && found < 0 && server->n_buttons_down < 8) {
		server->buttons_down[server->n_buttons_down++] = button;
	} else if (!pressed && found >= 0) {
		server->buttons_down[found] =
			server->buttons_down[--server->n_buttons_down];
	} else if (!pressed) {
		return;
	}
	wlr_seat_pointer_notify_button(server->seat, now_msec(), button,
		pressed ? WL_POINTER_BUTTON_STATE_PRESSED :
			WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void pointer_axis(struct vd_server *server, double dx, double dy,
		int32_t v120_x, int32_t v120_y) {
	uint32_t time = now_msec();
	if (dy != 0 || v120_y != 0) {
		wlr_seat_pointer_notify_axis(server->seat, time,
			WL_POINTER_AXIS_VERTICAL_SCROLL, dy, v120_y,
			v120_y != 0 ? WL_POINTER_AXIS_SOURCE_WHEEL :
				WL_POINTER_AXIS_SOURCE_FINGER,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	}
	if (dx != 0 || v120_x != 0) {
		wlr_seat_pointer_notify_axis(server->seat, time,
			WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx, v120_x,
			v120_x != 0 ? WL_POINTER_AXIS_SOURCE_WHEEL :
				WL_POINTER_AXIS_SOURCE_FINGER,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	}
	wlr_seat_pointer_notify_frame(server->seat);
}

// A client's touchscreen on gamescope's surface, which fills the screen.
// gamescope (patch 0006) hands the touches to its own seat, following its
// touch mode: SteamOS passes them through to Steam and the game.
static void touch(struct vd_server *server, int32_t id, int phase, double x,
		double y) {
	uint32_t time = now_msec();
	int w, h;
	screen_size(server, &w, &h);
	int found = -1;
	for (int i = 0; i < server->n_touches_down; i++) {
		if (server->touches_down[i] == id) {
			found = i;
		}
	}
	switch (phase) {
	case GM_TOUCH_DOWN:
		if (found >= 0 || server->n_touches_down >= 10 || server->view == NULL) {
			return;
		}
		server->touches_down[server->n_touches_down++] = id;
		wlr_seat_touch_notify_down(server->seat,
			server->view->toplevel->base->surface, time, id, x * w, y * h);
		break;
	case GM_TOUCH_MOTION:
		if (found < 0) {
			return;
		}
		wlr_seat_touch_notify_motion(server->seat, time, id, x * w, y * h);
		break;
	case GM_TOUCH_UP:
		if (found < 0) {
			return;
		}
		server->touches_down[found] =
			server->touches_down[--server->n_touches_down];
		wlr_seat_touch_notify_up(server->seat, time, id);
		break;
	default:
		// gamescope's own seat has no cancel either: each touch ends.
		if (server->n_touches_down == 0) {
			return;
		}
		while (server->n_touches_down > 0) {
			wlr_seat_touch_notify_up(server->seat, time,
				server->touches_down[--server->n_touches_down]);
		}
		break;
	}
	wlr_seat_touch_notify_frame(server->seat);
}

static void release_all_input(struct vd_server *server) {
	touch(server, 0, GM_TOUCH_CANCEL, 0, 0);
	while (server->n_buttons_down > 0) {
		pointer_button(server, server->buttons_down[0], false);
	}
	while (server->keyboard.num_keycodes > 0) {
		key(server, server->keyboard.keycodes[0], false);
	}
}

// gamescope's constraint says two things: a lock asks for motion, a confinement
// doesn't, and the region of either is the game's clip (none: no clip). It
// replaces a constraint to change it, so the news goes out when the event loop
// is done with the old one going and the new one coming.
static void report_pointer(void *data) {
	struct vd_server *server = data;
	server->pointer_report = NULL;
	bool relative = pointer_locked(server);
	uint16_t clip[4] = {0};
	if (server->active_constraint != NULL &&
			pixman_region32_not_empty(&server->active_constraint->current.region)) {
		const pixman_box32_t *box =
			pixman_region32_extents(&server->active_constraint->current.region);
		clip[0] = box->x1;
		clip[1] = box->y1;
		clip[2] = box->x2;
		clip[3] = box->y2;
	}
	if (relative != server->relative_sent) {
		server->relative_sent = relative;
		wlr_log(WLR_INFO, "Game wants %s input", relative ? "relative" : "absolute");
		server->cb->relative(server->cb->ctx, relative);
	}
	if (memcmp(clip, server->clip_sent, sizeof(clip)) != 0) {
		memcpy(server->clip_sent, clip, sizeof(clip));
		wlr_log(WLR_INFO, "Cursor clip %d,%d-%d,%d", clip[0], clip[1], clip[2], clip[3]);
		server->cb->cursor_clip(server->cb->ctx, clip[0], clip[1], clip[2], clip[3]);
	}
}

static void report_pointer_soon(struct vd_server *server) {
	if (server->pointer_report == NULL) {
		server->pointer_report = wl_event_loop_add_idle(server->loop, report_pointer, server);
	}
}

static void handle_constraint_destroy(struct wl_listener *listener,
		void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, constraint_destroy);
	wl_list_remove(&server->constraint_destroy.link);
	server->active_constraint = NULL;
	report_pointer_soon(server);
}

static void handle_new_constraint(struct wl_listener *listener, void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	// There is only one surface to constrain to, so honor the newest one.
	if (server->active_constraint != NULL) {
		wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
		wl_list_remove(&server->constraint_destroy.link);
	}
	server->active_constraint = constraint;
	server->constraint_destroy.notify = handle_constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &server->constraint_destroy);
	wlr_pointer_constraint_v1_send_activated(constraint);
	report_pointer_soon(server);
}

static void handle_request_set_selection(struct wl_listener *listener,
		void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

/* ---- panel ------------------------------------------------------------ */
//
// With no client, gamescope's frames go to a screen of this machine, the way
// native Game Mode's gamescope shows them on its DRM backend: each committed
// buffer goes to the screen as is when the display engine can scan it out,
// else it is copied into the screen's own buffer; one flip at a time, the
// newest frame committed during a flip going next. Presentation feedback is
// answered from the flip's completion (real timestamps; refresh 0 under VRR),
// so gamescope's scheduler runs on the screen's real vblanks as it does
// natively. gamescope's cursor is put on the screen's cursor plane.

static bool screen_internal(struct wlr_output *output) {
	return strncmp(output->name, "eDP", 3) == 0 || strncmp(output->name, "LVDS", 4) == 0 ||
		strncmp(output->name, "DSI", 3) == 0;
}

// The surface is on the output frames go to: the screen, or the stream's.
static void view_outputs(struct vd_server *server, struct vd_view *view) {
	struct wlr_surface *surface = view->toplevel->base->surface;
	struct vd_screen *screen;
	wl_list_for_each(screen, &server->screens, link) {
		if (server->panel && screen == server->screen) {
			wlr_surface_send_enter(surface, screen->output);
		} else {
			wlr_surface_send_leave(surface, screen->output);
		}
	}
	if (server->output != NULL) {
		if (server->panel) {
			wlr_surface_send_leave(surface, server->output);
		} else {
			wlr_surface_send_enter(surface, server->output);
		}
	}
}

static void send_frame_done_at(struct vd_server *server, const struct timespec *when) {
	if (server->view != NULL) {
		wlr_xdg_surface_for_each_surface(server->view->toplevel->base,
			send_frame_done, (void *)when);
	}
}

static void answer_now(struct vd_server *server,
		struct wlr_presentation_feedback *feedback) {
	if (feedback == NULL) {
		return;
	}
	uint64_t now = gm_now_ns();
	struct wlr_presentation_event event = {
		.output = server->screen != NULL ? server->screen->output : server->output,
		.tv_sec = now / 1000000000,
		.tv_nsec = now % 1000000000,
	};
	wlr_presentation_feedback_send_presented(feedback, &event);
	wlr_presentation_feedback_destroy(feedback);
}

// Commits `buffer` (NULL: nothing new, e.g. for the cursor) to the screen.
static void screen_show(struct vd_server *server, struct wlr_buffer *buffer,
		struct wlr_presentation_feedback *feedback) {
	struct wlr_output *output = server->screen->output;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (buffer != NULL) {
		wlr_output_state_set_buffer(&state, buffer);
		if (!wlr_output_test_state(output, &state)) {
			// Not scanned out as is (format, modifier, size): copied into
			// the screen's own buffer, scaled to fit.
			wlr_output_state_finish(&state);
			wlr_output_state_init(&state);
			server->stats_copied++;
			struct wlr_texture *texture =
				wlr_texture_from_buffer(server->renderer, buffer);
			struct wlr_render_pass *pass = texture != NULL ?
				wlr_output_begin_render_pass(output, &state, NULL) : NULL;
			if (pass != NULL) {
				wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
					.texture = texture,
					.dst_box = { .width = output->width, .height = output->height },
					.filter_mode = WLR_SCALE_FILTER_BILINEAR,
				});
				wlr_render_pass_submit(pass);
			}
			wlr_texture_destroy(texture);
			if (pass == NULL) {
				wlr_log(WLR_ERROR, "Panel: cannot draw the frame");
				wlr_output_state_finish(&state);
				answer_now(server, feedback);
				return;
			}
		}
	}
	server->cursor_dirty = false;
	if (wlr_output_commit_state(output, &state)) {
		server->panel_fails = 0;
		server->flipping = true;
		server->flip_feedback = feedback;
		server->stats_presented += buffer != NULL;
	} else {
		// Session inactive (another VT), or the screen went away. A few in
		// a row and the session is not coming back (see panel_take); the
		// seat session's active signal usually gets there first.
		if (++server->panel_fails > 10) {
			panel_lost(server);
		}
		answer_now(server, feedback);
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		send_frame_done_at(server, &now);
	}
	wlr_output_state_finish(&state);
}

static void panel_commit(struct vd_server *server, struct wlr_surface *surface) {
	struct wlr_presentation_feedback *feedback =
		wlr_presentation_surface_sampled(surface);
	struct wlr_buffer *buffer = surface->current.buffer;
	// The last frame, for a client that joins (repeat_last_frame).
	wlr_buffer_unlock(server->last_buffer);
	server->last_buffer = wlr_buffer_lock(buffer);
	server->last_width = surface->current.width;
	server->last_height = surface->current.height;
	if (server->flipping) {
		// Replaces any frame already waiting, as a newer flip would.
		if (server->next_buffer != NULL) {
			server->stats_superseded++;
			wlr_buffer_unlock(server->next_buffer);
		}
		if (server->next_feedback != NULL) {
			wlr_presentation_feedback_destroy(server->next_feedback); // discarded
		}
		server->next_buffer = wlr_buffer_lock(buffer);
		server->next_feedback = feedback;
		return;
	}
	screen_show(server, buffer, feedback);
}

static void handle_screen_present(struct wl_listener *listener, void *data) {
	struct vd_screen *screen = wl_container_of(listener, screen, present);
	struct vd_server *server = screen->server;
	struct wlr_output_event_present *event = data;
	if (server->screen != screen || !server->flipping) {
		return;
	}
	server->flipping = false;
	if (server->flip_feedback != NULL) {
		if (event->presented) {
			struct wlr_presentation_event presented;
			wlr_presentation_event_from_output(&presented, event);
			wlr_presentation_feedback_send_presented(server->flip_feedback, &presented);
		}
		wlr_presentation_feedback_destroy(server->flip_feedback);
		server->flip_feedback = NULL;
	}
	send_frame_done_at(server, &event->when);
	if (!server->panel) {
		return;
	}
	if (server->next_buffer != NULL) {
		struct wlr_buffer *buffer = server->next_buffer;
		struct wlr_presentation_feedback *feedback = server->next_feedback;
		server->next_buffer = NULL;
		server->next_feedback = NULL;
		screen_show(server, buffer, feedback);
		wlr_buffer_unlock(buffer);
	} else if (server->cursor_dirty) {
		screen_show(server, NULL, NULL);
	}
}

// The cursor moved (or the backend needs a commit for another reason).
static void handle_screen_needs_frame(struct wl_listener *listener, void *data) {
	struct vd_screen *screen = wl_container_of(listener, screen, needs_frame);
	struct vd_server *server = screen->server;
	if (!server->panel || server->screen != screen) {
		return;
	}
	if (server->flipping) {
		server->cursor_dirty = true;
	} else {
		screen_show(server, NULL, NULL);
	}
}

// Where the screen shows the cursor. wlr_cursor works in layout coordinates;
// ours are the screen's own (the view is the whole screen at 0,0).
static void cursor_sync(struct vd_server *server) {
	if (server->cursor == NULL || !server->panel || server->screen == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->layout, server->screen->output, &box);
	wlr_cursor_warp_closest(server->cursor, NULL, box.x + server->cursor_x,
		box.y + server->cursor_y);
}

static void screen_cursor_update(struct vd_server *server) {
	if (server->cursor == NULL) {
		return;
	}
	if (!server->panel || server->cursor_surface == NULL) {
		wlr_cursor_unset_image(server->cursor);
		if (gm_debug_cursor()) {
			wlr_log(WLR_INFO, "Cursor: hidden (%s)",
				!server->panel ? "no panel" : "the client set none");
		}
		return;
	}
	wlr_cursor_set_surface(server->cursor, server->cursor_surface,
		server->cursor_hotspot_x, server->cursor_hotspot_y);
	cursor_sync(server);
	if (gm_debug_cursor()) {
		wlr_log(WLR_INFO, "Cursor: on the screen, %dx%d hotspot %d,%d",
			server->cursor_surface->current.width,
			server->cursor_surface->current.height,
			server->cursor_hotspot_x, server->cursor_hotspot_y);
	}
}

static void drop_next_frame(struct vd_server *server) {
	wlr_buffer_unlock(server->next_buffer);
	server->next_buffer = NULL;
	if (server->next_feedback != NULL) {
		wlr_presentation_feedback_destroy(server->next_feedback);
		server->next_feedback = NULL;
	}
}

// Back to this machine's screen (no client): gamescope's output takes its
// native mode, as on native Game Mode.
static void panel_enter(struct vd_server *server) {
	if (server->panel || server->screen == NULL || server->streaming) {
		return;
	}
	struct wlr_output *output = server->screen->output;
	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	if (output->adaptive_sync_supported) {
		wlr_output_state_set_adaptive_sync_enabled(&state, true);
	}
	bool ok = wlr_output_commit_state(output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		// Almost always the seat session this screen came through: see
		// panel_take.
		wlr_log(server->panel_complained ? WLR_DEBUG : WLR_ERROR,
			"Panel: cannot enable %s", output->name);
		panel_lost(server);
		return;
	}
	server->panel = true;
	server->panel_complained = false; // a later loss is worth logging again
	// The modeset is a flip too: frames wait for it (sending one right
	// behind it failed with "a page-flip is already pending").
	server->flipping = true;
	wlr_log(WLR_INFO, "Panel: showing on %s, %dx%d@%.3fHz%s", output->name,
		output->width, output->height, output->refresh / 1000.0,
		output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED ? " VRR" : "");
	set_screen_size(server, output->width, output->height, output->refresh);
	struct vd_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->toplevel->base->surface->mapped) {
			view_outputs(server, view);
		}
	}
	screen_cursor_update(server);
	if (server->last_buffer != NULL) {
		server->next_buffer = wlr_buffer_lock(server->last_buffer); // after the modeset
	}
}

// A client takes over: the screen turns off, as when native Game Mode moves
// to a newly connected display.
static void panel_leave(struct vd_server *server) {
	if (!server->panel) {
		return;
	}
	server->panel = false;
	drop_next_frame(server);
	if (server->flip_feedback != NULL) {
		answer_now(server, server->flip_feedback);
		server->flip_feedback = NULL;
	}
	server->flipping = false;
	if (server->cursor != NULL) {
		wlr_cursor_unset_image(server->cursor);
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, false);
	wlr_output_commit_state(server->screen->output, &state);
	wlr_output_state_finish(&state);
	wlr_log(WLR_INFO, "Panel: %s off", server->screen->output->name);
	struct vd_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->toplevel->base->surface->mapped) {
			view_outputs(server, view);
		}
	}
}

// The screen to use: an external one if connected, else the built-in panel
// (native Game Mode's choice).
static void choose_screen(struct vd_server *server) {
	struct vd_screen *best = NULL, *screen;
	wl_list_for_each(screen, &server->screens, link) {
		if (best == NULL || (screen_internal(best->output) && !screen_internal(screen->output))) {
			best = screen;
		}
	}
	if (best == server->screen) {
		return;
	}
	bool was = server->panel;
	if (was) {
		panel_leave(server);
	}
	server->screen = best;
	if (was || !server->streaming) {
		panel_enter(server);
	}
}

static void handle_screen_destroy(struct wl_listener *listener, void *data) {
	struct vd_screen *screen = wl_container_of(listener, screen, destroy);
	struct vd_server *server = screen->server;
	if (server->screen == screen) {
		panel_leave(server);
		server->screen = NULL;
	}
	wl_list_remove(&screen->link);
	wl_list_remove(&screen->present.link);
	wl_list_remove(&screen->needs_frame.link);
	wl_list_remove(&screen->destroy.link);
	free(screen);
	choose_screen(server);
}

static void screen_added(struct vd_server *server, struct wlr_output *output) {
	if (output->non_desktop) {
		return;
	}
	wlr_output_init_render(output, server->allocator, server->renderer);
	struct vd_screen *screen = calloc(1, sizeof(*screen));
	screen->server = server;
	screen->output = output;
	screen->present.notify = handle_screen_present;
	wl_signal_add(&output->events.present, &screen->present);
	screen->needs_frame.notify = handle_screen_needs_frame;
	wl_signal_add(&output->events.needs_frame, &screen->needs_frame);
	screen->destroy.notify = handle_screen_destroy;
	wl_signal_add(&output->events.destroy, &screen->destroy);
	wl_list_insert(&server->screens, &screen->link);
	// In the layout before the global: a client that asks for this output's
	// xdg_output aborts wlroots if it is not in one (gamescope asks, so
	// panel mode brought the whole host down). The layout drops it again
	// when the output is destroyed.
	wlr_output_layout_add_auto(server->layout, output);
	wlr_output_create_global(output, server->display);
	wlr_log(WLR_INFO, "Panel: screen %s (%s)", output->name,
		output->description ? output->description : "");
	if (server->output != NULL) { // past start-up: hotplug
		choose_screen(server);
	}
}

// Panel mode's seat session (logind, through libseat), which is what the
// screens and input devices of this machine come through.
//
// It cannot be taken once at start-up and kept. The Game Mode session relogs
// several times in the first seconds of a boot, and once more whenever this
// service restarts (gamescope dies with our socket, so SDDM logs in again),
// and logind revokes the devices of the session that went away: every flip
// then fails with EACCES. Nor can we wait for a session to settle before
// coming up, because nothing settles until gamescope can connect to us.
//
// So the compositor starts without a screen and takes one afterwards, on a
// timer, dropping it and taking a fresh one whenever it stops working. The
// session that sticks is the one logind has as the user's display session
// when the relogins are over.
static void panel_retry_soon(struct vd_server *server, unsigned ms) {
	if (server->panel_timer != NULL) {
		wl_event_source_timer_update(server->panel_timer, ms);
	}
}

// Called from output and commit handlers, so the session cannot be destroyed
// here: the timer does it, out of wlroots' event handling.
static void panel_lost(struct vd_server *server) {
	if (server->session == NULL || server->panel_broken) {
		return;
	}
	server->panel_broken = true;
	panel_retry_soon(server, 0);
}

static void panel_drop(struct vd_server *server) {
	server->panel_broken = false;
	if (server->session == NULL) {
		return;
	}
	panel_leave(server);
	// The backends take their outputs and input devices with them, so our
	// screens and inputs go through handle_screen_destroy and the input
	// destroy handlers.
	if (server->drm_backend != NULL) {
		wlr_backend_destroy(server->drm_backend);
		server->drm_backend = NULL;
	}
	if (server->libinput_backend != NULL) {
		wlr_backend_destroy(server->libinput_backend);
		server->libinput_backend = NULL;
	}
	wl_list_remove(&server->session_active.link);
	wl_list_init(&server->session_active.link);
	wlr_session_destroy(server->session);
	server->session = NULL;
	server->panel_fails = 0;
}

// Takes the seat session and this machine's devices with it, without starting
// the backends: at start-up the multi-backend's own start does that, and it
// has to happen after the renderer and the allocator exist.
static bool panel_create(struct vd_server *server) {
	// libseat's logind backend takes XDG_SESSION_ID when it is set, which
	// is the session systemd started this service in -- by now often the
	// dead one. Without it, it asks logind for the user's display session.
	unsetenv("XDG_SESSION_ID");
	server->session = wlr_session_create(server->loop);
	if (server->session == NULL) {
		return false;
	}
	struct wlr_device *gpu;
	if (wlr_session_find_gpus(server->session, 1, &gpu) != 1) {
		return false;
	}
	server->drm_backend = wlr_drm_backend_create(server->session, gpu, NULL);
	if (server->drm_backend == NULL) {
		return false;
	}
	wlr_multi_backend_add(server->backend, server->drm_backend);
	// logind marks the session inactive when it ends, so this is how we
	// hear that it is gone; see handle_session_active.
	wl_signal_add(&server->session->events.active, &server->session_active);
	server->libinput_backend = wlr_libinput_backend_create(server->session);
	if (server->libinput_backend != NULL) {
		wlr_multi_backend_add(server->backend, server->libinput_backend);
	}
	return true;
}

// Takes it again while running, where the multi-backend is long since
// started: starting the DRM backend is what hands us its connectors
// (new_output, then choose_screen and panel_enter).
static bool panel_take(struct vd_server *server) {
	if (!panel_create(server)) {
		return false;
	}
	if (!wlr_backend_start(server->drm_backend)) {
		return false;
	}
	if (server->libinput_backend != NULL) {
		wlr_backend_start(server->libinput_backend);
	}
	return true;
}

// Whether the devices we took through the seat session are still ours. When
// SDDM logs in again, logind ends the session we took them through and revokes
// them: every flip then fails with EACCES, and nothing tells us -- libseat
// only errors when we call it, and the session is removed rather than
// deactivated, so its active signal may never fire. So ask the card.
//
// (Not libdrm's drmIsMaster: it reads a revoked device, which answers ENODEV,
// as still being ours.)
static bool panel_holds_drm(struct vd_server *server) {
	int fd = server->drm_backend != NULL ?
		wlr_backend_get_drm_fd(server->drm_backend) : -1;
	if (fd < 0) {
		return false;
	}
	struct drm_auth auth = {0};
	if (ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth) == 0) {
		return true;
	}
	// Master: magic 0 is simply not in its table (EINVAL). Not master:
	// EACCES. Revoked with the session: ENODEV.
	return errno == EINVAL;
}

static void handle_session_active(struct wl_listener *listener, void *data) {
	struct vd_server *server = wl_container_of(listener, server, session_active);
	if (server->session == NULL || server->session->active) {
		return;
	}
	// Either a VT switch (it comes back) or the session has ended, which is
	// what happens whenever SDDM logs in again. Look once it has had time
	// to come back.
	wlr_log(WLR_INFO, "Panel: seat session inactive");
	panel_retry_soon(server, 2000);
}

static int handle_panel_retry(void *data) {
	struct vd_server *server = data;
	if (server->session != NULL) {
		if (!server->panel_broken && panel_holds_drm(server)) {
			panel_retry_soon(server, 1000); // still ours: keep watch
			return 0;
		}
		wlr_log(WLR_INFO, "Panel: the seat session %s went away; "
			"taking the screen again", server->session->seat);
		panel_drop(server);
	}
	if (!panel_take(server) || server->screen == NULL) {
		if (!server->panel_complained) {
			wlr_log(WLR_INFO, "Panel: no screen yet; retrying");
			server->panel_complained = true;
		}
		panel_drop(server);
		panel_retry_soon(server, 2000);
		return 0;
	}
	panel_retry_soon(server, 1000);
	return 0;
}

// Input devices of this machine.

static void input_key(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[0]);
	struct wlr_keyboard_key_event *event = data;
	key(input->server, event->keycode, event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void input_motion(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[0]);
	struct wlr_pointer_motion_event *event = data;
	pointer_moved(input->server, event->delta_x, event->delta_y, false, 0, 0);
}

static void input_motion_absolute(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[1]);
	struct wlr_pointer_motion_absolute_event *event = data;
	int w, h;
	screen_size(input->server, &w, &h);
	pointer_moved(input->server, 0, 0, true, event->x * w, event->y * h);
}

static void input_button(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[2]);
	struct wlr_pointer_button_event *event = data;
	pointer_button(input->server, event->button,
		event->state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void input_axis(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[3]);
	struct wlr_pointer_axis_event *event = data;
	if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		pointer_axis(input->server, 0, event->delta, 0, event->delta_discrete);
	} else {
		pointer_axis(input->server, event->delta, 0, event->delta_discrete, 0);
	}
}

static void input_touch_down(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[0]);
	struct wlr_touch_down_event *event = data;
	touch(input->server, event->touch_id, GM_TOUCH_DOWN, event->x, event->y);
}

static void input_touch_motion(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[1]);
	struct wlr_touch_motion_event *event = data;
	touch(input->server, event->touch_id, GM_TOUCH_MOTION, event->x, event->y);
}

static void input_touch_up(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[2]);
	struct wlr_touch_up_event *event = data;
	touch(input->server, event->touch_id, GM_TOUCH_UP, 0, 0);
}

static void input_touch_cancel(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, events[3]);
	touch(input->server, 0, GM_TOUCH_CANCEL, 0, 0);
}

static void handle_input_destroy(struct wl_listener *listener, void *data) {
	struct vd_input *input = wl_container_of(listener, input, destroy);
	for (int i = 0; i < 4; i++) {
		if (input->events[i].notify != NULL) {
			wl_list_remove(&input->events[i].link);
		}
	}
	wl_list_remove(&input->destroy.link);
	wl_list_remove(&input->link);
	free(input);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
	struct vd_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	struct vd_input *input = calloc(1, sizeof(*input));
	input->server = server;
	input->device = device;
	struct wl_signal *signals[4] = {0};
	wl_notify_func_t handlers[4] = {0};
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: {
		struct wlr_keyboard *k = wlr_keyboard_from_input_device(device);
		signals[0] = &k->events.key;
		handlers[0] = input_key;
		break;
	}
	case WLR_INPUT_DEVICE_POINTER: {
		struct wlr_pointer *p = wlr_pointer_from_input_device(device);
		signals[0] = &p->events.motion;
		handlers[0] = input_motion;
		signals[1] = &p->events.motion_absolute;
		handlers[1] = input_motion_absolute;
		signals[2] = &p->events.button;
		handlers[2] = input_button;
		signals[3] = &p->events.axis;
		handlers[3] = input_axis;
		break;
	}
	case WLR_INPUT_DEVICE_TOUCH: {
		struct wlr_touch *t = wlr_touch_from_input_device(device);
		signals[0] = &t->events.down;
		handlers[0] = input_touch_down;
		signals[1] = &t->events.motion;
		handlers[1] = input_touch_motion;
		signals[2] = &t->events.up;
		handlers[2] = input_touch_up;
		signals[3] = &t->events.cancel;
		handlers[3] = input_touch_cancel;
		break;
	}
	default: // switches (lid), tablets: not used
		free(input);
		return;
	}
	for (int i = 0; i < 4; i++) {
		if (signals[i] != NULL) {
			input->events[i].notify = handlers[i];
			wl_signal_add(signals[i], &input->events[i]);
		}
	}
	input->destroy.notify = handle_input_destroy;
	wl_signal_add(&device->events.destroy, &input->destroy);
	wl_list_insert(&server->inputs, &input->link);
	wlr_log(WLR_INFO, "Input: %s", device->name ? device->name : "?");
}

// Drops the modifiers the video encoder cannot read from. AMD's colour
// compression (DCC) is the default for anything the GPU renders into, and
// every other consumer here copes with it — the 3D engine decompresses as it
// samples (PyroWave), so does a VA-API blit, and the display engine scans it
// out — but the encode block reads such a surface as raw bytes and turns the
// picture into compression blocks. Vulkan reports no modifier at all as
// encodable on this driver, which is not true (a plain one works), so the
// filter goes by what DCC actually is: AMD's modifier with its DCC bit set,
// which also carries the extra metadata plane.
static bool encodable_modifier(uint64_t mod) {
	return !IS_AMD_FMT_MOD(mod) || AMD_FMT_MOD_GET(DCC, mod) == 0;
}

// Ten-bit formats: gamescope composites at ten bits whenever it is offered
// one, and the client throws the extra bits away — VideoToolbox converts them
// to eight-bit on the way out of the decoder, which costs it about 3 ms a
// frame at 2560x1600. Leaving them unadvertised keeps everything eight-bit
// from gamescope's composite to the client's texture. GM_10BIT offers them
// again (for HDR, or to measure).
static bool wanted_format(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return getenv("GM_10BIT") != NULL;
	}
	return true;
}

static bool filter_formats(struct wlr_drm_format_set *set) {
	struct wlr_drm_format_set kept = {0};
	for (size_t i = 0; i < set->len; i++) {
		const struct wlr_drm_format *f = &set->formats[i];
		if (!wanted_format(f->format)) {
			continue;
		}
		for (size_t j = 0; j < f->len; j++) {
			if (encodable_modifier(f->modifiers[j]) &&
					!wlr_drm_format_set_add(&kept, f->format, f->modifiers[j])) {
				wlr_drm_format_set_finish(&kept);
				return false;
			}
		}
	}
	if (kept.len == 0) {
		wlr_drm_format_set_finish(&kept);
		return false;
	}
	wlr_drm_format_set_finish(set);
	*set = kept;
	return true;
}

static size_t filter_feedback(struct wlr_linux_dmabuf_feedback_v1 *feedback) {
	size_t n = feedback->tranches.size / sizeof(struct wlr_linux_dmabuf_feedback_v1_tranche);
	struct wlr_linux_dmabuf_feedback_v1_tranche *t = feedback->tranches.data;
	size_t dropped = 0;
	for (size_t i = 0; i < n; i++) {
		size_t before = 0;
		for (size_t j = 0; j < t[i].formats.len; j++) {
			before += t[i].formats.formats[j].len;
		}
		if (!filter_formats(&t[i].formats)) {
			continue; // nothing encodable in this tranche: leave it as it was
		}
		size_t after = 0;
		for (size_t j = 0; j < t[i].formats.len; j++) {
			after += t[i].formats.formats[j].len;
		}
		dropped += before - after;
	}
	return dropped;
}

// gamescope picks its buffers' modifier from the formats we advertise
// (linux-dmabuf v3: the flat list, no feedback). With a screen, only those
// its primary plane can scan out (and the renderer read) are offered, so
// frames go to the screen as they are instead of being copied (180 of 180
// frames copied at 2560x1600@180 before). The encoder imports these too, so
// the compressed modifiers are dropped as well (encodable_modifier): then
// gamescope composites into a buffer the encoder can read where it lies.
static void create_linux_dmabuf(struct vd_server *server) {
	if (wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DMABUF) == NULL) {
		return;
	}
	struct wlr_linux_dmabuf_v1 *dmabuf = NULL;
	struct wlr_linux_dmabuf_feedback_v1 feedback = {0};
	if (wlr_linux_dmabuf_feedback_v1_init_with_options(&feedback,
			&(struct wlr_linux_dmabuf_feedback_v1_init_options){
				.main_renderer = server->renderer,
				.scanout_primary_output = server->screen != NULL
					? server->screen->output : NULL,
			})) {
		size_t n = feedback.tranches.size / sizeof(struct wlr_linux_dmabuf_feedback_v1_tranche);
		struct wlr_linux_dmabuf_feedback_v1_tranche *t = feedback.tranches.data;
		if (server->screen != NULL && n >= 2 && t[0].formats.len > 0) {
			// The first tranche scans out; the last is everything the
			// renderer reads. Legacy clients get their union: narrow the
			// last to the first.
			struct wlr_drm_format_set scanout = {0};
			if (wlr_drm_format_set_intersect(&scanout, &t[0].formats, &t[n - 1].formats)) {
				wlr_drm_format_set_finish(&t[n - 1].formats);
				t[n - 1].formats = scanout;
			}
			wlr_log(WLR_INFO, "Panel: offering %zu formats %s can scan out",
				t[0].formats.len, server->screen->output->name);
		}
		size_t dropped = filter_feedback(&feedback);
		if (dropped > 0) {
			wlr_log(WLR_INFO, "Not offering %zu compressed modifiers: the video "
				"encoder cannot read them", dropped);
		}
		dmabuf = wlr_linux_dmabuf_v1_create(server->display, 4, &feedback);
		wlr_linux_dmabuf_feedback_v1_finish(&feedback);
	}
	if (dmabuf == NULL) {
		wlr_linux_dmabuf_v1_create_with_renderer(server->display, 4, server->renderer);
	}
}

/* ---- commands from other threads ---------------------------------------- */

enum cmd_type {
	CMD_QUIT,
	CMD_STREAM_START,
	CMD_STREAM_CONFIGURE,
	CMD_STREAM_STOP,
	CMD_KEYFRAME,
	CMD_BITRATE,
	CMD_PARALLELISM,
	CMD_ROTATION,
	CMD_MOTION,
	CMD_POSITION,
	CMD_BUTTON,
	CMD_AXIS,
	CMD_KEY,
	CMD_PAD_UPDATE,
	CMD_PAD_REMOVE,
	CMD_DISPLAY_GRID,
	CMD_VRR_POLICY,
	CMD_TOUCH,
};

struct cmd {
	enum cmd_type type;
	union {
		struct { enum gm_codec codec; int w, h, refresh, kbps, packet, flags; } stream;
		unsigned parallelism;
		struct { double x, y; int32_t v120_x, v120_y; } motion;
		struct { uint32_t code; bool pressed; } key;
		struct { uint8_t pad; struct gm_gamepad_state state; } pad;
		struct { uint64_t anchor_ns, interval_ns; } grid;
		int vrr_policy;
		struct { int32_t id; int phase; double x, y; } touch;
	};
};

static struct {
	pthread_mutex_t lock;
	struct cmd *items;
	size_t len, cap;
	int efd; // -1 while gm_run is not running
} queue = { .lock = PTHREAD_MUTEX_INITIALIZER, .efd = -1 };

static _Atomic uint32_t supported_codecs;
static _Atomic uint64_t heartbeat_ns;
static struct wl_event_source *heartbeat_timer;

uint64_t gm_heartbeat_ns(void) {
	return atomic_load(&heartbeat_ns);
}

static int handle_heartbeat(void *data) {
	atomic_store(&heartbeat_ns, gm_now_ns());
	wl_event_source_timer_update(heartbeat_timer, 500);
	return 0;
}

static void push(struct cmd cmd) {
	pthread_mutex_lock(&queue.lock);
	if (queue.efd >= 0) {
		if (queue.len == queue.cap) {
			queue.cap = queue.cap ? queue.cap * 2 : 64;
			queue.items = realloc(queue.items, queue.cap * sizeof(cmd));
		}
		queue.items[queue.len++] = cmd;
		uint64_t one = 1;
		if (write(queue.efd, &one, sizeof(one)) < 0) {
			// The counter saturating still leaves it readable.
		}
	}
	pthread_mutex_unlock(&queue.lock);
}

static void run_cmd(struct vd_server *server, const struct cmd *c) {
	switch (c->type) {
	case CMD_QUIT:
		wl_display_terminate(server->display);
		break;
	case CMD_STREAM_START:
		panel_leave(server);
		set_screen_size(server, c->stream.w, c->stream.h, c->stream.refresh);
		server->grid_anchor_ns = server->grid_interval_ns = 0; // until this client reports one
		server->grid_last_vblank_ns = 0;
		server->vrr_policy = 0;
		grid_leads_reset(server);
		if (server->encoder != NULL) {
			encoder_start(server->encoder, c->stream.codec, c->stream.kbps,
				server->refresh, c->stream.packet, c->stream.flags);
		}
		audio_start(server->audio);
		server->streaming = true;
		// The cursor image is only sent on change; resend for this client.
		free(server->cursor_sent);
		server->cursor_sent = NULL;
		send_cursor(server);
		// Nor is the rest of the mouse's state: tell this client from scratch.
		server->relative_sent = false;
		memset(server->clip_sent, 0, sizeof(server->clip_sent));
		server->cb->relative(server->cb->ctx, false);
		server->cb->cursor_clip(server->cb->ctx, 0, 0, 0, 0);
		report_pointer_soon(server);
		repeat_last_frame(server);
		break;
	case CMD_STREAM_CONFIGURE:
		set_screen_size(server, c->stream.w, c->stream.h, c->stream.refresh);
		if (server->encoder != NULL) {
			encoder_set_refresh(server->encoder, server->refresh);
		}
		break;
	case CMD_STREAM_STOP:
		server->streaming = false;
		server->vrr_policy = 0;
		server->grid_anchor_ns = server->grid_interval_ns = 0;
		server->grid_last_vblank_ns = 0;
		if (server->encoder != NULL) {
			encoder_stop(server->encoder);
		}
		audio_stop(server->audio);
		release_all_input(server);
		gamepads_remove_all(server->gamepads);
		panel_enter(server);
		if (server->session == NULL) {
			panel_retry_soon(server, 500); // never got a screen: try again
		}
		break;
	case CMD_DISPLAY_GRID:
		if (server->grid_interval_ns == 0 && c->grid.interval_ns != 0) {
			wlr_log(WLR_INFO, "Presenting on the client's display grid (%.3f Hz)",
				1e9 / (double)c->grid.interval_ns);
		}
		server->grid_anchor_ns = c->grid.anchor_ns;
		server->grid_interval_ns = c->grid.interval_ns;
		if (grid_trace() != NULL) {
			fprintf(grid_trace(), "g %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", gm_now_ns(),
				c->grid.anchor_ns, c->grid.interval_ns);
		}
		break;
	case CMD_VRR_POLICY:
		server->vrr_policy = c->vrr_policy;
		break;
	case CMD_KEYFRAME:
		if (server->encoder != NULL) {
			encoder_request_keyframe(server->encoder);
		}
		repeat_last_frame(server);
		break;
	case CMD_BITRATE:
		if (server->encoder != NULL) {
			encoder_set_bitrate(server->encoder, c->stream.kbps);
		}
		break;
	case CMD_PARALLELISM:
		if (server->encoder != NULL) {
			encoder_set_parallelism(server->encoder, c->parallelism);
		}
		break;
	case CMD_ROTATION:
		if (server->encoder != NULL) {
			encoder_set_rotation(server->encoder, c->parallelism);
		}
		break;
	case CMD_MOTION:
		client_motion(server, c->motion.x, c->motion.y);
		break;
	case CMD_POSITION: {
		int w, h;
		screen_size(server, &w, &h);
		client_position(server, c->motion.x * w, c->motion.y * h);
		break;
	}
	case CMD_BUTTON:
		pointer_button(server, c->key.code, c->key.pressed);
		break;
	case CMD_AXIS:
		pointer_axis(server, c->motion.x, c->motion.y, c->motion.v120_x,
			c->motion.v120_y);
		break;
	case CMD_KEY:
		key(server, c->key.code, c->key.pressed);
		break;
	case CMD_TOUCH:
		touch(server, c->touch.id, c->touch.phase, c->touch.x, c->touch.y);
		break;
	case CMD_PAD_UPDATE:
		gamepads_update(server->gamepads, c->pad.pad, &c->pad.state);
		break;
	case CMD_PAD_REMOVE:
		gamepads_remove(server->gamepads, c->pad.pad);
		break;
	}
}

static int handle_queue(int fd, uint32_t mask, void *data) {
	struct vd_server *server = data;
	uint64_t n;
	if (read(fd, &n, sizeof(n)) < 0) {
		return 0;
	}
	for (;;) {
		pthread_mutex_lock(&queue.lock);
		struct cmd *items = queue.items;
		size_t len = queue.len;
		queue.items = NULL;
		queue.len = queue.cap = 0;
		pthread_mutex_unlock(&queue.lock);
		if (len == 0) {
			free(items);
			break;
		}
		for (size_t i = 0; i < len; i++) {
			run_cmd(server, &items[i]);
		}
		free(items);
	}
	return 0;
}

void gm_quit(void) {
	push((struct cmd){ .type = CMD_QUIT });
}

void gm_stream_start(enum gm_codec codec, int width, int height,
		int refresh_mhz, int bitrate_kbps, int packet_bytes, int flags) {
	push((struct cmd){ .type = CMD_STREAM_START, .stream = { codec, width,
		height, refresh_mhz, bitrate_kbps, packet_bytes, flags } });
}

void gm_stream_configure(int width, int height, int refresh_mhz) {
	push((struct cmd){ .type = CMD_STREAM_CONFIGURE, .stream = { .w = width,
		.h = height, .refresh = refresh_mhz } });
}

void gm_stream_stop(void) {
	push((struct cmd){ .type = CMD_STREAM_STOP });
}

void gm_set_vrr_policy(int policy) {
	push((struct cmd){ .type = CMD_VRR_POLICY, .vrr_policy = policy });
}

void gm_set_display_grid(uint64_t anchor_ns, uint64_t interval_ns) {
	push((struct cmd){ .type = CMD_DISPLAY_GRID,
		.grid = { anchor_ns, interval_ns } });
}

void gm_request_keyframe(void) {
	push((struct cmd){ .type = CMD_KEYFRAME });
}

void gm_set_bitrate(int bitrate_kbps) {
	push((struct cmd){ .type = CMD_BITRATE, .stream = { .kbps = bitrate_kbps } });
}

void gm_set_parallelism(unsigned parts) {
	push((struct cmd){ .type = CMD_PARALLELISM, .parallelism = parts });
}

void gm_set_rotation(unsigned rotation) {
	push((struct cmd){ .type = CMD_ROTATION, .parallelism = rotation });
}

uint32_t gm_supported_codecs(void) {
	return atomic_load(&supported_codecs);
}

void gm_pointer_motion(double dx, double dy) {
	push((struct cmd){ .type = CMD_MOTION, .motion = { dx, dy, 0, 0 } });
}

void gm_pointer_position(double x, double y) {
	push((struct cmd){ .type = CMD_POSITION, .motion = { x, y, 0, 0 } });
}

void gm_pointer_button(uint32_t button, bool pressed) {
	push((struct cmd){ .type = CMD_BUTTON, .key = { button, pressed } });
}

void gm_pointer_axis(double dx, double dy, int32_t v120_x, int32_t v120_y) {
	push((struct cmd){ .type = CMD_AXIS,
		.motion = { dx, dy, v120_x, v120_y } });
}

void gm_key(uint32_t code, bool pressed) {
	push((struct cmd){ .type = CMD_KEY, .key = { code, pressed } });
}

void gm_touch(int32_t id, int phase, double x, double y) {
	push((struct cmd){ .type = CMD_TOUCH, .touch = { id, phase, x, y } });
}

void gm_gamepad_update(uint8_t pad, const struct gm_gamepad_state *state) {
	push((struct cmd){ .type = CMD_PAD_UPDATE, .pad = { pad, *state } });
}

void gm_gamepad_remove(uint8_t pad) {
	push((struct cmd){ .type = CMD_PAD_REMOVE, .pad = { .pad = pad } });
}

/* ---- main -------------------------------------------------------------- */

static int handle_sigchld(int signo, void *data) {
	struct vd_server *server = data;
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == server->child) {
			wlr_log(WLR_INFO, "Child exited, shutting down");
			server->child = 0;
			wl_display_terminate(server->display);
		}
	}
	return 0;
}

static int handle_stats(void *data) {
	struct vd_server *server = data;
	unsigned encoded = 0, dropped = 0;
	double encode_ms = 0;
	if (server->encoder != NULL) {
		encoder_take_stats(server->encoder, &encoded, &dropped, &encode_ms);
	}
	unsigned chunk_frames = 0, chunks = 0, patched = 0;
	double first_ms = 0, last_ms = 0;
	if (server->encoder != NULL) {
		encoder_take_chunk_stats(server->encoder, &chunk_frames, &chunks,
			&patched, &first_ms, &last_ms);
	}
	struct wlr_output *shown = server->panel ? server->screen->output : server->output;
	wlr_log(WLR_INFO, "stats: %u presented (%u copied), %u superseded, %u fence waits, "
		"%u encoded (%.2f ms avg), %u encoder drops, %s %dx%d",
		server->stats_presented, server->stats_copied, server->stats_superseded,
		server->stats_fence_waits, encoded, encode_ms, dropped,
		shown->name, shown->width, shown->height);
	if (chunk_frames > 0) {
		wlr_log(WLR_INFO, "stats: %u frames streamed in %u pieces, first piece "
			"%.2f ms after the frame was given to the GPU, bitstream done "
			"%.2f ms, %u bytes re-sent", chunk_frames, chunks, first_ms,
			last_ms, patched);
	}
	server->stats_presented = server->stats_superseded = 0;
	server->stats_fence_waits = server->stats_copied = 0;
	wl_event_source_timer_update(server->stats_timer, 1000);
	return 0;
}

static int handle_terminate(int signo, void *data) {
	wl_display_terminate(data);
	return 0;
}

int gm_run(const struct gm_config *config, const struct gm_callbacks *cb) {
	struct vd_server server = {
		.cb = cb,
		.width = config->width > 0 ? config->width : 1920,
		.height = config->height > 0 ? config->height : 1080,
		.refresh = config->refresh_mhz > 0 ? config->refresh_mhz : 120000,
	};
	wlr_log_init(config->debug ? WLR_DEBUG : WLR_INFO, NULL);
	wl_list_init(&server.views);
	wl_list_init(&server.screens);
	wl_list_init(&server.inputs);

	server.display = wl_display_create();
	server.loop = wl_display_get_event_loop(server.display);
	server.backend = wlr_multi_backend_create(server.loop);
	server.headless = wlr_headless_backend_create(server.loop);
	if (server.backend == NULL || server.headless == NULL) {
		return 1;
	}
	wlr_multi_backend_add(server.backend, server.headless);
	// Panel mode: this machine's screens and input devices, through the
	// seat session (logind) as native gamescope takes them. On whenever
	// there is a seat to take, so this machine's screen works as it does
	// under native Game Mode; GM_NO_PANEL, or no seat, leaves a host that
	// only streams. Rough edge: the screen also reaches clients as a
	// second wl_output. The session itself is taken once the event loop
	// runs and re-taken as needed, see panel_take.
	if (getenv("GM_NO_PANEL") != NULL) {
		wlr_log(WLR_INFO, "Panel: off (GM_NO_PANEL); streams only");
	} else {
		server.session_active.notify = handle_session_active;
		wl_list_init(&server.session_active.link);
		server.panel_timer = wl_event_loop_add_timer(server.loop,
			handle_panel_retry, &server);
		// Now, so that the screen exists before the dmabuf formats are
		// offered (create_linux_dmabuf): a client that binds without a
		// screen picks a modifier the display engine cannot scan out, and
		// every frame is copied. This session rarely survives the next
		// relogin, but the formats of the screen it finds do.
		panel_create(&server);
		panel_retry_soon(&server, 1000); // then watch it, see panel_take
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "Failed to create renderer");
		return 1;
	}
	wlr_renderer_init_wl_shm(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "Failed to create allocator");
		return 1;
	}

	int drm_fd = wlr_renderer_get_drm_fd(server.renderer);
	server.encoder = drm_fd >= 0 ? encoder_create(drm_fd, cb) : NULL;
	if (server.encoder == NULL) {
		wlr_log(WLR_ERROR, "No video encoder; streaming will show nothing");
	} else {
		atomic_store(&supported_codecs,
			encoder_supported_codecs(server.encoder));
	}
	server.audio = audio_create(cb);
	server.gamepads = gamepads_create(server.loop, cb);

	wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_primary_selection_v1_device_manager_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_single_pixel_buffer_manager_v1_create(server.display);
	wlr_fractional_scale_manager_v1_create(server.display, 1);
	server.presentation =
		wlr_presentation_create(server.display, server.backend, 2);

	server.layout = wlr_output_layout_create(server.display);
	server.cursor = wlr_cursor_create();
	if (server.cursor != NULL) {
		wlr_cursor_attach_output_layout(server.cursor, server.layout);
	}
	wlr_xdg_output_manager_v1_create(server.display, server.layout);
	server.new_output.notify = handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.new_input.notify = handle_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);

	struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(server.display, 6);
	server.new_toplevel.notify = handle_new_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &server.new_toplevel);
	server.decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server.display);
	server.new_decoration.notify = handle_new_decoration;
	wl_signal_add(&server.decoration_manager->events.new_toplevel_decoration,
		&server.new_decoration);

	server.seat = wlr_seat_create(server.display, "seat0");
	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER |
		WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
	server.cursor_hide_timer = wl_event_loop_add_timer(server.loop,
		handle_cursor_hide, &server);
	server.request_set_cursor.notify = handle_request_set_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
		&server.request_set_cursor);
	server.request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);
	server.request_set_primary_selection.notify =
		handle_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection,
		&server.request_set_primary_selection);
	if (!init_keyboard(&server)) {
		return 1;
	}

	server.relative_pointer =
		wlr_relative_pointer_manager_v1_create(server.display);
	server.constraints = wlr_pointer_constraints_v1_create(server.display);
	server.new_constraint.notify = handle_new_constraint;
	wl_signal_add(&server.constraints->events.new_constraint,
		&server.new_constraint);

	const char *socket = config->socket;
	if (socket != NULL) {
		if (wl_display_add_socket(server.display, socket) != 0) {
			wlr_log_errno(WLR_ERROR, "Creating Wayland socket %s", socket);
			return 1;
		}
	} else if ((socket = wl_display_add_socket_auto(server.display)) == NULL) {
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		return 1;
	}
	wlr_headless_add_output(server.headless, server.width, server.height);
	server.cursor_x = server.width / 2.0;
	server.cursor_y = server.height / 2.0;
	choose_screen(&server); // panel mode until a client connects
	create_linux_dmabuf(&server);

	setenv("WAYLAND_DISPLAY", socket, true);
	wlr_log(WLR_INFO, "Running on WAYLAND_DISPLAY=%s", socket);

	server.pace_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (server.pace_fd < 0) {
		wlr_log_errno(WLR_ERROR, "timerfd_create failed");
		return 1;
	}
	server.pace_source = wl_event_loop_add_fd(server.loop, server.pace_fd,
		WL_EVENT_READABLE, handle_pace, &server);

	int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	struct wl_event_source *queue_source = wl_event_loop_add_fd(server.loop,
		efd, WL_EVENT_READABLE, handle_queue, &server);
	pthread_mutex_lock(&queue.lock);
	queue.efd = efd;
	pthread_mutex_unlock(&queue.lock);

	heartbeat_timer = wl_event_loop_add_timer(server.loop, handle_heartbeat, NULL);
	handle_heartbeat(NULL);
	if (config->stats) {
		server.stats_timer =
			wl_event_loop_add_timer(server.loop, handle_stats, &server);
		wl_event_source_timer_update(server.stats_timer, 1000);
	}

	struct wl_event_source *sigchld = wl_event_loop_add_signal(server.loop,
		SIGCHLD, handle_sigchld, &server);
	struct wl_event_source *sigterm = wl_event_loop_add_signal(server.loop,
		SIGTERM, handle_terminate, server.display);
	struct wl_event_source *sigint = wl_event_loop_add_signal(server.loop,
		SIGINT, handle_terminate, server.display);

	if (config->child_argv != NULL && config->child_argv[0] != NULL) {
		server.child = fork();
		if (server.child == 0) {
			sigset_t set;
			sigemptyset(&set);
			sigprocmask(SIG_SETMASK, &set, NULL);
			execvp(config->child_argv[0], config->child_argv);
			perror("execvp");
			_exit(127);
		}
	}

	wl_display_run(server.display);

	if (server.child > 0) {
		kill(server.child, SIGTERM);
	}
	pthread_mutex_lock(&queue.lock);
	queue.efd = -1;
	free(queue.items);
	queue.items = NULL;
	queue.len = queue.cap = 0;
	pthread_mutex_unlock(&queue.lock);
	wl_event_source_remove(queue_source);
	close(efd);
	wl_event_source_remove(sigchld);
	wl_event_source_remove(sigterm);
	wl_event_source_remove(sigint);
	wl_event_source_remove(heartbeat_timer);
	heartbeat_timer = NULL;
	wl_event_source_remove(server.pace_source);
	close(server.pace_fd);
	if (server.stats_timer != NULL) {
		wl_event_source_remove(server.stats_timer);
	}
	frame_destroy(server.waiting);
	server.waiting = NULL; // the view's unmap below would free it again
	wlr_buffer_unlock(server.last_buffer);
	server.last_buffer = NULL;
	clear_cursor_surface(&server);
	wl_event_source_remove(server.cursor_hide_timer);
	free(server.cursor_sent);
	audio_destroy(server.audio);
	gamepads_destroy(server.gamepads);
	wl_display_destroy_clients(server.display);
	// Imported buffers are gone with the clients; the encoder can go now.
	encoder_destroy(server.encoder);

	// wlroots asserts that globals have no listeners left when the display
	// tears them down.
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_toplevel.link);
	wl_list_remove(&server.new_decoration.link);
	wl_list_remove(&server.new_constraint.link);
	wl_list_remove(&server.request_set_cursor.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_set_primary_selection.link);
	wl_list_remove(&server.keyboard_key.link);
	wl_list_remove(&server.keyboard_modifiers.link);
	wlr_keyboard_finish(&server.keyboard);
	wl_display_destroy(server.display);
	return 0;
}
