// PyroWave encoding (github.com/Themaister/pyrowave): an intra-only GPU
// wavelet codec whose rate control makes every frame at most a fixed number
// of bytes, so its transmit time is bounded. Encode takes well under a
// millisecond, color conversion included.
//
// The compositor thread hands over gamescope's dma-buf (a duplicated file
// descriptor, so no wlr_buffer crosses threads) and a sync file exported from
// it, the fence gamescope's rendering signals. The worker thread imports the
// dma-buf into Vulkan (cached per buffer), encodes it straight from there, and
// cuts the bitstream into packets that each fit one datagram. The frame goes
// to the callback as the packets back to back, each after a u16 length.

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <vulkan/vulkan.h>
#include <pyrowave/pyrowave.h>
#include <wlr/util/log.h>

#include "core.h"

// PyroWave packet payload by default; with our datagram header it fits both
// a WebTransport datagram (~1200 bytes) and a plain UDP one (1400). Plain UDP
// sessions set their own (pyrowave_start): over a Thunderbolt link (MTU ~64 KB)
// each datagram costs ~40 µs whatever its size, so a frame in 1100-byte
// packets took ~8 ms to arrive and in ~64 KB packets ~0.2 ms.
#define DEFAULT_PACKET 1100
#define MAX_IMPORTS 8

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
};

struct import {
	dev_t dev;
	ino_t ino;
	int width, height;
	uint32_t format;
	uint64_t modifier;
	pyrowave_image image;
	uint64_t used;
};

struct gm_pyrowave {
	const struct gm_callbacks *cb;
	pyrowave_device device;

	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	// Protected by `lock`.
	bool quit, active;
	struct job pending;
	int bitrate_kbps, refresh_mhz, packet_bytes;
	unsigned stats_encoded, stats_dropped;
	uint64_t stats_encode_ns;

	// Worker thread only.
	pyrowave_encoder encoder;
	int enc_width, enc_height;
	struct import imports[MAX_IMPORTS];
	uint64_t use_counter;
	uint32_t next_frame_id;
	pyrowave_packet *packets;
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

bool pyrowave_submit(struct gm_pyrowave *pw, struct wlr_buffer *buffer,
		int width, int height, uint64_t slot_ns) {
	struct wlr_dmabuf_attributes attr;
	if (!wlr_buffer_get_dmabuf(buffer, &attr) || vk_format(attr.format) == VK_FORMAT_UNDEFINED) {
		return false; // gamescope's single-pixel fills: nothing to encode
	}
	struct job job = { .valid = true, .sync_fd = -1, .width = width & ~1,
		.height = height & ~1, .slot_ns = slot_ns };
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

// Takes ownership of the job's file descriptors either way.
static pyrowave_image import_dmabuf(struct gm_pyrowave *pw, struct dmabuf *b) {
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
		pyrowave_image_destroy(slot->image);
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
	pyrowave_image_create_info info = {
		.device = pw->device,
		.external_handle = (pyrowave_os_handle)b->fds[0],
		.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.image_create_info = &ci,
	};
	pyrowave_image image = NULL;
	pyrowave_result r = pyrowave_image_create(&info, &image);
	if (r != PYROWAVE_SUCCESS) {
		wlr_log(WLR_ERROR, "PyroWave: importing a %dx%d dma-buf (modifier 0x%llx) failed: %d",
			b->width, b->height, (unsigned long long)b->modifier, r);
		return NULL;
	}
	b->fds[0] = -1; // the import owns it now
	*slot = (struct import){ .dev = st.st_dev, .ino = st.st_ino, .width = b->width,
		.height = b->height, .format = b->format, .modifier = b->modifier,
		.image = image, .used = ++pw->use_counter };
	return image;
}

static bool ensure_encoder(struct gm_pyrowave *pw, int width, int height) {
	if (pw->encoder != NULL && pw->enc_width == width && pw->enc_height == height) {
		return true;
	}
	if (pw->encoder != NULL) {
		pyrowave_encoder_destroy(pw->encoder);
		pw->encoder = NULL;
	}
	pyrowave_encoder_create_info info = { .device = pw->device, .width = width,
		.height = height, .chroma = PYROWAVE_CHROMA_SUBSAMPLING_420 };
	if (pyrowave_encoder_create(&info, &pw->encoder) != PYROWAVE_SUCCESS) {
		wlr_log(WLR_ERROR, "PyroWave: creating a %dx%d encoder failed", width, height);
		pw->encoder = NULL;
		return false;
	}
	pw->enc_width = width;
	pw->enc_height = height;
	wlr_log(WLR_INFO, "Encoding PyroWave %dx%d", width, height);
	return true;
}

static void *grow(void *p, size_t *cap, size_t need, size_t elem) {
	if (*cap >= need) {
		return p;
	}
	*cap = need * 2;
	return realloc(p, *cap * elem);
}

static void encode_one(struct gm_pyrowave *pw, struct job *job, int bitrate_kbps,
		int refresh_mhz, size_t packet) {
	pyrowave_image image = import_dmabuf(pw, &job->buf);
	if (image == NULL || !ensure_encoder(pw, job->width, job->height)) {
		return;
	}
	pyrowave_sync_object sync = NULL;
	if (job->sync_fd >= 0) {
		pyrowave_sync_object_create_info si = { .device = pw->device,
			.external_handle = (pyrowave_os_handle)job->sync_fd,
			.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
			.semaphore_type = VK_SEMAPHORE_TYPE_BINARY,
			.import_flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT };
		if (pyrowave_sync_object_create(&si, &sync) == PYROWAVE_SUCCESS) {
			job->sync_fd = -1; // owned by the sync object now
		} else {
			sync = NULL;
		}
	}

	// Each frame gets its share of the bitrate, exactly.
	int fps_milli = refresh_mhz > 0 ? refresh_mhz : 60000;
	size_t budget = (size_t)((int64_t)bitrate_kbps * 1000 / 8 * 1000 / fps_milli);
	pyrowave_rate_control rc = { .maximum_bitstream_size = budget };
	pyrowave_gpu_external_reference ref = { .image = image,
		.queue_family_index = VK_QUEUE_FAMILY_FOREIGN_EXT };
	pyrowave_gpu_sync_operation acquire = { .images = &ref, .num_images = 1,
		.sync = { .semaphore = sync ? pyrowave_sync_object_get_semaphore(sync) : VK_NULL_HANDLE } };
	pyrowave_gpu_sync_operation release = { .images = &ref, .num_images = 1 };
	pyrowave_scaled_encode_info si = {
		.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.intermediate_plane_format = VK_FORMAT_R8_UNORM,
		.ycbcr_chroma_midpoint = 0.5f,
	};
	pyrowave_image_get_image_view(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &si.view);

	uint64_t start = gm_now_ns();
	pyrowave_result r = pyrowave_encoder_encode_gpu_scaled(pw->encoder, &acquire, &release, &si, &rc);
	size_t n = 0;
	if (r == PYROWAVE_SUCCESS) {
		// Blocks until the GPU is done.
		r = pyrowave_encoder_compute_num_packets(pw->encoder, packet, &n);
	}
	if (sync != NULL) {
		pyrowave_sync_object_destroy(sync);
	}
	if (r != PYROWAVE_SUCCESS) {
		wlr_log(WLR_ERROR, "PyroWave: encode failed: %d", r);
		return;
	}
	pw->packets = grow(pw->packets, &pw->packets_cap, n, sizeof(*pw->packets));
	pw->bitstream = grow(pw->bitstream, &pw->bitstream_cap, budget + n * 64 + 4096, 1);
	size_t out_packets = 0;
	if (pyrowave_encoder_packetize(pw->encoder, pw->packets, packet, &out_packets,
			pw->bitstream, pw->bitstream_cap) != PYROWAVE_SUCCESS) {
		wlr_log(WLR_ERROR, "PyroWave: packetize failed");
		return;
	}
	size_t total = 0;
	for (size_t i = 0; i < out_packets; i++) {
		total += 2 + pw->packets[i].size;
	}
	pw->out = grow(pw->out, &pw->out_cap, total, 1);
	size_t off = 0;
	for (size_t i = 0; i < out_packets; i++) {
		uint16_t len = (uint16_t)pw->packets[i].size;
		memcpy(pw->out + off, &len, 2);
		memcpy(pw->out + off + 2, pw->bitstream + pw->packets[i].offset, len);
		off += 2 + len;
	}
	uint64_t done = gm_now_ns();
	struct gm_video_packet out = {
		.data = pw->out,
		.len = total,
		.frame_id = pw->next_frame_id++,
		.slot_ns = job->slot_ns,
		.encoded_ns = done,
		.width = (uint16_t)job->width,
		.height = (uint16_t)job->height,
		.keyframe = true, // every frame stands alone
		.codec = GM_CODEC_PYROWAVE,
	};
	pw->cb->video(pw->cb->ctx, &out);

	pthread_mutex_lock(&pw->lock);
	pw->stats_encoded++;
	pw->stats_encode_ns += done - start;
	pthread_mutex_unlock(&pw->lock);
}

static void *worker(void *data) {
	struct gm_pyrowave *pw = data;
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
		pthread_mutex_unlock(&pw->lock);
		encode_one(pw, &job, bitrate, refresh, packet);
		job_clear(&job);
		pthread_mutex_lock(&pw->lock);
	}
	pthread_mutex_unlock(&pw->lock);
	return NULL;
}

/* ---- setup -------------------------------------------------------------- */

struct gm_pyrowave *pyrowave_create(const struct gm_callbacks *cb) {
	struct gm_pyrowave *pw = calloc(1, sizeof(*pw));
	pw->cb = cb;
	pw->pending.sync_fd = -1;
	// A high-priority async compute queue: at normal priority the encode
	// waited behind everything else on the GPU (with Steam's overlay up over
	// a 117 fps game, 14–15 ms per frame instead of ~2, so only ~60 frames a
	// second got encoded). Above MEDIUM needs CAP_SYS_NICE (setcap on the
	// binary, as SteamOS does for gamescope); without it the driver gives
	// MEDIUM.
	pyrowave_result r = pyrowave_create_device_by_compat(0, 0, NULL, NULL, NULL,
		VK_QUEUE_GLOBAL_PRIORITY_REALTIME, &pw->device);
	if (r != PYROWAVE_SUCCESS) {
		r = pyrowave_create_default_device(&pw->device);
	}
	if (r != PYROWAVE_SUCCESS) {
		wlr_log(WLR_INFO, "PyroWave unavailable (Vulkan device: %d)", r);
		free(pw);
		return NULL;
	}
	wlr_log(WLR_INFO, "PyroWave compute queue priority: %d (MEDIUM %d, HIGH %d, REALTIME %d)",
		pyrowave_device_get_global_priority(pw->device), VK_QUEUE_GLOBAL_PRIORITY_MEDIUM,
		VK_QUEUE_GLOBAL_PRIORITY_HIGH, VK_QUEUE_GLOBAL_PRIORITY_REALTIME);
	pthread_mutex_init(&pw->lock, NULL);
	pthread_cond_init(&pw->cond, NULL);
	pthread_create(&pw->thread, NULL, worker, pw);
	wlr_log(WLR_INFO, "PyroWave encoder ready");
	return pw;
}

void pyrowave_destroy(struct gm_pyrowave *pw) {
	if (pw == NULL) {
		return;
	}
	pthread_mutex_lock(&pw->lock);
	pw->quit = true;
	pthread_cond_signal(&pw->cond);
	pthread_mutex_unlock(&pw->lock);
	pthread_join(pw->thread, NULL);
	job_clear(&pw->pending);
	for (int i = 0; i < MAX_IMPORTS; i++) {
		if (pw->imports[i].image != NULL) {
			pyrowave_image_destroy(pw->imports[i].image);
		}
	}
	if (pw->encoder != NULL) {
		pyrowave_encoder_destroy(pw->encoder);
	}
	pyrowave_device_destroy(pw->device);
	free(pw->packets);
	free(pw->bitstream);
	free(pw->out);
	free(pw);
}

void pyrowave_start(struct gm_pyrowave *pw, int bitrate_kbps, int refresh_mhz,
		int packet_bytes) {
	pthread_mutex_lock(&pw->lock);
	pw->active = true;
	pw->bitrate_kbps = bitrate_kbps;
	pw->refresh_mhz = refresh_mhz;
	pw->packet_bytes = packet_bytes;
	pthread_mutex_unlock(&pw->lock);
}

void pyrowave_stop(struct gm_pyrowave *pw) {
	pthread_mutex_lock(&pw->lock);
	pw->active = false;
	job_clear(&pw->pending);
	pthread_mutex_unlock(&pw->lock);
}

void pyrowave_set_rate(struct gm_pyrowave *pw, int bitrate_kbps, int refresh_mhz) {
	pthread_mutex_lock(&pw->lock);
	if (bitrate_kbps > 0) {
		pw->bitrate_kbps = bitrate_kbps;
	}
	if (refresh_mhz > 0) {
		pw->refresh_mhz = refresh_mhz;
	}
	pthread_mutex_unlock(&pw->lock);
}

void pyrowave_take_stats(struct gm_pyrowave *pw, unsigned *encoded,
		unsigned *dropped, uint64_t *encode_ns) {
	pthread_mutex_lock(&pw->lock);
	*encoded = pw->stats_encoded;
	*dropped = pw->stats_dropped;
	*encode_ns = pw->stats_encode_ns;
	pw->stats_encoded = pw->stats_dropped = 0;
	pw->stats_encode_ns = 0;
	pthread_mutex_unlock(&pw->lock);
}
