// Riptide encoding (riptide/, our codec; PROTOCOL.md "Riptide"): each frame
// predicted from the encoder's own decode of the previous one, which is what
// the client has as long as nothing is lost, and coded in stripes, horizontal
// bands each coded as an image of its own. The GPU finishes the stripes one
// after another and each goes out as soon as it is done, so the frame is on
// the wire while the rest is coded, and the client decodes each while the next
// arrives. A client that misses packets says which; the encoder replays its
// decode (riptide_encoder_report_loss) so the next frames predict from what it
// actually has. Every frame is still at most bitrate / refresh rate bytes.
//
// The compositor thread hands over gamescope's dma-buf (a duplicated file
// descriptor, so no wlr_buffer crosses threads) and a sync file exported from
// it, the fence gamescope's rendering signals. The worker thread imports the
// dma-buf into Vulkan (cached per buffer) and encodes it straight from there.

#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <vulkan/vulkan.h>
#include <riptide/riptide.h>
#include <wlr/util/log.h>

#include "core.h"

// The packet payload when the session gives none (rtenc_start); with our
// datagram header it fits a 1400-byte UDP datagram.
#define DEFAULT_PACKET 1100
#define MAX_IMPORTS 8
// Inter frames: the frames kept for replaying a client's losses (~133 ms at 60
// fps; a report older than that makes the next frame intra), and the reports
// waiting for the worker.
#define INTER_HISTORY 8
#define FRAME_MAP 64
#define MAX_LOSSES 16
#define MAX_RANGES 64
// Inter frames: each stripe's rows (the last takes the rest). Shorter stripes
// go out sooner but cost bits (on Euro Truck at 1080p, 256 rows cost 0.05 dB
// PSNR-Y, 128 rows 0.15) and a fixed amount of GPU work each, on both ends.
// Measured at 2560x1600, 120 Hz (present to decoded on the client, with both
// GPUs kept awake): 384 rows after a first stripe of 128 were best or close to
// it, light or heavy GPU load, 100 or 300 Mb/s (5.3-8.6 ms; 256 rows 5.4-9.2,
// one stripe 6.4-9.7). GM_RIPTIDE_STRIPE_ROWS (0 for one stripe) and
// GM_RIPTIDE_FIRST_ROWS.
#define STRIPE_ROWS 384
#define FIRST_STRIPE_ROWS 128

struct loss {
	uint32_t frame_id;
	uint16_t count;
	uint16_t n_ranges;
	uint16_t ranges[2 * MAX_RANGES];
};

// An encoded frame's id and what the encoder calls it.
struct frame_entry {
	bool valid;
	uint32_t frame_id, number;
	uint16_t packets;
};

struct dmabuf {
	int fds[WLR_DMABUF_MAX_PLANES];
	uint32_t offsets[WLR_DMABUF_MAX_PLANES], strides[WLR_DMABUF_MAX_PLANES];
	int n_planes;
	int width, height;
	uint32_t format;
	uint64_t modifier;
};

struct job {
	bool valid;
	struct dmabuf buf;
	int sync_fd; // -1: none
	int width, height; // encoded size
	uint64_t slot_ns;
	uint64_t submit_ns; // handed to the worker (GM_DEBUG_ENCODE)
};

struct import {
	dev_t dev;
	ino_t ino;
	int width, height;
	uint32_t format;
	uint64_t modifier;
	riptide_image image;
	uint64_t used;
};

struct gm_rtenc {
	const struct gm_callbacks *cb;
	riptide_device device;
	struct gm_gpuwarm *warm;

	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	// Protected by `lock`.
	bool quit, active;
	struct job pending;
	int bitrate_kbps, refresh_mhz, packet_bytes;
	unsigned stripes; // the client's choice (rtenc_set_stripes); 0: ours
	bool intra_requested;
	struct loss losses[MAX_LOSSES];
	int n_losses;
	unsigned stats_encoded, stats_dropped;
	uint64_t stats_encode_ns;

	// Worker thread only.
	riptide_encoder encoder;
	int enc_width, enc_height;
	bool inter_on; // the encoder is set up (riptide_encoder_enable_inter)
	size_t inter_packet;
	unsigned inter_stripes; // what it was set up for
	int stripe_rows, first_rows;
	struct frame_entry frames[FRAME_MAP];
	uint32_t repaired; // newest frame id whose loss report was taken in
	uint8_t *received;
	size_t received_cap;
	struct import imports[MAX_IMPORTS];
	uint64_t use_counter;
	uint32_t next_frame_id;
	riptide_packet *packets;
	size_t packets_cap;
	uint8_t *bitstream, *out;
	size_t bitstream_cap, out_cap;
};

static void dmabuf_close(struct dmabuf *b) {
	for (int i = 0; i < b->n_planes; i++) {
		if (b->fds[i] >= 0) {
			close(b->fds[i]);
		}
	}
	b->n_planes = 0;
}

static void job_clear(struct job *j) {
	if (!j->valid) {
		return;
	}
	dmabuf_close(&j->buf);
	if (j->sync_fd >= 0) {
		close(j->sync_fd);
	}
	j->valid = false;
}

static VkFormat vk_format(uint32_t drm_format) {
	switch (drm_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	}
	return VK_FORMAT_UNDEFINED;
}

/* ---- compositor thread -------------------------------------------------- */

bool rtenc_submit(struct gm_rtenc *pw, struct wlr_buffer *buffer,
		int width, int height, uint64_t slot_ns) {
	struct wlr_dmabuf_attributes attr;
	if (!wlr_buffer_get_dmabuf(buffer, &attr) || vk_format(attr.format) == VK_FORMAT_UNDEFINED) {
		return false; // gamescope's single-pixel fills: nothing to encode
	}
	struct job job = { .valid = true, .sync_fd = -1, .width = width & ~1,
		.height = height & ~1, .slot_ns = slot_ns, .submit_ns = gm_now_ns() };
	job.buf = (struct dmabuf){ .n_planes = attr.n_planes, .width = attr.width,
		.height = attr.height, .format = attr.format, .modifier = attr.modifier };
	for (int i = 0; i < attr.n_planes; i++) {
		job.buf.fds[i] = fcntl(attr.fd[i], F_DUPFD_CLOEXEC, 0);
		job.buf.offsets[i] = attr.offset[i];
		job.buf.strides[i] = attr.stride[i];
	}
	// The fence for gamescope's rendering into this buffer.
	struct dma_buf_export_sync_file exp = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };
	if (ioctl(attr.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) == 0) {
		job.sync_fd = exp.fd;
	}

	pthread_mutex_lock(&pw->lock);
	if (!pw->active) {
		pthread_mutex_unlock(&pw->lock);
		job_clear(&job);
		return true;
	}
	if (pw->pending.valid) {
		job_clear(&pw->pending);
		pw->stats_dropped++;
	}
	pw->pending = job;
	pthread_cond_signal(&pw->cond);
	pthread_mutex_unlock(&pw->lock);
	return true;
}

/* ---- worker thread ------------------------------------------------------ */

// Measuring (GM_DEBUG_ENCODE=<file>): per encoded frame "e slot submitted
// started gpu_done packetized first", host ns: its slot, when the compositor
// handed it over, when the worker started on it, when the GPU work was done,
// when the packets were ready, when the first stripe's were (inter frames;
// 0 otherwise), and when the encode call had submitted the GPU work.
// GM_DEBUG_INTER: log loss reports as they are taken in.
static bool debug_inter(void) {
	static int on = -1;
	if (on < 0) {
		on = getenv("GM_DEBUG_INTER") != NULL;
	}
	return on;
}

static void profile_line(void *userdata, const char *msg) {
	(void)userdata;
	if (strstr(msg, "Heap") == NULL) {
		wlr_log(WLR_INFO, "Riptide GPU %s", msg);
	}
}

static FILE *encode_trace(void) {
	static FILE *f;
	static bool opened;
	if (!opened) {
		opened = true;
		const char *path = getenv("GM_DEBUG_ENCODE");
		f = path != NULL ? fopen(path, "w") : NULL;
		if (f != NULL) {
			setvbuf(f, NULL, _IOLBF, 0);
		}
	}
	return f;
}

// Takes ownership of the job's file descriptors either way.
static riptide_image import_dmabuf(struct gm_rtenc *pw, struct dmabuf *b) {
	struct stat st;
	if (fstat(b->fds[0], &st) != 0) {
		return NULL;
	}
	struct import *slot = &pw->imports[0];
	for (int i = 0; i < MAX_IMPORTS; i++) {
		struct import *imp = &pw->imports[i];
		if (imp->image != NULL && imp->dev == st.st_dev && imp->ino == st.st_ino &&
				imp->width == b->width && imp->height == b->height &&
				imp->format == b->format && imp->modifier == b->modifier) {
			imp->used = ++pw->use_counter;
			return imp->image;
		}
		if (imp->used < slot->used) {
			slot = imp; // least recently used
		}
	}
	if (slot->image != NULL) {
		riptide_image_destroy(slot->image);
		slot->image = NULL;
	}

	VkSubresourceLayout layouts[WLR_DMABUF_MAX_PLANES] = { 0 };
	for (int i = 0; i < b->n_planes; i++) {
		layouts[i].offset = b->offsets[i];
		layouts[i].rowPitch = b->strides[i];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.drmFormatModifier = b->modifier,
		.drmFormatModifierPlaneCount = (uint32_t)b->n_planes,
		.pPlaneLayouts = layouts,
	};
	VkImageCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &modifier,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = vk_format(b->format),
		.extent = { (uint32_t)b->width, (uint32_t)b->height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	riptide_image_create_info info = {
		.device = pw->device,
		.external_handle = (riptide_os_handle)b->fds[0],
		.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.image_create_info = &ci,
	};
	riptide_image image = NULL;
	riptide_result r = riptide_image_create(&info, &image);
	if (r != RIPTIDE_SUCCESS) {
		wlr_log(WLR_ERROR, "Riptide: importing a %dx%d dma-buf (modifier 0x%llx) failed: %d",
			b->width, b->height, (unsigned long long)b->modifier, r);
		return NULL;
	}
	b->fds[0] = -1; // the import owns it now
	*slot = (struct import){ .dev = st.st_dev, .ino = st.st_ino, .width = b->width,
		.height = b->height, .format = b->format, .modifier = b->modifier,
		.image = image, .used = ++pw->use_counter };
	return image;
}

static bool ensure_encoder(struct gm_rtenc *pw, int width, int height) {
	if (pw->encoder != NULL && pw->enc_width == width && pw->enc_height == height) {
		return true;
	}
	if (pw->encoder != NULL) {
		riptide_encoder_destroy(pw->encoder);
		pw->encoder = NULL;
	}
	riptide_encoder_create_info info = { .device = pw->device, .width = width,
		.height = height, .chroma = RIPTIDE_CHROMA_SUBSAMPLING_420 };
	if (riptide_encoder_create(&info, &pw->encoder) != RIPTIDE_SUCCESS) {
		wlr_log(WLR_ERROR, "Riptide: creating a %dx%d encoder failed", width, height);
		pw->encoder = NULL;
		return false;
	}
	pw->enc_width = width;
	pw->enc_height = height;
	pw->inter_on = false; // a new encoder starts without (encode_one turns them on)
	wlr_log(WLR_INFO, "Encoding Riptide %dx%d", width, height);
	return true;
}

static void *grow(void *p, size_t *cap, size_t need, size_t elem) {
	if (*cap >= need) {
		return p;
	}
	*cap = need * 2;
	return realloc(p, *cap * elem);
}

// Sends a stripe's packets to the callback: n packets numbered from first,
// its inter packets first.
static void emit(struct gm_rtenc *pw, struct job *job, uint32_t frame_id, const riptide_packet *packets,
		const uint8_t *bitstream, size_t first, size_t n, size_t n_inter, int stripe, int stripes,
		bool predicted, uint64_t ready_ns) {
	size_t total = 0;
	for (size_t i = 0; i < n; i++) {
		total += 2 + packets[i].size;
	}
	pw->out = grow(pw->out, &pw->out_cap, total, 1);
	size_t off = 0;
	for (size_t i = 0; i < n; i++) {
		uint16_t len = (uint16_t)packets[i].size;
		memcpy(pw->out + off, &len, 2);
		memcpy(pw->out + off + 2, bitstream + packets[i].offset, len);
		off += 2 + len;
	}
	struct gm_riptide_packets out = {
		.data = pw->out,
		.len = total,
		.frame_id = frame_id,
		.first = (uint16_t)first,
		.count = (uint16_t)n,
		.stripe = (uint8_t)stripe,
		.stripes = (uint8_t)stripes,
		.first_rows = (uint16_t)pw->first_rows,
		.stripe_rows = (uint16_t)pw->stripe_rows,
		.inter = (uint8_t)n_inter,
		.predicted = predicted,
		.repaired = pw->repaired,
		.slot_ns = job->slot_ns,
		.encoded_ns = ready_ns,
		.width = (uint16_t)job->width,
		.height = (uint16_t)job->height,
	};
	pw->cb->riptide(pw->cb->ctx, &out);
}

// Inter frames: the client's loss reports, oldest first, into the encoder.
static void take_losses(struct gm_rtenc *pw) {
	struct loss losses[MAX_LOSSES];
	pthread_mutex_lock(&pw->lock);
	int n = pw->n_losses;
	memcpy(losses, pw->losses, sizeof(losses[0]) * (size_t)n);
	pw->n_losses = 0;
	pthread_mutex_unlock(&pw->lock);
	for (int k = 0; k < n; k++) {
		struct loss *l = &losses[k];
		if ((int32_t)(l->frame_id - pw->repaired) <= 0) {
			continue; // taken in already (the client repeats a report until it hears)
		}
		struct frame_entry *f = &pw->frames[l->frame_id % FRAME_MAP];
		pw->repaired = l->frame_id;
		if (!f->valid || f->frame_id != l->frame_id) {
			riptide_encoder_request_intra(pw->encoder);
			wlr_log(WLR_INFO, "Riptide: loss report for unknown frame %u; next frame intra", l->frame_id);
			continue;
		}
		pw->received = grow(pw->received, &pw->received_cap, f->packets, 1);
		memset(pw->received, 1, f->packets);
		for (int r = 0; r < l->n_ranges; r++) {
			uint32_t first = l->ranges[2 * r], len = l->ranges[2 * r + 1];
			for (uint32_t i = first; i < first + len && i < f->packets; i++) {
				pw->received[i] = 0;
			}
		}
		riptide_result r = riptide_encoder_report_loss(pw->encoder, f->number, pw->received, f->packets);
		if (debug_inter()) {
			wlr_log(WLR_INFO, "Riptide: frame %u loss report (%u ranges): %s", l->frame_id, l->n_ranges,
				r == RIPTIDE_SUCCESS ? "replayed" : "too old, next frame intra");
		}
	}
}

// The stripes for a frame `height` rows tall: the first's rows and every
// other's (the last takes the rest; 0, 0: one stripe). stripes 0 is ours: a
// short first stripe, then STRIPE_ROWS; else that many, about equal (rows on
// 64-row boundaries, at least 128).
static void stripe_layout(int height, unsigned stripes, int *first, int *rows) {
	if (stripes == 0) {
		const char *rows_env = getenv("GM_RIPTIDE_STRIPE_ROWS"), *first_env = getenv("GM_RIPTIDE_FIRST_ROWS");
		*rows = rows_env != NULL ? atoi(rows_env) : STRIPE_ROWS;
		*first = first_env != NULL ? atoi(first_env) : FIRST_STRIPE_ROWS;
	} else {
		*rows = (height / (int)stripes) & ~63;
		*first = *rows;
	}
	if (stripes == 1 || *rows < 128 || height < *first + 128) {
		*first = *rows = 0;
	}
}

static void encode_one(struct gm_rtenc *pw, struct job *job, int bitrate_kbps,
		int refresh_mhz, size_t packet, unsigned stripes) {
	riptide_image image = import_dmabuf(pw, &job->buf);
	if (image == NULL || !ensure_encoder(pw, job->width, job->height)) {
		return;
	}
	gpuwarm_touch(pw->warm);
	if (!pw->inter_on || packet != pw->inter_packet || stripes != pw->inter_stripes) {
		int first, rows;
		stripe_layout(job->height, stripes, &first, &rows);
		riptide_inter_info ii = { .enable = true, .packet_boundary = packet, .history = INTER_HISTORY,
			.stripe_rows = rows, .first_stripe_rows = first };
		pw->inter_on = riptide_encoder_enable_inter(pw->encoder, &ii) == RIPTIDE_SUCCESS;
		if (!pw->inter_on) {
			wlr_log(WLR_ERROR, "Riptide: setting up stripes of %d rows (first %d) failed", rows, first);
			return;
		}
		pw->stripe_rows = rows;
		pw->first_rows = first;
		pw->inter_packet = packet;
		pw->inter_stripes = stripes;
		memset(pw->frames, 0, sizeof(pw->frames));
		int count = riptide_encoder_get_stripe_count(pw->encoder);
		char text[64];
		snprintf(text, sizeof(text), "%d stripes (%d, then %d rows)", count, first ? first : job->height,
			rows ? rows : job->height);
		pw->cb->encode_format(pw->cb->ctx, text);
		wlr_log(WLR_INFO, "Riptide: %s", text);
	}
	take_losses(pw);
	pthread_mutex_lock(&pw->lock);
	bool intra = pw->intra_requested;
	pw->intra_requested = false;
	pthread_mutex_unlock(&pw->lock);
	if (intra) {
		riptide_encoder_request_intra(pw->encoder);
	}
	uint64_t fence_ns = 0;
	if (getenv("GM_DEBUG_FENCEWAIT") != NULL && job->sync_fd >= 0) {
		// Measuring: when gamescope's rendering of the frame is done (the CPU
		// waits for it here instead of the GPU).
		struct pollfd p = { .fd = job->sync_fd, .events = POLLIN };
		poll(&p, 1, 100);
		fence_ns = gm_now_ns();
	}
	riptide_sync_object sync = NULL;
	if (job->sync_fd >= 0) {
		riptide_sync_object_create_info si = { .device = pw->device,
			.external_handle = (riptide_os_handle)job->sync_fd,
			.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
			.semaphore_type = VK_SEMAPHORE_TYPE_BINARY,
			.import_flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT };
		if (riptide_sync_object_create(&si, &sync) == RIPTIDE_SUCCESS) {
			job->sync_fd = -1; // owned by the sync object now
		} else {
			sync = NULL;
		}
	}

	// Each frame gets its share of the bitrate, exactly.
	int fps_milli = refresh_mhz > 0 ? refresh_mhz : 60000;
	size_t budget = (size_t)((int64_t)bitrate_kbps * 1000 / 8 * 1000 / fps_milli);
	riptide_rate_control rc = { .maximum_bitstream_size = budget };
	riptide_gpu_external_reference ref = { .image = image,
		.queue_family_index = VK_QUEUE_FAMILY_FOREIGN_EXT };
	riptide_gpu_sync_operation acquire = { .images = &ref, .num_images = 1,
		.sync = { .semaphore = sync ? riptide_sync_object_get_semaphore(sync) : VK_NULL_HANDLE } };
	riptide_gpu_sync_operation release = { .images = &ref, .num_images = 1 };
	riptide_scaled_encode_info si = {
		.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.intermediate_plane_format = VK_FORMAT_R8_UNORM,
		.ycbcr_chroma_midpoint = 0.5f,
	};
	riptide_image_get_image_view(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &si.view);

	uint64_t start = gm_now_ns();
	riptide_result r = riptide_encoder_encode_gpu_scaled(pw->encoder, &acquire, &release, &si, &rc);
	uint64_t submitted = gm_now_ns();
	uint32_t frame_id = pw->next_frame_id++;
	uint64_t first_done = 0, gpu_done = 0, done = 0;
	if (r == RIPTIDE_SUCCESS) {
		bool predicted = false;
		uint32_t number = 0;
		riptide_encoder_get_frame_info(pw->encoder, &number, &predicted);
		int stripes = riptide_encoder_get_stripe_count(pw->encoder);
		size_t cap = budget / 256 + 1024;
		pw->packets = grow(pw->packets, &pw->packets_cap, cap, sizeof(*pw->packets));
		pw->bitstream = grow(pw->bitstream, &pw->bitstream_cap, budget + 65536, 1);
		size_t first = 0;
		for (int k = 0; k < stripes && r == RIPTIDE_SUCCESS; k++) {
			// Blocks until the GPU has finished this stripe, not the frame.
			size_t n = 0, n_inter = 0;
			r = riptide_encoder_packetize_stripe(pw->encoder, k, packet, pw->packets, pw->packets_cap, &n,
				&n_inter, pw->bitstream, pw->bitstream_cap);
			if (r != RIPTIDE_SUCCESS) {
				break;
			}
			uint64_t ready = gm_now_ns();
			if (k == 0) {
				first_done = ready;
			}
			emit(pw, job, frame_id, pw->packets, pw->bitstream, first, n, n_inter, k, stripes,
				predicted, ready);
			uint64_t emitted = gm_now_ns();
			first += n;
			// The next frame's reference (submitted; the GPU does it after this frame).
			riptide_encoder_decode_stripe(pw->encoder, k);
			if (encode_trace() != NULL) {
				fprintf(encode_trace(), "s %d %" PRIu64 " %" PRIu64 " %" PRIu64 " %zu\n", k, ready - start,
					emitted - start, gm_now_ns() - start, n);
			}
		}
		gpu_done = done = gm_now_ns();
		if (r == RIPTIDE_SUCCESS) {
			pw->frames[frame_id % FRAME_MAP] = (struct frame_entry){ .valid = true, .frame_id = frame_id,
				.number = number, .packets = (uint16_t)first };
		}
	}
	if (sync != NULL) {
		riptide_sync_object_destroy(sync);
	}
	if (r != RIPTIDE_SUCCESS) {
		wlr_log(WLR_ERROR, "Riptide: encode failed: %d", r);
		riptide_encoder_request_intra(pw->encoder);
		return;
	}
	if (encode_trace() != NULL) {
		fprintf(encode_trace(), "e %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
			job->slot_ns, job->submit_ns, start, gpu_done, done, first_done, submitted, fence_ns);
	}

	static unsigned profiled;
	if (getenv("GM_DEBUG_RTPROF") != NULL && ++profiled % 240 == 0) {
		// Measuring: the encoder's GPU time per pass, averaged since the last report.
		riptide_device_report_performance_stats(pw->device, profile_line, NULL, true);
	}
	pthread_mutex_lock(&pw->lock);
	pw->stats_encoded++;
	pw->stats_encode_ns += done - start;
	pthread_mutex_unlock(&pw->lock);
}

static void *worker(void *data) {
	struct gm_rtenc *pw = data;
	pthread_mutex_lock(&pw->lock);
	while (!pw->quit) {
		if (!pw->pending.valid) {
			pthread_cond_wait(&pw->cond, &pw->lock);
			continue;
		}
		struct job job = pw->pending;
		pw->pending.valid = false;
		int bitrate = pw->bitrate_kbps, refresh = pw->refresh_mhz;
		size_t packet = pw->packet_bytes > 0 ? (size_t)pw->packet_bytes : DEFAULT_PACKET;
		unsigned stripes = pw->stripes;
		pthread_mutex_unlock(&pw->lock);
		encode_one(pw, &job, bitrate, refresh, packet, stripes);
		job_clear(&job);
		pthread_mutex_lock(&pw->lock);
	}
	pthread_mutex_unlock(&pw->lock);
	return NULL;
}

/* ---- setup -------------------------------------------------------------- */

// The one encoder, for gm_riptide_loss.
static struct gm_rtenc *current;
static pthread_mutex_t current_lock = PTHREAD_MUTEX_INITIALIZER;

void gm_riptide_loss(uint32_t frame_id, uint16_t count, const uint16_t *ranges, size_t n_ranges) {
	pthread_mutex_lock(&current_lock);
	struct gm_rtenc *pw = current;
	if (pw != NULL) {
		pthread_mutex_lock(&pw->lock);
		if (pw->n_losses < MAX_LOSSES) {
			struct loss *l = &pw->losses[pw->n_losses++];
			l->frame_id = frame_id;
			l->count = count;
			l->n_ranges = (uint16_t)(n_ranges < MAX_RANGES ? n_ranges : MAX_RANGES);
			memcpy(l->ranges, ranges, sizeof(uint16_t) * 2 * l->n_ranges);
			if (n_ranges > MAX_RANGES) {
				// Too fragmented to list: count everything after the listed ones lost.
				l->ranges[2 * MAX_RANGES - 1] = 0xffff;
			}
		}
		pthread_mutex_unlock(&pw->lock);
	}
	pthread_mutex_unlock(&current_lock);
}

struct gm_rtenc *rtenc_create(const struct gm_callbacks *cb) {
	struct gm_rtenc *pw = calloc(1, sizeof(*pw));
	pw->cb = cb;
	pw->pending.sync_fd = -1;
	// A high-priority async compute queue: at normal priority the encode
	// waited behind everything else on the GPU (with Steam's overlay up over
	// a 117 fps game, 14–15 ms per frame instead of ~2, so only ~60 frames a
	// second got encoded). Above MEDIUM needs CAP_SYS_NICE (setcap on the
	// binary, as SteamOS does for gamescope); without it the driver gives
	// MEDIUM.
	riptide_result r = riptide_create_device_by_compat(0, 0, NULL, NULL, NULL,
		VK_QUEUE_GLOBAL_PRIORITY_REALTIME, &pw->device);
	if (r != RIPTIDE_SUCCESS) {
		r = riptide_create_default_device(&pw->device);
	}
	if (r != RIPTIDE_SUCCESS) {
		wlr_log(WLR_INFO, "Riptide unavailable (Vulkan device: %d)", r);
		free(pw);
		return NULL;
	}
	wlr_log(WLR_INFO, "Riptide compute queue priority: %d (MEDIUM %d, HIGH %d, REALTIME %d)",
		riptide_device_get_global_priority(pw->device), VK_QUEUE_GLOBAL_PRIORITY_MEDIUM,
		VK_QUEUE_GLOBAL_PRIORITY_HIGH, VK_QUEUE_GLOBAL_PRIORITY_REALTIME);
	pthread_mutex_init(&pw->lock, NULL);
	pthread_cond_init(&pw->cond, NULL);
	pw->warm = gpuwarm_create();
	pthread_create(&pw->thread, NULL, worker, pw);
	pthread_mutex_lock(&current_lock);
	current = pw;
	pthread_mutex_unlock(&current_lock);
	wlr_log(WLR_INFO, "Riptide encoder ready");
	return pw;
}

void rtenc_destroy(struct gm_rtenc *pw) {
	if (pw == NULL) {
		return;
	}
	pthread_mutex_lock(&current_lock);
	if (current == pw) {
		current = NULL;
	}
	pthread_mutex_unlock(&current_lock);
	pthread_mutex_lock(&pw->lock);
	pw->quit = true;
	pthread_cond_signal(&pw->cond);
	pthread_mutex_unlock(&pw->lock);
	pthread_join(pw->thread, NULL);
	gpuwarm_destroy(pw->warm);
	job_clear(&pw->pending);
	for (int i = 0; i < MAX_IMPORTS; i++) {
		if (pw->imports[i].image != NULL) {
			riptide_image_destroy(pw->imports[i].image);
		}
	}
	if (pw->encoder != NULL) {
		riptide_encoder_destroy(pw->encoder);
	}
	riptide_device_destroy(pw->device);
	free(pw->packets);
	free(pw->bitstream);
	free(pw->out);
	free(pw->received);
	free(pw);
}

void rtenc_start(struct gm_rtenc *pw, int bitrate_kbps, int refresh_mhz, int packet_bytes) {
	pthread_mutex_lock(&pw->lock);
	pw->active = true;
	pw->n_losses = 0;
	pw->bitrate_kbps = bitrate_kbps;
	pw->refresh_mhz = refresh_mhz;
	pw->packet_bytes = packet_bytes;
	pthread_mutex_unlock(&pw->lock);
}

void rtenc_stop(struct gm_rtenc *pw) {
	pthread_mutex_lock(&pw->lock);
	pw->active = false;
	job_clear(&pw->pending);
	pthread_mutex_unlock(&pw->lock);
}

void rtenc_set_rate(struct gm_rtenc *pw, int bitrate_kbps, int refresh_mhz) {
	pthread_mutex_lock(&pw->lock);
	if (bitrate_kbps > 0) {
		pw->bitrate_kbps = bitrate_kbps;
	}
	if (refresh_mhz > 0) {
		pw->refresh_mhz = refresh_mhz;
	}
	pthread_mutex_unlock(&pw->lock);
}

void rtenc_set_stripes(struct gm_rtenc *pw, unsigned stripes) {
	pthread_mutex_lock(&pw->lock);
	pw->stripes = stripes;
	pthread_mutex_unlock(&pw->lock);
}

void rtenc_request_intra(struct gm_rtenc *pw) {
	pthread_mutex_lock(&pw->lock);
	pw->intra_requested = true;
	pthread_mutex_unlock(&pw->lock);
}

void rtenc_take_stats(struct gm_rtenc *pw, unsigned *encoded,
		unsigned *dropped, uint64_t *encode_ns) {
	pthread_mutex_lock(&pw->lock);
	*encoded = pw->stats_encoded;
	*dropped = pw->stats_dropped;
	*encode_ns = pw->stats_encode_ns;
	pw->stats_encoded = pw->stats_dropped = 0;
	pw->stats_encode_ns = 0;
	pthread_mutex_unlock(&pw->lock);
}
