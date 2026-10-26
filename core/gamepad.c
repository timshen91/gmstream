// Client gamepads as uinput Xbox 360 controllers, which Steam Input picks up
// like real ones. Rumble requests from games come back as uinput force-
// feedback uploads and are forwarded to the client.

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "core.h"

#define MAX_PADS 16
#define MAX_EFFECTS 16

struct pad {
	struct gm_gamepads *pads;
	uint8_t index;
	int fd;
	struct wl_event_source *source;
	struct ff_rumble_effect effects[MAX_EFFECTS];
	uint16_t lengths[MAX_EFFECTS];
};

struct gm_gamepads {
	struct wl_event_loop *loop;
	const struct gm_callbacks *cb;
	struct pad *pads[MAX_PADS];
};

// W3C standard-mapping button index → evdev key; 0 for the analog triggers
// and the d-pad, which are axes on this controller.
static const uint16_t button_keys[17] = {
	BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, 0, 0,
	BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR, 0, 0, 0, 0, BTN_MODE,
};

static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
	struct input_event ev = { .type = type, .code = code, .value = value };
	if (write(fd, &ev, sizeof(ev)) < 0 && errno != EAGAIN) {
		wlr_log_errno(WLR_ERROR, "uinput write");
	}
}

static void abs_setup(int fd, uint16_t code, int min, int max, int fuzz,
		int flat) {
	struct uinput_abs_setup abs = {
		.code = code,
		.absinfo = { .minimum = min, .maximum = max, .fuzz = fuzz, .flat = flat },
	};
	ioctl(fd, UI_ABS_SETUP, &abs);
}

static int handle_pad_event(int fd, uint32_t mask, void *data) {
	struct pad *pad = data;
	struct input_event ev;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
			struct uinput_ff_upload up = { .request_id = (uint32_t)ev.value };
			if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &up) < 0) {
				continue;
			}
			int id = up.effect.id;
			if (id >= 0 && id < MAX_EFFECTS && up.effect.type == FF_RUMBLE) {
				pad->effects[id] = up.effect.u.rumble;
				pad->lengths[id] = up.effect.replay.length;
				up.retval = 0;
			} else {
				up.retval = -EINVAL;
			}
			ioctl(fd, UI_END_FF_UPLOAD, &up);
		} else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
			struct uinput_ff_erase er = { .request_id = (uint32_t)ev.value };
			if (ioctl(fd, UI_BEGIN_FF_ERASE, &er) < 0) {
				continue;
			}
			if (er.effect_id < MAX_EFFECTS) {
				memset(&pad->effects[er.effect_id], 0, sizeof(pad->effects[0]));
			}
			er.retval = 0;
			ioctl(fd, UI_END_FF_ERASE, &er);
		} else if (ev.type == EV_FF && ev.code < MAX_EFFECTS) {
			const struct ff_rumble_effect *e = &pad->effects[ev.code];
			bool play = ev.value > 0;
			pad->pads->cb->rumble(pad->pads->cb->ctx, pad->index,
				play ? e->strong_magnitude : 0, play ? e->weak_magnitude : 0,
				play ? (pad->lengths[ev.code] ? pad->lengths[ev.code] : 1000) : 0);
		}
	}
	return 0;
}

static struct pad *pad_create(struct gm_gamepads *pads, uint8_t index) {
	int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "Opening /dev/uinput");
		return NULL;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (size_t i = 0; i < sizeof(button_keys) / sizeof(button_keys[0]); i++) {
		if (button_keys[i] != 0) {
			ioctl(fd, UI_SET_KEYBIT, button_keys[i]);
		}
	}
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	static const uint16_t sticks[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
	for (size_t i = 0; i < 4; i++) {
		ioctl(fd, UI_SET_ABSBIT, sticks[i]);
		abs_setup(fd, sticks[i], -32768, 32767, 16, 128);
	}
	ioctl(fd, UI_SET_ABSBIT, ABS_Z);
	abs_setup(fd, ABS_Z, 0, 255, 0, 0);
	ioctl(fd, UI_SET_ABSBIT, ABS_RZ);
	abs_setup(fd, ABS_RZ, 0, 255, 0, 0);
	ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
	abs_setup(fd, ABS_HAT0X, -1, 1, 0, 0);
	ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);
	abs_setup(fd, ABS_HAT0Y, -1, 1, 0, 0);
	ioctl(fd, UI_SET_EVBIT, EV_FF);
	ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);

	struct uinput_setup setup = {
		.id = { .bustype = BUS_USB, .vendor = 0x045e, .product = 0x028e,
			.version = 0x110 },
		.name = "Microsoft X-Box 360 pad",
		.ff_effects_max = MAX_EFFECTS,
	};
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		wlr_log_errno(WLR_ERROR, "Creating uinput gamepad");
		close(fd);
		return NULL;
	}
	struct pad *pad = calloc(1, sizeof(*pad));
	pad->pads = pads;
	pad->index = index;
	pad->fd = fd;
	pad->source = wl_event_loop_add_fd(pads->loop, fd, WL_EVENT_READABLE,
		handle_pad_event, pad);
	wlr_log(WLR_INFO, "Gamepad %u connected", index);
	return pad;
}

static void pad_destroy(struct pad *pad) {
	wl_event_source_remove(pad->source);
	ioctl(pad->fd, UI_DEV_DESTROY);
	close(pad->fd);
	wlr_log(WLR_INFO, "Gamepad %u disconnected", pad->index);
	free(pad);
}

struct gm_gamepads *gamepads_create(struct wl_event_loop *loop,
		const struct gm_callbacks *cb) {
	struct gm_gamepads *pads = calloc(1, sizeof(*pads));
	pads->loop = loop;
	pads->cb = cb;
	return pads;
}

void gamepads_update(struct gm_gamepads *pads, uint8_t index,
		const struct gm_gamepad_state *s) {
	if (index >= MAX_PADS) {
		return;
	}
	if (pads->pads[index] == NULL) {
		pads->pads[index] = pad_create(pads, index);
		if (pads->pads[index] == NULL) {
			return;
		}
	}
	int fd = pads->pads[index]->fd;
	for (size_t i = 0; i < sizeof(button_keys) / sizeof(button_keys[0]); i++) {
		if (button_keys[i] != 0) {
			emit(fd, EV_KEY, button_keys[i], (s->buttons >> i) & 1);
		}
	}
	emit(fd, EV_ABS, ABS_X, s->lx);
	emit(fd, EV_ABS, ABS_Y, s->ly);
	emit(fd, EV_ABS, ABS_RX, s->rx);
	emit(fd, EV_ABS, ABS_RY, s->ry);
	emit(fd, EV_ABS, ABS_Z, s->lt);
	emit(fd, EV_ABS, ABS_RZ, s->rt);
	emit(fd, EV_ABS, ABS_HAT0X,
		(int)((s->buttons >> 15) & 1) - (int)((s->buttons >> 14) & 1));
	emit(fd, EV_ABS, ABS_HAT0Y,
		(int)((s->buttons >> 13) & 1) - (int)((s->buttons >> 12) & 1));
	emit(fd, EV_SYN, SYN_REPORT, 0);
}

void gamepads_remove(struct gm_gamepads *pads, uint8_t index) {
	if (index < MAX_PADS && pads->pads[index] != NULL) {
		pad_destroy(pads->pads[index]);
		pads->pads[index] = NULL;
	}
}

void gamepads_remove_all(struct gm_gamepads *pads) {
	for (uint8_t i = 0; i < MAX_PADS; i++) {
		gamepads_remove(pads, i);
	}
}

void gamepads_destroy(struct gm_gamepads *pads) {
	if (pads != NULL) {
		gamepads_remove_all(pads);
		free(pads);
	}
}
