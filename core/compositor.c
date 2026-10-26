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

#include <assert.h>
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

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
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
	struct wl_event_source *fence_source; // NULL once the GPU work is done
	int fence_fd;
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
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output *output;
	struct wlr_output_layout *layout;
	struct wlr_presentation *presentation;

	struct wlr_seat *seat;
	struct wlr_keyboard keyboard;
	double cursor_x, cursor_y;
	uint32_t buttons_down[8]; // pressed BTN_* codes, to release on stop
	int n_buttons_down;
	struct wlr_relative_pointer_manager_v1 *relative_pointer;
	struct wlr_pointer_constraints_v1 *constraints;
	struct wlr_pointer_constraint_v1 *active_constraint;
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

	struct gm_encoder *encoder;
	struct gm_audio *audio;
	struct gm_gamepads *gamepads;
	bool streaming;

	pid_t child;

	// -S: per-second delivery statistics.
	struct wl_event_source *stats_timer;
	unsigned stats_presented, stats_superseded, stats_fence_waits;

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

	if (gm_now_ns() < frame->present_ns) {
		struct itimerspec its = {
			.it_value = {
				.tv_sec = frame->present_ns / 1000000000,
				.tv_nsec = frame->present_ns % 1000000000,
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

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct vd_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *output = data;
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
// Always as a VRR display (refresh 0): gamescope's Wayland backend reports
// VRR support only then, and Steam hides its VRR toggle otherwise. With
// Steam's toggle off gamescope still paces to these timestamps (its vblank
// timer, at the output mode's refresh), so a client grid still sets where
// frames land; with it on, gamescope presents as soon as frames are ready.
static void send_feedback(struct vd_server *server, struct wlr_surface *surface,
		uint64_t when_ns, bool frame) {
	struct wlr_presentation_feedback *feedback =
		wlr_presentation_surface_sampled(surface);
	if (feedback == NULL) {
		return;
	}
	if (server->grid_interval_ns != 0) {
		when_ns = grid_vblank(server, gm_now_ns(), frame);
	}
	struct wlr_presentation_event event = {
		.output = server->output,
		.tv_sec = when_ns / 1000000000,
		.tv_nsec = when_ns % 1000000000,
		.refresh = 0,
	};
	wlr_presentation_feedback_send_presented(feedback, &event);
	wlr_presentation_feedback_destroy(feedback);
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

	// Decide when a VRR panel would show this frame: now, or one maximum-
	// refresh interval after the previous flip; a frame replacing one still
	// queued takes over its slot.
	uint64_t slot;
	if (server->waiting != NULL) {
		server->stats_superseded++;
		slot = server->waiting->present_ns;
	} else {
		uint64_t period = 1000000000000ull / (uint64_t)server->refresh;
		slot = gm_now_ns();
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
	send_feedback(server, surface, slot, true);

	// gamescope sends the whole composited frame on its root surface when
	// run with --force-composition; anything on subsurfaces is not shown.
	frame_destroy(server->waiting);
	server->waiting = frame_create(server, surface);
	if (server->waiting != NULL) {
		server->waiting->present_ns = slot;
	}
	try_present(server);
}

static void handle_view_map(struct wl_listener *listener, void *data) {
	struct vd_view *view = wl_container_of(listener, view, map);
	struct vd_server *server = view->server;
	wlr_log(WLR_INFO, "Mapped toplevel %p '%s'", (void *)view,
		view->toplevel->title ? view->toplevel->title : "");
	wlr_surface_send_enter(view->toplevel->base->surface, server->output);
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

static void handle_cursor_commit(struct wl_listener *listener, void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, cursor_commit);
	server->cursor_hotspot_x -= server->cursor_surface->current.dx;
	server->cursor_hotspot_y -= server->cursor_surface->current.dy;
	send_cursor(server);
}

static void handle_cursor_destroy(struct wl_listener *listener, void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, cursor_destroy);
	clear_cursor_surface(server);
	send_cursor(server);
}

static void handle_request_set_cursor(struct wl_listener *listener,
		void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (event->seat_client != server->seat->pointer_state.focused_client) {
		return;
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

static bool pointer_locked(struct vd_server *server) {
	return server->active_constraint != NULL &&
		server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

static void pointer_moved(struct vd_server *server, double dx, double dy,
		bool absolute, double x, double y) {
	uint32_t time = now_msec();
	if (absolute) {
		dx = x - server->cursor_x;
		dy = y - server->cursor_y;
	}
	wlr_relative_pointer_manager_v1_send_relative_motion(
		server->relative_pointer, server->seat, (uint64_t)time * 1000,
		dx, dy, dx, dy);
	// Absolute input keeps being tracked while the pointer is locked, so the
	// relative motion derived from it stays true (gamescope locks the pointer
	// whenever Steam's cursor is hidden). Relative input under a lock only
	// moves the client's own cursor.
	if (absolute || !pointer_locked(server)) {
		int w = server->output != NULL ? server->output->width : server->width;
		int h = server->output != NULL ? server->output->height : server->height;
		x = absolute ? x : server->cursor_x + dx;
		y = absolute ? y : server->cursor_y + dy;
		server->cursor_x = x < 0 ? 0 : x > w - 1 ? w - 1 : x;
		server->cursor_y = y < 0 ? 0 : y > h - 1 ? h - 1 : y;
	}
	if (!pointer_locked(server)) {
		// The view is the whole screen at (0, 0): layout == surface coords.
		wlr_seat_pointer_notify_motion(server->seat, time,
			server->cursor_x, server->cursor_y);
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

static void release_all_input(struct vd_server *server) {
	while (server->n_buttons_down > 0) {
		pointer_button(server, server->buttons_down[0], false);
	}
	while (server->keyboard.num_keycodes > 0) {
		key(server, server->keyboard.keycodes[0], false);
	}
}

static void handle_constraint_destroy(struct wl_listener *listener,
		void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, constraint_destroy);
	bool was_locked = pointer_locked(server);
	wl_list_remove(&server->constraint_destroy.link);
	server->active_constraint = NULL;
	if (was_locked) {
		wlr_log(WLR_INFO, "Pointer unlocked");
		server->cb->pointer_lock(server->cb->ctx, false);
	}
}

static void handle_new_constraint(struct wl_listener *listener, void *data) {
	struct vd_server *server =
		wl_container_of(listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	bool was_locked = pointer_locked(server);
	// There is only one surface to constrain to, so honor the newest one.
	if (server->active_constraint != NULL) {
		wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
		wl_list_remove(&server->constraint_destroy.link);
	}
	server->active_constraint = constraint;
	server->constraint_destroy.notify = handle_constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &server->constraint_destroy);
	wlr_pointer_constraint_v1_send_activated(constraint);
	if (pointer_locked(server) != was_locked) {
		wlr_log(WLR_INFO, "Pointer %s", was_locked ? "unlocked" : "locked");
		server->cb->pointer_lock(server->cb->ctx, !was_locked);
	}
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

/* ---- commands from other threads ---------------------------------------- */

enum cmd_type {
	CMD_QUIT,
	CMD_STREAM_START,
	CMD_STREAM_CONFIGURE,
	CMD_STREAM_STOP,
	CMD_KEYFRAME,
	CMD_BITRATE,
	CMD_MOTION,
	CMD_POSITION,
	CMD_BUTTON,
	CMD_AXIS,
	CMD_KEY,
	CMD_PAD_UPDATE,
	CMD_PAD_REMOVE,
	CMD_DISPLAY_GRID,
};

struct cmd {
	enum cmd_type type;
	union {
		struct { enum gm_codec codec; int w, h, refresh, kbps; } stream;
		struct { double x, y; int32_t v120_x, v120_y; } motion;
		struct { uint32_t code; bool pressed; } key;
		struct { uint8_t pad; struct gm_gamepad_state state; } pad;
		struct { uint64_t anchor_ns, interval_ns; } grid;
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
		set_screen_size(server, c->stream.w, c->stream.h, c->stream.refresh);
		server->grid_anchor_ns = server->grid_interval_ns = 0; // until this client reports one
		server->grid_last_vblank_ns = 0;
		if (server->encoder != NULL) {
			encoder_start(server->encoder, c->stream.codec, c->stream.kbps,
				server->refresh);
		}
		audio_start(server->audio);
		server->streaming = true;
		// The cursor image is only sent on change; resend for this client.
		free(server->cursor_sent);
		server->cursor_sent = NULL;
		send_cursor(server);
		server->cb->pointer_lock(server->cb->ctx, pointer_locked(server));
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
		server->grid_anchor_ns = server->grid_interval_ns = 0;
		server->grid_last_vblank_ns = 0;
		if (server->encoder != NULL) {
			encoder_stop(server->encoder);
		}
		audio_stop(server->audio);
		release_all_input(server);
		gamepads_remove_all(server->gamepads);
		break;
	case CMD_DISPLAY_GRID:
		if (server->grid_interval_ns == 0 && c->grid.interval_ns != 0) {
			wlr_log(WLR_INFO, "Presenting on the client's display grid (%.3f Hz)",
				1e9 / (double)c->grid.interval_ns);
		}
		server->grid_anchor_ns = c->grid.anchor_ns;
		server->grid_interval_ns = c->grid.interval_ns;
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
	case CMD_MOTION:
		pointer_moved(server, c->motion.x, c->motion.y, false, 0, 0);
		break;
	case CMD_POSITION: {
		int w = server->output != NULL ? server->output->width : server->width;
		int h = server->output != NULL ? server->output->height : server->height;
		pointer_moved(server, 0, 0, true, c->motion.x * w, c->motion.y * h);
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
		int refresh_mhz, int bitrate_kbps) {
	push((struct cmd){ .type = CMD_STREAM_START, .stream = { codec, width,
		height, refresh_mhz, bitrate_kbps } });
}

void gm_stream_configure(int width, int height, int refresh_mhz) {
	push((struct cmd){ .type = CMD_STREAM_CONFIGURE, .stream = { .w = width,
		.h = height, .refresh = refresh_mhz } });
}

void gm_stream_stop(void) {
	push((struct cmd){ .type = CMD_STREAM_STOP });
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
	wlr_log(WLR_INFO, "stats: %u presented, %u superseded, %u fence waits, "
		"%u encoded (%.2f ms avg), %u encoder drops, %dx%d",
		server->stats_presented, server->stats_superseded,
		server->stats_fence_waits, encoded, encode_ms, dropped,
		server->output->width, server->output->height);
	server->stats_presented = server->stats_superseded = 0;
	server->stats_fence_waits = 0;
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

	server.display = wl_display_create();
	server.loop = wl_display_get_event_loop(server.display);
	server.backend = wlr_headless_backend_create(server.loop);
	if (server.backend == NULL) {
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "Failed to create renderer");
		return 1;
	}
	wlr_renderer_init_wl_shm(server.renderer, server.display);
	if (wlr_renderer_get_texture_formats(server.renderer,
			WLR_BUFFER_CAP_DMABUF) != NULL) {
		wlr_linux_dmabuf_v1_create_with_renderer(server.display, 4,
			server.renderer);
	}
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
	wlr_xdg_output_manager_v1_create(server.display, server.layout);
	server.new_output.notify = handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(server.display, 6);
	server.new_toplevel.notify = handle_new_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &server.new_toplevel);
	server.decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server.display);
	server.new_decoration.notify = handle_new_decoration;
	wl_signal_add(&server.decoration_manager->events.new_toplevel_decoration,
		&server.new_decoration);

	server.seat = wlr_seat_create(server.display, "seat0");
	wlr_seat_set_capabilities(server.seat,
		WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
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
	wlr_headless_add_output(server.backend, server.width, server.height);
	server.cursor_x = server.width / 2.0;
	server.cursor_y = server.height / 2.0;

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
