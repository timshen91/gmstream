// AV1 encoding on the GPU's Vulkan video encode queue, straight from
// gamescope's dma-buf, with each piece of the bitstream sent while the GPU is
// still writing it (DESIGN.md §12).
//
// What this does that the VA-API path (encode.c) cannot:
// - No copy and no conversion pass. The encoder takes gamescope's BGRA image
//   as it is (VK_VALVE_video_encode_rgb_conversion: VCN converts on the way
//   in), so the VPP blit into an NV12 surface is gone, and with it the GPU
//   round trip the worker thread used to wait for before the encode started.
// - Nothing on the CPU waits for gamescope's rendering. The fence exported
//   from the dma-buf becomes a Vulkan semaphore the encode submission waits on
//   on the device, so VCN starts the moment the frame is drawn.
// - The bitstream buffer is ours: our own pages, imported into Vulkan
//   (VK_EXT_external_memory_host), so watching the GPU fill it needs no
//   driver tricks (the VA-API path had to wrap libva's calls to get at it).
// - The temporal delimiter and, on a keyframe, the sequence header are written
//   by us and are known before the encode: they go out first, while VCN works.
//
// What the hardware demands (found with tools/vkenc, which records the rest):
// - CBR rate control hangs the encode ring, so the bitrate is held with VBR
//   plus a per-frame size cap (useMaxFrameSize), which is the bound that
//   actually matters: how long one frame takes to reach the client.
// - gamescope composites at ten bits whenever we offer it a ten-bit format,
//   and an eight-bit AV1 profile only accepts eight-bit input. So the session
//   follows the buffer: a ten-bit composite is encoded as ten-bit AV1, with
//   no conversion anywhere. The client reads the depth out of the sequence
//   header, so the stream describes itself.
// - Two DPB slots have to take turns; encoding into the slot being referenced
//   hangs it too.
// - Tile spacing has to be spelled out (no uniform_tile_spacing), and the
//   number of tiles is ours to pick per frame: see tiles_for().
#include <errno.h>
#include <dlfcn.h>
#include <math.h>
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
#include <wlr/util/log.h>

#include "core.h"

#define MAX_IMPORTS 8
// Never send the first bytes of the encoder's output early: VCN comes back to
// patch the frame header once the frame is done.
#define HOLD_PREFIX 128
// How long to sleep between looks at the buffer while the GPU encodes. The
// timeline semaphore wakes us earlier when the frame is finished, so this is
// only the granularity of the pieces, not of the frame.
#define POLL_NS 200000
// What to keep patterned, as a multiple of the largest recent frame.
#define PREFILL_SLACK 2
#define PREFILL_MIN (1u << 18)
#define PREFILL_MAX (4u << 20)
// One datagram's worth of bitstream, by default.
#define DEFAULT_PACKET 1100
// Bytes of bitstream per tile: the frame is split into about this many pieces,
// so a keyframe gets many tiles and a small inter frame gets one or two.
#define DEFAULT_TILE_BYTES 49152

/* ---- Vulkan, loaded at run time ----------------------------------------- */

#define INSTANCE_FUNCS(F) \
	F(EnumeratePhysicalDevices) \
	F(GetPhysicalDeviceProperties2) \
	F(GetPhysicalDeviceQueueFamilyProperties2) \
	F(GetPhysicalDeviceMemoryProperties) \
	F(GetPhysicalDeviceVideoCapabilitiesKHR) \
	F(CreateDevice) \
	F(GetDeviceProcAddr) \
	F(DestroyInstance)

#define DEVICE_FUNCS(F) \
	F(GetDeviceQueue) \
	F(DestroyDevice) \
	F(DeviceWaitIdle) \
	F(CreateImage) \
	F(DestroyImage) \
	F(CreateImageView) \
	F(DestroyImageView) \
	F(GetImageMemoryRequirements) \
	F(BindImageMemory) \
	F(CreateBuffer) \
	F(DestroyBuffer) \
	F(GetBufferMemoryRequirements) \
	F(BindBufferMemory) \
	F(AllocateMemory) \
	F(FreeMemory) \
	F(MapMemory) \
	F(GetMemoryHostPointerPropertiesEXT) \
	F(GetMemoryFdPropertiesKHR) \
	F(CreateVideoSessionKHR) \
	F(DestroyVideoSessionKHR) \
	F(GetVideoSessionMemoryRequirementsKHR) \
	F(BindVideoSessionMemoryKHR) \
	F(CreateVideoSessionParametersKHR) \
	F(DestroyVideoSessionParametersKHR) \
	F(GetEncodedVideoSessionParametersKHR) \
	F(CreateQueryPool) \
	F(DestroyQueryPool) \
	F(GetQueryPoolResults) \
	F(CreateCommandPool) \
	F(DestroyCommandPool) \
	F(AllocateCommandBuffers) \
	F(ResetCommandBuffer) \
	F(BeginCommandBuffer) \
	F(EndCommandBuffer) \
	F(CmdPipelineBarrier2) \
	F(CmdResetQueryPool) \
	F(CmdBeginVideoCodingKHR) \
	F(CmdControlVideoCodingKHR) \
	F(CmdEncodeVideoKHR) \
	F(CmdEndVideoCodingKHR) \
	F(CmdBeginQuery) \
	F(CmdEndQuery) \
	F(QueueSubmit2) \
	F(CreateSemaphore) \
	F(DestroySemaphore) \
	F(ImportSemaphoreFdKHR) \
	F(WaitSemaphores) \
	F(GetSemaphoreCounterValue)

struct vkapi {
#define DECL(n) PFN_vk##n n;
	INSTANCE_FUNCS(DECL)
	DEVICE_FUNCS(DECL)
#undef DECL
};

// One of these per bit depth: the session, its images, the bitstream buffer
// and the query pool all have to name the same video profile.
struct vkprofile {
	VkVideoEncodeProfileRgbConversionInfoVALVE rgb;
	VkVideoEncodeUsageInfoKHR usage;
	VkVideoEncodeAV1ProfileInfoKHR av1;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR list;
};

/* ---- state -------------------------------------------------------------- */

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
	int sync_fd; // gamescope's rendering fence, or -1
	int width, height;
	uint64_t slot_ns;
	uint64_t submit_ns;
};

struct import {
	dev_t dev;
	ino_t ino;
	int width, height;
	uint32_t format;
	uint64_t modifier;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	uint64_t used;
};

struct gm_vkencode {
	const struct gm_callbacks *cb;
	void *lib;
	struct vkapi vk;
	VkInstance instance;
	VkPhysicalDevice phys;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkPhysicalDeviceMemoryProperties mem_props;
	uint32_t host_ptr_align;

	VkVideoEncodeAV1CapabilitiesKHR av1_caps;
	VkVideoEncodeCapabilitiesKHR enc_caps;
	VkVideoCapabilitiesKHR caps;
	struct vkprofile prof; // the profile the session and its images were made with
	VkFormat picture_format; // what gamescope gives us, and what the encoder takes
	uint32_t src_format_drm; // the same, as gamescope's own fourcc
	uint8_t told_cols, told_rows; // the tile layout the client has been told

	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	// Protected by `lock`.
	bool quit, active, keyframe_wanted;
	struct job pending;
	int bitrate_kbps, refresh_mhz, packet_bytes;
	bool chunked;
	bool client_hdr; // the client can show HDR10
	unsigned stats_encoded, stats_dropped, stats_chunks, stats_patched;
	uint64_t stats_encode_ns, stats_first_ns;

	// Worker thread only.
	struct import imports[MAX_IMPORTS];
	uint64_t use_counter;
	uint32_t next_frame_id;

	// The session, for one coded size.
	VkVideoSessionKHR session;
	VkVideoSessionParametersKHR params;
	VkDeviceMemory session_memory[4];
	unsigned session_binds;
	int coded_width, coded_height; // what the encoder is set up for
	int want_width, want_height; // what gamescope gives us
	uint8_t *seq_obu;
	size_t seq_size;

	VkImage dpb;
	VkImageView dpb_view;
	VkDeviceMemory dpb_memory;
	uint32_t ref_slot;
	bool have_ref;

	VkBuffer bitstream;
	VkDeviceMemory bitstream_memory;
	uint8_t *bs_host; // our pages, if the host-pointer import worked
	volatile uint8_t *bs_map;
	uint32_t bs_size;
	uint8_t *shadow;
	unsigned prefill, largest;

	VkQueryPool queries;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd;
	VkSemaphore timeline;
	uint64_t timeline_value;

	// Rate control: ours, since CBR hangs the ring and VBR ignores the
	// bitrate. GM_AV1_Q pins a q index, GM_AV1_RC=vbr hands it back to the
	// hardware; both for measuring.
	unsigned pinned_q, q_inter, q_key, tile_bytes;
	bool hw_rate;
	unsigned rate_gen, rate_applied;
	unsigned q_applied; // the q index the encoder was last programmed with

	bool debug;
	FILE *dump; // GM_AV1_DUMP: 120 frames of bitstream, to look at
	unsigned dumped, dump_from, dump_frames; // GM_AV1_DUMP_FROM: the first
	// frame to take; GM_AV1_DUMP_FRAMES: how many (120)
};

/* ---- the profile ---------------------------------------------------------- */

// Everything (the session, the images, the buffer, the query pool) has to name
// the same video profile, so one of these is built per bit depth and pointed
// at from everywhere. gamescope composites at ten bits whenever we offer it a
// ten-bit format, and the encoder takes that as it is: an eight-bit AV1
// profile would only accept eight-bit input, which would mean converting.

static void profile_init(struct vkprofile *p, bool ten_bit) {
	VkVideoComponentBitDepthFlagsKHR depth = ten_bit
		? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR
		: VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	p->rgb = (VkVideoEncodeProfileRgbConversionInfoVALVE){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_PROFILE_RGB_CONVERSION_INFO_VALVE,
		.performEncodeRgbConversion = VK_TRUE };
	p->usage = (VkVideoEncodeUsageInfoKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR, .pNext = &p->rgb,
		.videoUsageHints = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR,
		.videoContentHints = VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR,
		.tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR };
	p->av1 = (VkVideoEncodeAV1ProfileInfoKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR,
		.pNext = &p->usage, .stdProfile = STD_VIDEO_AV1_PROFILE_MAIN };
	p->profile = (VkVideoProfileInfoKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, .pNext = &p->av1,
		.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
		.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
		.lumaBitDepth = depth, .chromaBitDepth = depth };
	p->list = (VkVideoProfileListInfoKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
		.profileCount = 1, .pProfiles = &p->profile };
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

static bool ten_bit(VkFormat f) {
	return f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ||
		f == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
}

static uint32_t align_up(uint32_t v, uint32_t a) {
	return a > 1 ? (v + a - 1) / a * a : v;
}

static uint32_t memory_type(struct gm_vkencode *vk, uint32_t mask,
		VkMemoryPropertyFlags want) {
	for (uint32_t i = 0; i < vk->mem_props.memoryTypeCount; i++) {
		if ((mask & (1u << i)) &&
				(vk->mem_props.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

/* ---- the frame's bitstream, watched as it is written -------------------- */

// FNV-1a, so a debug run can check that what a client puts back together is
// the frame the encoder produced (probe -H prints the same hash).
static uint64_t hash64(uint64_t h, const uint8_t *data, size_t len) {
	for (size_t i = 0; i < len; i++) {
		h = (h ^ data[i]) * 0x100000001b3ull;
	}
	return h;
}

static uint8_t pattern(unsigned i) {
	return (uint8_t)(0x5a ^ (i * 31u) ^ (i >> 8) ^ (i >> 17));
}

// Where the pattern resumes for good, i.e. how far the GPU has written.
// Thirty-two bytes of agreement is proof enough that nothing was written
// there; by chance it would take 2^256.
static unsigned frontier(struct gm_vkencode *vk, unsigned from, unsigned to) {
	const volatile uint8_t *p = vk->bs_map;
	unsigned run = 0;
	for (unsigned i = from; i < to; i++) {
		if (p[i] == pattern(i)) {
			if (++run == 32) {
				return i - 31;
			}
		} else {
			run = 0;
		}
	}
	return to;
}

static void lay_pattern(struct gm_vkencode *vk) {
	unsigned want = vk->largest * PREFILL_SLACK;
	if (want < PREFILL_MIN) {
		want = PREFILL_MIN;
	}
	if (want > PREFILL_MAX) {
		want = PREFILL_MAX;
	}
	if (want > vk->bs_size) {
		want = vk->bs_size;
	}
	volatile uint8_t *p = vk->bs_map;
	for (unsigned i = 0; i < want; i++) {
		p[i] = pattern(i);
	}
	vk->prefill = want;
}

// One frame being sent out in pieces.
struct stream {
	uint32_t frame_id;
	uint64_t slot_ns;
	uint16_t width, height;
	unsigned prefix; // bytes of ours (temporal delimiter, sequence header)
	unsigned sent; // bytes of the encoder's output streamed so far
	unsigned index;
	uint64_t first_ns;
};

static void emit(struct gm_vkencode *vk, struct stream *s, const uint8_t *data,
		unsigned offset, unsigned len, bool last, unsigned total,
		uint64_t encoded_ns, bool keyframe) {
	if (vk->cb->video_chunk == NULL || len == 0) {
		return;
	}
	unsigned chunk = (unsigned)vk->packet_bytes;
	while (len > 0) {
		unsigned n = len < chunk ? len : chunk;
		bool final = last && n == len;
		struct gm_video_chunk c = {
			.data = data,
			.len = n,
			.frame_id = s->frame_id,
			.offset = offset,
			.index = (uint16_t)s->index++,
			.count = final ? (uint16_t)s->index : 0,
			.total = final ? total : 0,
			.slot_ns = s->slot_ns,
			.encoded_ns = final ? encoded_ns : 0,
			.first_ns = final ? s->first_ns : 0,
			.width = s->width,
			.height = s->height,
			.keyframe = final && keyframe,
			.last = final,
			.codec = GM_CODEC_AV1,
		};
		if (s->first_ns == 0) {
			s->first_ns = gm_now_ns();
		}
		vk->stats_chunks++;
		vk->cb->video_chunk(vk->cb->ctx, &c);
		data += n;
		offset += n;
		len -= n;
	}
}

// Sends whole chunks of everything the GPU has written since last time.
static void push(struct gm_vkencode *vk, struct stream *s) {
	unsigned chunk = (unsigned)vk->packet_bytes;
	unsigned from = s->sent > HOLD_PREFIX ? s->sent : HOLD_PREFIX;
	if (from + chunk > vk->prefill) {
		return;
	}
	unsigned f = frontier(vk, from, vk->prefill);
	while (from + chunk <= f) {
		for (unsigned i = from; i < from + chunk; i++) {
			vk->shadow[i] = vk->bs_map[i];
		}
		emit(vk, s, vk->shadow + from, s->prefix + from, chunk, false, 0, 0, false);
		from += chunk;
	}
	s->sent = from;
}

/* ---- tiles -------------------------------------------------------------- */

#define MAX_TILE_ROWS 16
#define MAX_TILE_COLS 2
struct tiles {
	StdVideoAV1TileInfo info;
	uint16_t col_starts[MAX_TILE_COLS + 1], row_starts[MAX_TILE_ROWS + 1];
	uint16_t col_sizes[MAX_TILE_COLS], row_sizes[MAX_TILE_ROWS];
};

// How many tiles this frame should have, from how big it is expected to be.
// Tiles cost bits (each resets the entropy context and cannot predict across
// its edges), so a small inter frame gets one and a keyframe gets many; the
// pieces of a big frame then settle sooner and VCN has more to work on at
// once. Rows first: tiles are coded in raster order, so splitting by row
// keeps the bitstream in the same order as the picture.
static void tiles_for(struct gm_vkencode *vk, struct tiles *t, int w, int h,
		unsigned expect_bytes) {
	uint32_t sb_cols = align_up((uint32_t)w, 64) / 64;
	uint32_t sb_rows = align_up((uint32_t)h, 64) / 64;
	uint32_t max_rows = vk->av1_caps.maxTiles.height;
	uint32_t max_cols = vk->av1_caps.maxTiles.width;
	if (max_rows > MAX_TILE_ROWS) {
		max_rows = MAX_TILE_ROWS;
	}
	if (max_cols > MAX_TILE_COLS) {
		max_cols = MAX_TILE_COLS;
	}
	// A tile may not be smaller than the hardware's minimum.
	uint32_t min_h = vk->av1_caps.minTileSize.height;
	uint32_t min_w = vk->av1_caps.minTileSize.width;
	if (min_h > 0 && max_rows > (uint32_t)h / min_h) {
		max_rows = (uint32_t)h / min_h;
	}
	if (min_w > 0 && max_cols > (uint32_t)w / min_w) {
		max_cols = (uint32_t)w / min_w;
	}
	if (max_rows < 1) {
		max_rows = 1;
	}
	if (max_cols < 1) {
		max_cols = 1;
	}
	if (max_rows > sb_rows) {
		max_rows = sb_rows;
	}
	if (max_cols > sb_cols) {
		max_cols = sb_cols;
	}

	uint32_t want = expect_bytes / vk->tile_bytes;
	if (want < 1) {
		want = 1;
	}
	uint32_t rows = want < max_rows ? want : max_rows;
	uint32_t cols = rows > 0 && want / rows > 1 ? want / rows : 1;
	if (cols > max_cols) {
		cols = max_cols;
	}

	for (uint32_t i = 0; i < cols; i++) {
		uint32_t start = sb_cols * i / cols, end = sb_cols * (i + 1) / cols;
		t->col_starts[i] = (uint16_t)(start * 16);
		t->col_sizes[i] = (uint16_t)(end - start - 1);
	}
	t->col_starts[cols] = (uint16_t)(sb_cols * 16);
	for (uint32_t i = 0; i < rows; i++) {
		uint32_t start = sb_rows * i / rows, end = sb_rows * (i + 1) / rows;
		t->row_starts[i] = (uint16_t)(start * 16);
		t->row_sizes[i] = (uint16_t)(end - start - 1);
	}
	t->row_starts[rows] = (uint16_t)(sb_rows * 16);
	t->info = (StdVideoAV1TileInfo){
		.TileCols = (uint8_t)cols, .TileRows = (uint8_t)rows,
		.context_update_tile_id = (uint16_t)(cols * rows - 1),
		.tile_size_bytes_minus_1 = 3,
		.pMiColStarts = t->col_starts, .pMiRowStarts = t->row_starts,
		.pWidthInSbsMinus1 = t->col_sizes, .pHeightInSbsMinus1 = t->row_sizes };
}

/* ---- compositor thread -------------------------------------------------- */

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

bool vkencode_submit(struct gm_vkencode *vk, struct wlr_buffer *buffer,
		int width, int height, uint64_t slot_ns) {
	struct wlr_dmabuf_attributes attr;
	if (!wlr_buffer_get_dmabuf(buffer, &attr)) {
		return false; // gamescope's single-pixel fills: nothing to encode
	}
	if (vk_format(attr.format) == VK_FORMAT_UNDEFINED) {
		static uint32_t said;
		if (said != attr.format) {
			said = attr.format;
			wlr_log(WLR_ERROR, "The Vulkan AV1 encoder has no 8-bit format for "
				"gamescope's %.4s buffers", (const char *)&attr.format);
		}
		return false;
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
	// The fence for gamescope's rendering into this buffer: the encode waits
	// for it on the GPU, so nothing here blocks.
	struct dma_buf_export_sync_file exp = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };
	if (ioctl(attr.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) == 0) {
		job.sync_fd = exp.fd;
	}

	pthread_mutex_lock(&vk->lock);
	if (!vk->active) {
		pthread_mutex_unlock(&vk->lock);
		job_clear(&job);
		return true;
	}
	if (vk->pending.valid) {
		job_clear(&vk->pending);
		vk->stats_dropped++;
	}
	vk->pending = job;
	pthread_cond_signal(&vk->cond);
	pthread_mutex_unlock(&vk->lock);
	return true;
}

/* ---- worker thread: importing gamescope's buffer ------------------------ */

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

// Takes ownership of the job's first file descriptor on success.
static struct import *import_dmabuf(struct gm_vkencode *vk, struct dmabuf *b) {
	struct stat st;
	if (fstat(b->fds[0], &st) != 0) {
		return NULL;
	}
	struct import *slot = &vk->imports[0];
	for (int i = 0; i < MAX_IMPORTS; i++) {
		struct import *imp = &vk->imports[i];
		if (imp->image != VK_NULL_HANDLE && imp->dev == st.st_dev &&
				imp->ino == st.st_ino && imp->width == b->width &&
				imp->height == b->height && imp->format == b->format &&
				imp->modifier == b->modifier) {
			imp->used = ++vk->use_counter;
			return imp;
		}
		if (imp->used < slot->used) {
			slot = imp; // least recently used
		}
	}
	if (slot->image != VK_NULL_HANDLE) {
		vk->vk.DestroyImageView(vk->device, slot->view, NULL);
		vk->vk.DestroyImage(vk->device, slot->image, NULL);
		vk->vk.FreeMemory(vk->device, slot->memory, NULL);
		*slot = (struct import){ 0 };
	}

	VkSubresourceLayout layouts[WLR_DMABUF_MAX_PLANES] = { 0 };
	for (int i = 0; i < b->n_planes; i++) {
		layouts[i].offset = b->offsets[i];
		layouts[i].rowPitch = b->strides[i];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.pNext = &vk->prof.list,
		.drmFormatModifier = b->modifier,
		.drmFormatModifierPlaneCount = (uint32_t)b->n_planes,
		.pPlaneLayouts = layouts };
	VkExternalMemoryImageCreateInfo external = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &modifier,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
	VkImageCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
		.imageType = VK_IMAGE_TYPE_2D, .format = vk_format(b->format),
		.extent = { (uint32_t)b->width, (uint32_t)b->height, 1 },
		.mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 1, .pQueueFamilyIndices = &vk->family,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage image;
	VkResult r = vk->vk.CreateImage(vk->device, &ci, NULL, &image);
	if (r != VK_SUCCESS) {
		static bool said;
		if (!said) {
			said = true;
			wlr_log(WLR_ERROR, "The encoder will not take gamescope's %dx%d "
				"buffer (format %.4s, modifier 0x%" PRIx64 "): %d",
				b->width, b->height, (const char *)&b->format, b->modifier, r);
		}
		return NULL;
	}

	VkMemoryRequirements mr;
	vk->vk.GetImageMemoryRequirements(vk->device, image, &mr);
	VkMemoryFdPropertiesKHR fd_props = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
	if (vk->vk.GetMemoryFdPropertiesKHR(vk->device,
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, b->fds[0],
			&fd_props) != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "gamescope's buffer has no importable memory type");
		vk->vk.DestroyImage(vk->device, image, NULL);
		return NULL;
	}
	VkImportMemoryFdInfoKHR import_fd = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.fd = b->fds[0] };
	VkMemoryDedicatedAllocateInfo dedicated = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.pNext = &import_fd, .image = image };
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &dedicated, .allocationSize = mr.size,
		.memoryTypeIndex = memory_type(vk,
			mr.memoryTypeBits & fd_props.memoryTypeBits, 0) };
	VkDeviceMemory memory;
	if (mai.memoryTypeIndex == UINT32_MAX ||
			vk->vk.AllocateMemory(vk->device, &mai, NULL, &memory) != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Importing gamescope's buffer failed (types %#x & %#x)",
			mr.memoryTypeBits, fd_props.memoryTypeBits);
		vk->vk.DestroyImage(vk->device, image, NULL);
		return NULL;
	}
	b->fds[0] = -1; // Vulkan owns it now
	if (vk->vk.BindImageMemory(vk->device, image, memory, 0) != VK_SUCCESS) {
		vk->vk.FreeMemory(vk->device, memory, NULL);
		vk->vk.DestroyImage(vk->device, image, NULL);
		return NULL;
	}
	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ci.format,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1, .layerCount = 1 } };
	VkImageView view;
	if (vk->vk.CreateImageView(vk->device, &vci, NULL, &view) != VK_SUCCESS) {
		vk->vk.FreeMemory(vk->device, memory, NULL);
		vk->vk.DestroyImage(vk->device, image, NULL);
		return NULL;
	}
	wlr_log(WLR_INFO, "Encoding gamescope's %dx%d %.4s buffer, modifier "
		"0x%" PRIx64 ", %d plane%s", b->width, b->height,
		(const char *)&b->format, b->modifier, b->n_planes,
		b->n_planes == 1 ? "" : "s");
	*slot = (struct import){ .dev = st.st_dev, .ino = st.st_ino,
		.width = b->width, .height = b->height, .format = b->format,
		.modifier = b->modifier, .image = image, .view = view,
		.memory = memory, .used = ++vk->use_counter };
	return slot;
}

/* ---- worker thread: the session ----------------------------------------- */

static void session_free(struct gm_vkencode *vk) {
	if (vk->device == VK_NULL_HANDLE) {
		return;
	}
	vk->vk.DeviceWaitIdle(vk->device);
	if (vk->params != VK_NULL_HANDLE) {
		vk->vk.DestroyVideoSessionParametersKHR(vk->device, vk->params, NULL);
		vk->params = VK_NULL_HANDLE;
	}
	if (vk->session != VK_NULL_HANDLE) {
		vk->vk.DestroyVideoSessionKHR(vk->device, vk->session, NULL);
		vk->session = VK_NULL_HANDLE;
	}
	for (unsigned i = 0; i < vk->session_binds; i++) {
		vk->vk.FreeMemory(vk->device, vk->session_memory[i], NULL);
	}
	vk->session_binds = 0;
	if (vk->dpb_view != VK_NULL_HANDLE) {
		vk->vk.DestroyImageView(vk->device, vk->dpb_view, NULL);
		vk->dpb_view = VK_NULL_HANDLE;
	}
	if (vk->dpb != VK_NULL_HANDLE) {
		vk->vk.DestroyImage(vk->device, vk->dpb, NULL);
		vk->dpb = VK_NULL_HANDLE;
		vk->vk.FreeMemory(vk->device, vk->dpb_memory, NULL);
	}
	if (vk->queries != VK_NULL_HANDLE) {
		vk->vk.DestroyQueryPool(vk->device, vk->queries, NULL);
		vk->queries = VK_NULL_HANDLE;
	}
	if (vk->bitstream != VK_NULL_HANDLE) {
		vk->vk.DestroyBuffer(vk->device, vk->bitstream, NULL);
		vk->bitstream = VK_NULL_HANDLE;
		vk->vk.FreeMemory(vk->device, vk->bitstream_memory, NULL);
	}
	free(vk->bs_host);
	vk->bs_host = NULL;
	vk->bs_map = NULL;
	free(vk->shadow);
	vk->shadow = NULL;
	free(vk->seq_obu);
	vk->seq_obu = NULL;
	vk->seq_size = 0;
	vk->coded_width = vk->coded_height = 0;
	vk->have_ref = false;
	vk->q_applied = 0;
}

// The bitstream buffer: our own pages where the driver will take them
// (VK_EXT_external_memory_host), so reading what the GPU has written is an
// ordinary cached read and the pages can later go to the socket as they are.
static bool make_bitstream(struct gm_vkencode *vk, uint32_t size) {
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = &vk->prof.list, .size = size,
		.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 1, .pQueueFamilyIndices = &vk->family };
	if (vk->vk.CreateBuffer(vk->device, &bci, NULL, &vk->bitstream) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements mr;
	vk->vk.GetBufferMemoryRequirements(vk->device, vk->bitstream, &mr);

	const VkMemoryPropertyFlags host =
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if (vk->host_ptr_align > 0) {
		size_t bytes = align_up(size, vk->host_ptr_align);
		void *pages = NULL;
		if (posix_memalign(&pages, vk->host_ptr_align, bytes) == 0) {
			VkMemoryHostPointerPropertiesEXT hp = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
			VkImportMemoryHostPointerInfoEXT imp = {
				.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
				.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
				.pHostPointer = pages };
			uint32_t type = UINT32_MAX;
			if (vk->vk.GetMemoryHostPointerPropertiesEXT(vk->device,
					imp.handleType, pages, &hp) == VK_SUCCESS) {
				type = memory_type(vk, mr.memoryTypeBits & hp.memoryTypeBits, host);
			}
			VkMemoryAllocateInfo mai = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
				.allocationSize = bytes, .memoryTypeIndex = type };
			if (type != UINT32_MAX && vk->vk.AllocateMemory(vk->device, &mai, NULL,
					&vk->bitstream_memory) == VK_SUCCESS) {
				vk->bs_host = pages;
				vk->bs_map = pages;
			} else {
				free(pages);
			}
		}
	}
	if (vk->bs_map == NULL) {
		// The driver would not take our pages; its own host-visible memory
		// works, only the reads are slower.
		uint32_t type = memory_type(vk, mr.memoryTypeBits, host);
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = mr.size, .memoryTypeIndex = type };
		void *map = NULL;
		if (type == UINT32_MAX ||
				vk->vk.AllocateMemory(vk->device, &mai, NULL, &vk->bitstream_memory)
					!= VK_SUCCESS ||
				vk->vk.MapMemory(vk->device, vk->bitstream_memory, 0, VK_WHOLE_SIZE,
					0, &map) != VK_SUCCESS) {
			vk->vk.DestroyBuffer(vk->device, vk->bitstream, NULL);
			vk->bitstream = VK_NULL_HANDLE;
			return false;
		}
		vk->bs_map = map;
	}
	if (vk->vk.BindBufferMemory(vk->device, vk->bitstream, vk->bitstream_memory,
			0) != VK_SUCCESS) {
		return false;
	}
	vk->bs_size = size;
	vk->shadow = malloc(size + 64);
	wlr_log(WLR_INFO, "AV1 bitstream buffer: %u KiB%s", size / 1024,
		vk->bs_host != NULL ? ", our own pages" : "");
	return vk->shadow != NULL;
}

// HDR: gamescope composites HDR10 (PQ, BT.2020) when SteamOS has HDR on
// (Steam's display setting drives its hdr_enabled convar; we never force it).
// The encoder's own conversion only picks a matrix, so this says which one
// and what the sequence header should claim; the picture itself is already PQ
// by the time it reaches us, and still goes in without a copy.
//
// GM_HDR says the composite is PQ. Reading that from gamescope instead of
// being told needs a colour-management protocol between us, which is the
// piece still missing.
static bool hdr_content(void) {
	return getenv("GM_HDR") != NULL;
}

// Whether this session's stream is HDR: the composite has to be PQ and the
// client has to be able to show it.
static bool hdr_session(struct gm_vkencode *vk) {
	return hdr_content() && vk->client_hdr;
}

static bool full_range(void) {
	const char *r = getenv("GM_AV1_RANGE");
	return r != NULL && strcmp(r, "full") == 0;
}

// The sequence header's colour config. The client builds VideoToolbox's av1C
// record out of this, bit depth included, so a ten-bit stream describes itself.
static StdVideoAV1ColorConfig color_config_for(bool ten, bool hdr) {
	return (StdVideoAV1ColorConfig){
		.flags = { .color_range = full_range() ? 1u : 0u,
			.color_description_present_flag = 1 },
		.BitDepth = ten ? 10 : 8, .subsampling_x = 1, .subsampling_y = 1,
		.color_primaries = hdr ? STD_VIDEO_AV1_COLOR_PRIMARIES_BT_2020
			: STD_VIDEO_AV1_COLOR_PRIMARIES_BT_709,
		.transfer_characteristics = hdr
			? STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_SMPTE_2084
			: STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_BT_709,
		.matrix_coefficients = hdr ? STD_VIDEO_AV1_MATRIX_COEFFICIENTS_BT_2020_NCL
			: STD_VIDEO_AV1_MATRIX_COEFFICIENTS_BT_709,
		.chroma_sample_position = STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_UNKNOWN };
}

// What the encoder takes in and puts out, in words, for the client's stats
// line. The tile layout goes with it: it follows the frame's budget, so it is
// worth seeing next to the bitrate.
static void report_format(struct gm_vkencode *vk, unsigned cols, unsigned rows) {
	if (vk->cb->encode_format == NULL) {
		return;
	}
	bool ten = ten_bit(vk->picture_format);
	char text[160];
	snprintf(text, sizeof(text), "%.4s %u-bit %s, no copy → AV1 %u-bit %s, %ux%u tiles",
		(const char *)&vk->src_format_drm, ten ? 10 : 8,
		hdr_session(vk) ? "BT.2020" : "BT.709", ten ? 10 : 8,
		hdr_session(vk) ? "PQ" : "BT.709", cols, rows);
	vk->cb->encode_format(vk->cb->ctx, text);
}

static bool session_open(struct gm_vkencode *vk, int width, int height,
		VkFormat format) {
	session_free(vk);
	bool ten = ten_bit(format);
	profile_init(&vk->prof, ten);
	vk->av1_caps = (VkVideoEncodeAV1CapabilitiesKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR };
	vk->enc_caps = (VkVideoEncodeCapabilitiesKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR,
		.pNext = &vk->av1_caps };
	vk->caps = (VkVideoCapabilitiesKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, .pNext = &vk->enc_caps };
	if (vk->vk.GetPhysicalDeviceVideoCapabilitiesKHR(vk->phys, &vk->prof.profile,
			&vk->caps) != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "No %u-bit AV1 encoding from an RGB image", ten ? 10 : 8);
		return false;
	}
	// The reconstructed pictures are YCbCr at the same depth.
	VkFormat dpb_format = ten
		? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
		: VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	vk->picture_format = format;
	uint32_t w = align_up((uint32_t)width, vk->av1_caps.codedPictureAlignment.width);
	uint32_t h = align_up((uint32_t)height, vk->av1_caps.codedPictureAlignment.height);
	vk->want_width = width;
	vk->want_height = height;

	VkVideoEncodeSessionRgbConversionCreateInfoVALVE rgb = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_RGB_CONVERSION_CREATE_INFO_VALVE,
		.rgbModel = hdr_session(vk)
			? VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_2020_BIT_VALVE
			: VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_709_BIT_VALVE,
		// Narrow (video) range, which is what the client's decoder hands to
		// its shader. A full-range stream decodes just as fast, but then
		// VideoToolbox converts every frame on the way out to the video-range
		// NV12 the client asks for, and that pass is not cheap. GM_AV1_RANGE
		// =full asks for the other one, to measure that.
		.rgbRange = full_range()
			? VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_FULL_RANGE_BIT_VALVE
			: VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_NARROW_RANGE_BIT_VALVE,
		.xChromaOffset = VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_COSITED_EVEN_BIT_VALVE,
		.yChromaOffset = VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_COSITED_EVEN_BIT_VALVE };
	VkVideoSessionCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR, .pNext = &rgb,
		.queueFamilyIndex = vk->family, .pVideoProfile = &vk->prof.profile,
		.pictureFormat = format, .maxCodedExtent = { w, h },
		.referencePictureFormat = dpb_format,
		.maxDpbSlots = 2, .maxActiveReferencePictures = 1,
		.pStdHeaderVersion = &vk->caps.stdHeaderVersion };
	if (vk->vk.CreateVideoSessionKHR(vk->device, &sci, NULL, &vk->session)
			!= VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Creating a %ux%u AV1 encode session failed", w, h);
		return false;
	}

	uint32_t binds = 0;
	vk->vk.GetVideoSessionMemoryRequirementsKHR(vk->device, vk->session, &binds, NULL);
	if (binds > 4) {
		wlr_log(WLR_ERROR, "The encode session wants %u memory binds", binds);
		return false;
	}
	VkVideoSessionMemoryRequirementsKHR reqs[4] = { 0 };
	VkBindVideoSessionMemoryInfoKHR bind[4] = { 0 };
	for (uint32_t i = 0; i < binds; i++) {
		reqs[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	}
	vk->vk.GetVideoSessionMemoryRequirementsKHR(vk->device, vk->session, &binds, reqs);
	for (uint32_t i = 0; i < binds; i++) {
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = reqs[i].memoryRequirements.size,
			.memoryTypeIndex = memory_type(vk, reqs[i].memoryRequirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
		if (mai.memoryTypeIndex == UINT32_MAX || vk->vk.AllocateMemory(vk->device,
				&mai, NULL, &vk->session_memory[i]) != VK_SUCCESS) {
			wlr_log(WLR_ERROR, "Encode session memory failed");
			return false;
		}
		vk->session_binds = i + 1;
		bind[i] = (VkBindVideoSessionMemoryInfoKHR){
			.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
			.memoryBindIndex = reqs[i].memoryBindIndex,
			.memory = vk->session_memory[i],
			.memorySize = reqs[i].memoryRequirements.size };
	}
	if (vk->vk.BindVideoSessionMemoryKHR(vk->device, vk->session, binds, bind)
			!= VK_SUCCESS) {
		return false;
	}

	StdVideoAV1ColorConfig color_config = color_config_for(ten, hdr_session(vk));
	// Which coding tools the sequence header claims. The intra edge filter
	// must stay off: this encoder does not apply it to its own reconstruction,
	// so claiming it made every intra block inside an inter frame decode a
	// little differently from the reference the encoder predicted from, and
	// the reference chain carried that error forward. It showed as colour
	// casts and confetti growing over a minute or two, fastest at a fine q
	// index, where the most blocks are coded. CDEF is off for the same reason
	// -- we never set strengths for it, so there is nothing to claim. This is
	// what FFmpeg's Vulkan AV1 encoder does, which does not drift here.
	StdVideoAV1SequenceHeader seq = {
		.flags = { .enable_order_hint = 1 },
		.seq_profile = STD_VIDEO_AV1_PROFILE_MAIN,
		.frame_width_bits_minus_1 = 15, .frame_height_bits_minus_1 = 15,
		.max_frame_width_minus_1 = (uint16_t)(w - 1),
		.max_frame_height_minus_1 = (uint16_t)(h - 1),
		// Eight bits, because order_hint below is a whole byte of frame id.
		.order_hint_bits_minus_1 = 7, .pColorConfig = &color_config };
	VkVideoEncodeAV1SessionParametersCreateInfoKHR av1_params = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR,
		.pStdSequenceHeader = &seq };
	VkVideoEncodeQualityLevelInfoKHR quality = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,
		.pNext = &av1_params, .qualityLevel = 0 };
	VkVideoSessionParametersCreateInfoKHR pci = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
		.pNext = &quality, .videoSession = vk->session };
	if (vk->vk.CreateVideoSessionParametersKHR(vk->device, &pci, NULL, &vk->params)
			!= VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Encode session parameters failed");
		return false;
	}

	// The sequence header OBU: the encode operation does not emit it, so it is
	// ours to put in front of every keyframe.
	VkVideoEncodeSessionParametersGetInfoKHR gi = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
		.videoSessionParameters = vk->params };
	VkVideoEncodeSessionParametersFeedbackInfoKHR fb = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR };
	size_t seq_size = 0;
	if (vk->vk.GetEncodedVideoSessionParametersKHR(vk->device, &gi, &fb,
			&seq_size, NULL) != VK_SUCCESS || seq_size == 0) {
		wlr_log(WLR_ERROR, "No sequence header from the encoder");
		return false;
	}
	vk->seq_obu = malloc(seq_size);
	if (vk->seq_obu == NULL || vk->vk.GetEncodedVideoSessionParametersKHR(vk->device,
			&gi, &fb, &seq_size, vk->seq_obu) != VK_SUCCESS) {
		return false;
	}
	vk->seq_size = seq_size;

	// The DPB: two slots in one image, since this hardware cannot keep
	// reference pictures in separate images.
	VkImageCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &vk->prof.list, .imageType = VK_IMAGE_TYPE_2D,
		.format = dpb_format, .extent = { w, h, 1 },
		.mipLevels = 1, .arrayLayers = 2, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 1, .pQueueFamilyIndices = &vk->family,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	if (vk->vk.CreateImage(vk->device, &dci, NULL, &vk->dpb) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements dmr;
	vk->vk.GetImageMemoryRequirements(vk->device, vk->dpb, &dmr);
	VkMemoryAllocateInfo dmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = dmr.size, .memoryTypeIndex = memory_type(vk,
			dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	if (dmai.memoryTypeIndex == UINT32_MAX ||
			vk->vk.AllocateMemory(vk->device, &dmai, NULL, &vk->dpb_memory)
				!= VK_SUCCESS ||
			vk->vk.BindImageMemory(vk->device, vk->dpb, vk->dpb_memory, 0)
				!= VK_SUCCESS) {
		return false;
	}
	VkImageViewCreateInfo dvci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = vk->dpb, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = dci.format,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1, .layerCount = 2 } };
	if (vk->vk.CreateImageView(vk->device, &dvci, NULL, &vk->dpb_view) != VK_SUCCESS) {
		return false;
	}

	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR qfi = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
		.pNext = &vk->prof.profile,
		.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
			VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR };
	VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.pNext = &qfi, .queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR,
		.queryCount = 1 };
	if (vk->vk.CreateQueryPool(vk->device, &qpi, NULL, &vk->queries) != VK_SUCCESS) {
		return false;
	}

	// Room for a keyframe of this size, with slack.
	uint32_t bs = align_up(w * h / 2, 1u << 20);
	if (bs < (4u << 20)) {
		bs = 4u << 20;
	}
	if (!make_bitstream(vk, bs)) {
		return false;
	}
	vk->coded_width = (int)w;
	vk->coded_height = (int)h;
	vk->largest = 0;
	lay_pattern(vk);
	wlr_log(WLR_INFO, "Encoding %u-bit %s AV1 %dx%d (coded %ux%u) on the Vulkan "
		"video encode queue, straight from gamescope's buffer", ten ? 10 : 8,
		hdr_session(vk) ? "HDR10 (PQ, BT.2020)" : "SDR (BT.709)", width, height, w, h);
	vk->told_cols = vk->told_rows = 0; // the next inter frame tells the client
	return true;
}

/* ---- worker thread: one frame ------------------------------------------- */

// The q index for the next frame of this kind, from how big the last one came
// out. Measured on this encoder: about 58 q indices doubles or halves the
// bytes, so a whole doubling of error is worth about that much; a third of it
// per frame converges in two or three frames without hunting.
static unsigned next_q(struct gm_vkencode *vk, unsigned q, unsigned bytes,
		unsigned target) {
	if (bytes == 0 || target == 0) {
		return q;
	}
	double ratio = (double)bytes / (double)target;
	double doublings = log2(ratio);
	int step = (int)(20.0 * doublings);
	if (step > 40) {
		step = 40;
	}
	if (step < -40) {
		step = -40;
	}
	int next = (int)q + step;
	unsigned lo = vk->av1_caps.minQIndex > 1 ? vk->av1_caps.minQIndex : 1;
	unsigned hi = vk->av1_caps.maxQIndex > 0 ? vk->av1_caps.maxQIndex : 255;
	if (next < (int)lo) {
		next = (int)lo;
	}
	if (next > (int)hi) {
		next = (int)hi;
	}
	return (unsigned)next;
}

static void encode_one(struct gm_vkencode *vk, struct job *job, bool keyframe,
		int bitrate_kbps, int refresh_mhz) {
	// The session has to exist before the buffer can be imported: the image
	// names the video profile it will be encoded with.
	VkFormat format = vk_format(job->buf.format);
	vk->src_format_drm = job->buf.format;
	if (vk->coded_width == 0 || vk->want_width != job->width ||
			vk->want_height != job->height || vk->picture_format != format) {
		if (!session_open(vk, job->width, job->height, format)) {
			return;
		}
		keyframe = true;
	}
	struct import *src = import_dmabuf(vk, &job->buf);
	if (src == NULL) {
		return;
	}
	if (!vk->have_ref) {
		keyframe = true;
	}

	// What this frame is allowed to cost, and the q index to get there.
	unsigned fps_milli = refresh_mhz > 0 ? (unsigned)refresh_mhz : 60000;
	unsigned target = (unsigned)((uint64_t)bitrate_kbps * 1000 / 8 * 1000 / fps_milli);
	unsigned q = vk->pinned_q > 0 ? vk->pinned_q
		: (keyframe ? vk->q_key : vk->q_inter);
	// How many tiles: from what this frame is allowed to cost, never from what
	// the last one did. Tiles make a frame bigger, so sizing them by the last
	// frame's size feeds back on itself and runs away to the maximum.
	struct tiles tiles;
	tiles_for(vk, &tiles, vk->coded_width, vk->coded_height,
		keyframe ? target * 4 : target);
	// A keyframe gets its own, larger, layout; the steady state is what the
	// stats line is about.
	if (!keyframe && (tiles.info.TileCols != vk->told_cols ||
			tiles.info.TileRows != vk->told_rows)) {
		vk->told_cols = tiles.info.TileCols;
		vk->told_rows = tiles.info.TileRows;
		report_format(vk, tiles.info.TileCols, tiles.info.TileRows);
	}

	uint32_t slot = keyframe ? 0 : 1 - vk->ref_slot;
	uint32_t frame_id = vk->next_frame_id++;

	bool chunked = vk->cb->video_chunk != NULL && vk->chunked;
	// Our own bytes first: they are known before the encode, so they are on
	// the wire while the GPU is still working.
	uint8_t prefix[64];
	unsigned prefix_len = 0;
	prefix[prefix_len++] = 0x12; // temporal delimiter, has_size_field
	prefix[prefix_len++] = 0x00;
	if (keyframe && vk->seq_size + prefix_len <= sizeof(prefix)) {
		memcpy(prefix + prefix_len, vk->seq_obu, vk->seq_size);
		prefix_len += (unsigned)vk->seq_size;
	}
	struct stream s = { .frame_id = frame_id, .slot_ns = job->slot_ns,
		.width = (uint16_t)job->width, .height = (uint16_t)job->height,
		.prefix = prefix_len };

	// Import gamescope's fence as a semaphore the GPU waits on.
	VkSemaphore wait = VK_NULL_HANDLE;
	if (job->sync_fd >= 0) {
		VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if (vk->vk.CreateSemaphore(vk->device, &sci, NULL, &wait) == VK_SUCCESS) {
			VkImportSemaphoreFdInfoKHR imp = {
				.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
				.semaphore = wait, .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
				.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
				.fd = job->sync_fd };
			if (vk->vk.ImportSemaphoreFdKHR(vk->device, &imp) == VK_SUCCESS) {
				job->sync_fd = -1; // the semaphore owns it
			} else {
				vk->vk.DestroySemaphore(vk->device, wait, NULL);
				wait = VK_NULL_HANDLE;
			}
		}
	}

	vk->vk.ResetCommandBuffer(vk->cmd, 0);
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	vk->vk.BeginCommandBuffer(vk->cmd, &bi);

	// Take gamescope's image from whatever queue drew it, and the DPB on its
	// first use.
	VkImageMemoryBarrier2 in[2];
	uint32_t nin = 0;
	in[nin++] = (VkImageMemoryBarrier2){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
		.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
		.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.dstQueueFamilyIndex = vk->family,
		.image = src->image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1, .layerCount = 1 } };
	if (!vk->have_ref) {
		in[nin++] = (VkImageMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
			.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
			.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
			.image = vk->dpb,
			.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1, .layerCount = 2 } };
	}
	VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = nin, .pImageMemoryBarriers = in };
	vk->vk.CmdPipelineBarrier2(vk->cmd, &dep);
	vk->vk.CmdResetQueryPool(vk->cmd, vk->queries, 0, 1);

	VkVideoPictureResourceInfoKHR setup_res = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
		.codedExtent = { (uint32_t)vk->coded_width, (uint32_t)vk->coded_height },
		.baseArrayLayer = slot, .imageViewBinding = vk->dpb_view };
	VkVideoPictureResourceInfoKHR ref_res = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
		.codedExtent = { (uint32_t)vk->coded_width, (uint32_t)vk->coded_height },
		.baseArrayLayer = vk->ref_slot, .imageViewBinding = vk->dpb_view };
	StdVideoEncodeAV1ReferenceInfo setup_std = { .RefFrameId = slot,
		.frame_type = keyframe ? STD_VIDEO_AV1_FRAME_TYPE_KEY
			: STD_VIDEO_AV1_FRAME_TYPE_INTER,
		.OrderHint = (uint8_t)frame_id };
	VkVideoEncodeAV1DpbSlotInfoKHR setup_av1 = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR,
		.pStdReferenceInfo = &setup_std };
	VkVideoReferenceSlotInfoKHR setup = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
		.pNext = &setup_av1, .slotIndex = (int32_t)slot,
		.pPictureResource = &setup_res };
	StdVideoEncodeAV1ReferenceInfo ref_std = { .RefFrameId = vk->ref_slot,
		.frame_type = STD_VIDEO_AV1_FRAME_TYPE_INTER,
		.OrderHint = (uint8_t)(frame_id - 1) };
	VkVideoEncodeAV1DpbSlotInfoKHR ref_av1 = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR,
		.pStdReferenceInfo = &ref_std };
	VkVideoReferenceSlotInfoKHR ref = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
		.pNext = &ref_av1, .slotIndex = (int32_t)vk->ref_slot,
		.pPictureResource = &ref_res };
	// Begin-coding declares every slot the frame touches: the reference by
	// index, and the frame's own output slot as not yet in use.
	VkVideoReferenceSlotInfoKHR slots[2];
	uint32_t nslots = 0;
	if (!keyframe) {
		slots[nslots++] = ref;
	}
	slots[nslots] = setup;
	slots[nslots].slotIndex = -1;
	slots[nslots].pNext = NULL;
	nslots++;
	VkVideoBeginCodingInfoKHR begin = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
		.videoSession = vk->session, .videoSessionParameters = vk->params,
		.referenceSlotCount = nslots, .pReferenceSlots = slots };
	vk->vk.CmdBeginVideoCodingKHR(vk->cmd, &begin);

	// Rate control. The hardware's own is no use here: CBR hangs the encode
	// ring, and VBR ignores the bitrate on this driver (asking for 25 Mb/s
	// and 100 Mb/s both gave 59). So rate control is off and the q index is
	// chosen per frame (next_q), which bounds the bytes per frame rather than
	// the bits per second: it is one frame's transmit time that shows up as
	// latency. GM_AV1_RC=vbr asks the hardware instead, for comparison.
	//
	// Issued again whenever the q index changes, because on this driver
	// constantQIndex only reaches the encoder on the frame that follows this
	// command: without it the quantizer stays latched at whatever the session
	// started with and every later q is ignored. That was worth a factor of
	// sixty (measured at 1080p, same picture, q index sweeping 1..255: frames
	// stuck at 74 kB when the session began at q 1, at 1.2 kB when it began
	// at q 255) and it made the stream ignore the client's bitrate, since the
	// controller's q had no effect at all. The runs that looked right were
	// the ones whose latched q happened to suit the content.
	if (!vk->have_ref || vk->rate_gen != vk->rate_applied || q != vk->q_applied) {
		VkVideoEncodeAV1RateControlLayerInfoKHR av1_layer = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_LAYER_INFO_KHR,
			.useMaxFrameSize = VK_TRUE,
			.maxFrameSize = { .intraFrameSize = target * 4,
				.predictiveFrameSize = target * 2,
				.bipredictiveFrameSize = target } };
		VkVideoEncodeRateControlLayerInfoKHR layer = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR,
			.pNext = &av1_layer,
			.averageBitrate = (uint64_t)bitrate_kbps * 1000,
			.maxBitrate = (uint64_t)bitrate_kbps * 1000,
			.frameRateNumerator = fps_milli, .frameRateDenominator = 1000 };
		VkVideoEncodeAV1RateControlInfoKHR av1_rc = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR,
			.gopFrameCount = 60, .keyFramePeriod = 0, .temporalLayerCount = 1 };
		VkVideoEncodeRateControlInfoKHR rc = !vk->hw_rate
			? (VkVideoEncodeRateControlInfoKHR){
				.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
				.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR }
			: (VkVideoEncodeRateControlInfoKHR){
				.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
				.pNext = &av1_rc,
				.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR,
				.layerCount = 1, .pLayers = &layer,
				.virtualBufferSizeInMs = 100, .initialVirtualBufferSizeInMs = 50 };
		VkVideoEncodeQualityLevelInfoKHR ql = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,
			.pNext = &rc, .qualityLevel = 0 };
		VkVideoCodingControlInfoKHR control = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR, .pNext = &ql,
			.flags = (vk->have_ref ? 0 : VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR) |
				VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR |
				VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR };
		vk->vk.CmdControlVideoCodingKHR(vk->cmd, &control);
		vk->rate_applied = vk->rate_gen;
		vk->q_applied = q;
	}

	StdVideoAV1Quantization quant = { .base_q_idx = (uint8_t)q };
	StdVideoAV1LoopFilter lf = { 0 };
	StdVideoAV1CDEF cdef = { 0 };
	StdVideoAV1LoopRestoration lr = { .FrameRestorationType = {
		STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE,
		STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE,
		STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE } };
	StdVideoAV1GlobalMotion gm = { 0 };
	StdVideoEncodeAV1PictureInfo std_pic = {
		.flags = { .render_and_frame_size_different =
				vk->coded_width != job->width || vk->coded_height != job->height,
			.show_frame = 1, .showable_frame = !keyframe },
		.frame_type = keyframe ? STD_VIDEO_AV1_FRAME_TYPE_KEY
			: STD_VIDEO_AV1_FRAME_TYPE_INTER,
		.current_frame_id = slot,
		.order_hint = (uint8_t)frame_id,
		.primary_ref_frame = keyframe ? STD_VIDEO_AV1_PRIMARY_REF_NONE : 0,
		.refresh_frame_flags = keyframe ? 0xff : (uint8_t)(1u << slot),
		.render_width_minus_1 = (uint16_t)(job->width - 1),
		.render_height_minus_1 = (uint16_t)(job->height - 1),
		.interpolation_filter = STD_VIDEO_AV1_INTERPOLATION_FILTER_EIGHTTAP,
		.TxMode = STD_VIDEO_AV1_TX_MODE_SELECT,
		.pTileInfo = &tiles.info, .pQuantization = &quant, .pLoopFilter = &lf,
		.pCDEF = &cdef, .pLoopRestoration = &lr, .pGlobalMotion = &gm };
	for (unsigned i = 0; i < STD_VIDEO_AV1_REFS_PER_FRAME; i++) {
		std_pic.ref_frame_idx[i] = keyframe ? -1 : (int8_t)vk->ref_slot;
	}
	if (!keyframe) {
		std_pic.ref_order_hint[vk->ref_slot] = (uint8_t)(frame_id - 1);
	}
	VkVideoEncodeAV1PictureInfoKHR av1_pic = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_KHR,
		.predictionMode = keyframe ? VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR
			: VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR,
		.rateControlGroup = keyframe ? VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_INTRA_KHR
			: VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_PREDICTIVE_KHR,
		.constantQIndex = q, .pStdPictureInfo = &std_pic };
	for (unsigned i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; i++) {
		av1_pic.referenceNameSlotIndices[i] =
			!keyframe && i == 0 ? (int32_t)vk->ref_slot : -1;
	}
	VkVideoPictureResourceInfoKHR src_res = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
		.codedExtent = { (uint32_t)vk->coded_width, (uint32_t)vk->coded_height },
		.imageViewBinding = src->view };
	VkVideoEncodeInfoKHR info = { .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR,
		.pNext = &av1_pic, .dstBuffer = vk->bitstream, .dstBufferOffset = 0,
		.dstBufferRange = vk->bs_size, .srcPictureResource = src_res,
		.pSetupReferenceSlot = &setup,
		.referenceSlotCount = keyframe ? 0 : 1,
		.pReferenceSlots = keyframe ? NULL : &ref };
	vk->vk.CmdBeginQuery(vk->cmd, vk->queries, 0, 0);
	vk->vk.CmdEncodeVideoKHR(vk->cmd, &info);
	vk->vk.CmdEndQuery(vk->cmd, vk->queries, 0);
	VkVideoEndCodingInfoKHR end = { .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
	vk->vk.CmdEndVideoCodingKHR(vk->cmd, &end);

	// Give gamescope's image back.
	VkImageMemoryBarrier2 out = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
		.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
		.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
		.oldLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = vk->family,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.image = src->image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1, .layerCount = 1 } };
	VkDependencyInfo dep2 = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &out };
	vk->vk.CmdPipelineBarrier2(vk->cmd, &dep2);
	vk->vk.EndCommandBuffer(vk->cmd);

	uint64_t signal = ++vk->timeline_value;
	VkSemaphoreSubmitInfo waits[1] = { {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
		.stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR } };
	VkSemaphoreSubmitInfo signals[1] = { {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = vk->timeline,
		.value = signal, .stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR } };
	VkCommandBufferSubmitInfo cbs = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = vk->cmd };
	VkSubmitInfo2 si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.waitSemaphoreInfoCount = wait != VK_NULL_HANDLE ? 1 : 0,
		.pWaitSemaphoreInfos = waits,
		.commandBufferInfoCount = 1, .pCommandBufferInfos = &cbs,
		.signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = signals };
	uint64_t submitted = gm_now_ns();
	VkResult sr = vk->vk.QueueSubmit2(vk->queue, 1, &si, VK_NULL_HANDLE);
	if (sr != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Submitting the encode failed: %d", sr);
		if (wait != VK_NULL_HANDLE) {
			vk->vk.DestroySemaphore(vk->device, wait, NULL);
		}
		return;
	}

	// The frame's first bytes, before the encoder has written any of its own.
	if (chunked) {
		emit(vk, &s, prefix, 0, prefix_len, false, 0, 0, false);
	}

	// Send what the GPU has written, until it says it is done. The timeline
	// semaphore wakes us the moment it is, so the last piece waits on nothing.
	for (;;) {
		if (chunked) {
			push(vk, &s);
		}
		uint64_t value = 0;
		if (vk->vk.GetSemaphoreCounterValue(vk->device, vk->timeline, &value)
				== VK_SUCCESS && value >= signal) {
			break;
		}
		VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1, .pSemaphores = &vk->timeline, .pValues = &signal };
		vk->vk.WaitSemaphores(vk->device, &wi, chunked ? POLL_NS : UINT64_MAX);
	}
	if (chunked) {
		push(vk, &s);
	}
	uint64_t done = gm_now_ns();
	if (wait != VK_NULL_HANDLE) {
		vk->vk.DestroySemaphore(vk->device, wait, NULL);
	}

	uint32_t result[2] = { 0, 0 };
	if (vk->vk.GetQueryPoolResults(vk->device, vk->queries, 0, 1, sizeof(result),
			result, sizeof(result), VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "No encode feedback for frame %u", frame_id);
		return;
	}
	uint32_t offset = result[0], size = result[1];
	if (size == 0 || offset != 0 || size > vk->bs_size) {
		wlr_log(WLR_ERROR, "Frame %u: %u bytes at %u", frame_id, size, offset);
		return;
	}

	unsigned total = prefix_len + size;
	uint64_t sent_last = done; // when the frame's last piece went out
	if (!chunked) {
		// This client takes whole frames: our bytes and the encoder's, in one.
		memcpy(vk->shadow, prefix, prefix_len);
		for (unsigned j = 0; j < size; j++) {
			vk->shadow[prefix_len + j] = vk->bs_map[j];
		}
		struct gm_video_packet out = { .data = vk->shadow, .len = total,
			.frame_id = frame_id, .slot_ns = job->slot_ns, .encoded_ns = done,
			.width = (uint16_t)job->width, .height = (uint16_t)job->height,
			.keyframe = keyframe, .codec = GM_CODEC_AV1 };
		vk->cb->video(vk->cb->ctx, &out);
	}

	// The encoder's own bytes, whatever we sent early: everything the GPU went
	// back and changed goes out again, then what it wrote after our last whole
	// chunk, then the held-back start of the frame as the frame's last piece.
	unsigned sent = s.sent < size ? s.sent : size;
	unsigned patched = 0;
	unsigned i = HOLD_PREFIX;
	while (chunked && i < sent) {
		if (vk->shadow[i] == vk->bs_map[i]) {
			i++;
			continue;
		}
		unsigned start = i;
		while (i < sent && vk->shadow[i] != vk->bs_map[i]) {
			i++;
		}
		for (unsigned j = start; j < i; j++) {
			vk->shadow[j] = vk->bs_map[j];
		}
		emit(vk, &s, vk->shadow + start, prefix_len + start, i - start,
			false, 0, 0, false);
		patched += i - start;
	}
	if (chunked) {
		if (sent < size) {
			for (unsigned j = sent; j < size; j++) {
				vk->shadow[j] = vk->bs_map[j];
			}
			emit(vk, &s, vk->shadow + sent, prefix_len + sent, size - sent,
				false, 0, 0, false);
		}
		unsigned hold = HOLD_PREFIX < size ? HOLD_PREFIX : size;
		for (unsigned j = 0; j < hold; j++) {
			vk->shadow[j] = vk->bs_map[j];
		}
			emit(vk, &s, vk->shadow, prefix_len, hold, true, total, done, keyframe);
		sent_last = gm_now_ns();
	}

	if (vk->dump != NULL && vk->dumped < vk->dump_frames && frame_id >= vk->dump_from) {
		fwrite(prefix, 1, prefix_len, vk->dump);
		for (unsigned j = 0; j < size; j++) {
			fputc(vk->bs_map[j], vk->dump);
		}
		fflush(vk->dump);
		vk->dumped++;
	}

	// Ready the buffer for the next frame, and aim the next q index.
	vk->largest = size > vk->largest ? size : vk->largest - vk->largest / 64;
	lay_pattern(vk);
	vk->ref_slot = slot;
	vk->have_ref = true;
	// A keyframe is allowed four frames' worth of bytes; an inter frame one.
	if (vk->pinned_q == 0 && !vk->hw_rate) {
		if (keyframe) {
			vk->q_key = next_q(vk, q, size, target * 4);
		} else {
			vk->q_inter = next_q(vk, q, size, target);
			// A keyframe follows the picture's difficulty too.
			vk->q_key = vk->q_inter;
		}
	}

	pthread_mutex_lock(&vk->lock);
	vk->stats_encoded++;
	vk->stats_encode_ns += done - submitted;
	vk->stats_patched += patched;
	vk->stats_first_ns += s.first_ns > submitted ? s.first_ns - submitted : 0;
	pthread_mutex_unlock(&vk->lock);
	if (vk->debug) {
		uint64_t h = hash64(0xcbf29ce484222325ull, prefix, prefix_len);
		h = hash64(h, chunked ? vk->shadow : vk->shadow + prefix_len, size);
		fprintf(stderr, "vkencode frame %u %s %u bytes (target %u) in %u pieces, q %u, "
			"%ux%u tiles, first %.2f ms, done %.2f ms, last piece out %.2f ms "
			"(%.2f after the bitstream), %u patched, hash %016llx\n",
			frame_id, keyframe ? "key" : "inter", total,
			keyframe ? target * 4 : target, s.index, q,
			tiles.info.TileCols, tiles.info.TileRows,
			(s.first_ns - submitted) / 1e6, (done - submitted) / 1e6,
			(sent_last - submitted) / 1e6, (sent_last - done) / 1e6, patched,
			(unsigned long long)h);
	}
	if (encode_trace() != NULL) {
		fprintf(encode_trace(), "v %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
			" %" PRIu64 "\n", job->slot_ns, job->submit_ns, submitted,
			s.first_ns, done);
	}
}

static void *vkencode_thread(void *data) {
	struct gm_vkencode *vk = data;
	pthread_mutex_lock(&vk->lock);
	while (!vk->quit) {
		if (!vk->pending.valid) {
			pthread_cond_wait(&vk->cond, &vk->lock);
			continue;
		}
		struct job job = vk->pending;
		vk->pending.valid = false;
		bool keyframe = vk->keyframe_wanted;
		vk->keyframe_wanted = false;
		int bitrate = vk->bitrate_kbps, refresh = vk->refresh_mhz;
		vk->packet_bytes = vk->packet_bytes > 0 ? vk->packet_bytes : DEFAULT_PACKET;
		pthread_mutex_unlock(&vk->lock);

		encode_one(vk, &job, keyframe, bitrate, refresh);
		job_clear(&job);

		pthread_mutex_lock(&vk->lock);
	}
	pthread_mutex_unlock(&vk->lock);
	return NULL;
}

/* ---- setup -------------------------------------------------------------- */

static bool load_api(struct gm_vkencode *vk, PFN_vkGetInstanceProcAddr getproc) {
#define LOAD_I(n) \
	vk->vk.n = (PFN_vk##n)getproc(vk->instance, "vk" #n); \
	if (vk->vk.n == NULL) { \
		wlr_log(WLR_ERROR, "Vulkan has no vk%s", #n); \
		return false; \
	}
	INSTANCE_FUNCS(LOAD_I)
#undef LOAD_I
	return true;
}

static bool load_device_api(struct gm_vkencode *vk) {
#define LOAD_D(n) \
	vk->vk.n = (PFN_vk##n)vk->vk.GetDeviceProcAddr(vk->device, "vk" #n); \
	if (vk->vk.n == NULL) { \
		wlr_log(WLR_ERROR, "This device has no vk%s", #n); \
		return false; \
	}
	DEVICE_FUNCS(LOAD_D)
#undef LOAD_D
	return true;
}

struct gm_vkencode *vkencode_create(const struct gm_callbacks *cb) {
	struct gm_vkencode *vk = calloc(1, sizeof(*vk));
	if (vk == NULL) {
		return NULL;
	}
	vk->cb = cb;
	vk->packet_bytes = DEFAULT_PACKET;
	vk->tile_bytes = DEFAULT_TILE_BYTES;
	vk->q_inter = vk->q_key = 128;
	const char *pq = getenv("GM_AV1_Q");
	if (pq != NULL && atoi(pq) > 0) {
		vk->pinned_q = (unsigned)atoi(pq);
	}
	const char *rc = getenv("GM_AV1_RC");
	vk->hw_rate = rc != NULL && strcmp(rc, "vbr") == 0;
	const char *tb = getenv("GM_AV1_TILE_BYTES");
	if (tb != NULL && atoi(tb) > 0) {
		vk->tile_bytes = (unsigned)atoi(tb);
	}
	vk->debug = getenv("GM_DEBUG_CHUNKS") != NULL;
	const char *nf = getenv("GM_AV1_DUMP_FRAMES");
	vk->dump_frames = nf != NULL ? (unsigned)atoi(nf) : 120;
	const char *from = getenv("GM_AV1_DUMP_FROM");
	vk->dump_from = from != NULL ? (unsigned)atoi(from) : 0;
	const char *dump = getenv("GM_AV1_DUMP");
	if (dump != NULL) {
		vk->dump = fopen(dump, "wb");
	}

	vk->lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (vk->lib == NULL) {
		wlr_log(WLR_INFO, "No libvulkan: no Vulkan AV1 encoding");
		goto err;
	}
	PFN_vkGetInstanceProcAddr getproc =
		(PFN_vkGetInstanceProcAddr)dlsym(vk->lib, "vkGetInstanceProcAddr");
	PFN_vkCreateInstance create_instance = getproc != NULL ?
		(PFN_vkCreateInstance)getproc(NULL, "vkCreateInstance") : NULL;
	if (create_instance == NULL) {
		goto err;
	}
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "gmstream", .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	if (create_instance(&ici, NULL, &vk->instance) != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Creating a Vulkan instance failed");
		goto err;
	}
	if (!load_api(vk, getproc)) {
		goto err;
	}

	// The device with a queue family that encodes AV1.
	uint32_t n = 0;
	vk->vk.EnumeratePhysicalDevices(vk->instance, &n, NULL);
	VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
	if (devs == NULL) {
		goto err;
	}
	vk->vk.EnumeratePhysicalDevices(vk->instance, &n, devs);
	bool found = false;
	for (uint32_t i = 0; i < n && !found; i++) {
		uint32_t qn = 0;
		vk->vk.GetPhysicalDeviceQueueFamilyProperties2(devs[i], &qn, NULL);
		VkQueueFamilyProperties2 *qf = calloc(qn, sizeof(*qf));
		VkQueueFamilyVideoPropertiesKHR *qv = calloc(qn, sizeof(*qv));
		if (qf == NULL || qv == NULL) {
			free(qf);
			free(qv);
			break;
		}
		for (uint32_t q = 0; q < qn; q++) {
			qv[q].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
			qf[q].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
			qf[q].pNext = &qv[q];
		}
		vk->vk.GetPhysicalDeviceQueueFamilyProperties2(devs[i], &qn, qf);
		for (uint32_t q = 0; q < qn; q++) {
			if ((qf[q].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) &&
					(qv[q].videoCodecOperations &
						VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR)) {
				vk->phys = devs[i];
				vk->family = q;
				found = true;
				break;
			}
		}
		free(qf);
		free(qv);
	}
	free(devs);
	if (!found) {
		wlr_log(WLR_INFO, "No Vulkan AV1 encode queue on this GPU");
		goto err;
	}

	// Both depths, since gamescope picks the format: eight-bit must work, and
	// ten-bit is what it composites at whenever it can.
	profile_init(&vk->prof, false);
	VkVideoEncodeAV1CapabilitiesKHR a = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR };
	VkVideoEncodeCapabilitiesKHR e = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR, .pNext = &a };
	VkVideoCapabilitiesKHR c = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, .pNext = &e };
	if (vk->vk.GetPhysicalDeviceVideoCapabilitiesKHR(vk->phys, &vk->prof.profile, &c)
			!= VK_SUCCESS) {
		wlr_log(WLR_INFO, "This GPU will not encode AV1 from an RGB image");
		goto err;
	}
	vk->vk.GetPhysicalDeviceMemoryProperties(vk->phys, &vk->mem_props);
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &host_props };
	vk->vk.GetPhysicalDeviceProperties2(vk->phys, &props);
	vk->host_ptr_align = (uint32_t)host_props.minImportedHostPointerAlignment;

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = vk->family, .queueCount = 1, .pQueuePriorities = &prio };
	const char *exts[] = {
		VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME,
		VK_VALVE_VIDEO_ENCODE_RGB_CONVERSION_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
		VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	};
	VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE rgb = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_RGB_CONVERSION_FEATURES_VALVE,
		.videoEncodeRgbConversion = VK_TRUE };
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
		.pNext = &rgb, .timelineSemaphore = VK_TRUE };
	VkPhysicalDeviceSynchronization2Features sync2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
		.pNext = &timeline, .synchronization2 = VK_TRUE };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &sync2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]),
		.ppEnabledExtensionNames = exts };
	if (vk->vk.CreateDevice(vk->phys, &dci, NULL, &vk->device) != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Creating the Vulkan encode device failed");
		goto err;
	}
	if (!load_device_api(vk)) {
		goto err;
	}
	vk->vk.GetDeviceQueue(vk->device, vk->family, 0, &vk->queue);

	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = vk->family };
	VkCommandBufferAllocateInfo cbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkSemaphoreTypeCreateInfo stc = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
	VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &stc };
	if (vk->vk.CreateCommandPool(vk->device, &cpi, NULL, &vk->cmd_pool) != VK_SUCCESS ||
			(cbi.commandPool = vk->cmd_pool,
				vk->vk.AllocateCommandBuffers(vk->device, &cbi, &vk->cmd)) != VK_SUCCESS ||
			vk->vk.CreateSemaphore(vk->device, &sci, NULL, &vk->timeline) != VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Encode command buffers failed");
		goto err;
	}

	wlr_log(WLR_INFO, "Vulkan AV1 encoding on %s (queue family %u, tiles up to "
		"%ux%u)", props.properties.deviceName, vk->family, a.maxTiles.width,
		a.maxTiles.height);
	pthread_mutex_init(&vk->lock, NULL);
	pthread_cond_init(&vk->cond, NULL);
	if (pthread_create(&vk->thread, NULL, vkencode_thread, vk) != 0) {
		goto err;
	}
	return vk;

err:
	if (vk->device != VK_NULL_HANDLE) {
		vk->vk.DestroyDevice(vk->device, NULL);
	}
	if (vk->instance != VK_NULL_HANDLE && vk->vk.DestroyInstance != NULL) {
		vk->vk.DestroyInstance(vk->instance, NULL);
	}
	if (vk->lib != NULL) {
		dlclose(vk->lib);
	}
	free(vk);
	return NULL;
}

void vkencode_destroy(struct gm_vkencode *vk) {
	if (vk == NULL) {
		return;
	}
	pthread_mutex_lock(&vk->lock);
	vk->quit = true;
	pthread_cond_signal(&vk->cond);
	pthread_mutex_unlock(&vk->lock);
	pthread_join(vk->thread, NULL);
	job_clear(&vk->pending);
	session_free(vk);
	for (int i = 0; i < MAX_IMPORTS; i++) {
		if (vk->imports[i].image != VK_NULL_HANDLE) {
			vk->vk.DestroyImageView(vk->device, vk->imports[i].view, NULL);
			vk->vk.DestroyImage(vk->device, vk->imports[i].image, NULL);
			vk->vk.FreeMemory(vk->device, vk->imports[i].memory, NULL);
		}
	}
	vk->vk.DestroySemaphore(vk->device, vk->timeline, NULL);
	vk->vk.DestroyCommandPool(vk->device, vk->cmd_pool, NULL);
	vk->vk.DestroyDevice(vk->device, NULL);
	vk->vk.DestroyInstance(vk->instance, NULL);
	pthread_mutex_destroy(&vk->lock);
	pthread_cond_destroy(&vk->cond);
	dlclose(vk->lib);
	free(vk);
}

void vkencode_start(struct gm_vkencode *vk, int bitrate_kbps, int refresh_mhz,
		int packet_bytes, int flags) {
	pthread_mutex_lock(&vk->lock);
	vk->active = true;
	vk->keyframe_wanted = true;
	vk->bitrate_kbps = bitrate_kbps;
	vk->refresh_mhz = refresh_mhz;
	vk->packet_bytes = packet_bytes > 0 ? packet_bytes : DEFAULT_PACKET;
	vk->chunked = (flags & GM_STREAM_CHUNKED) != 0;
	vk->client_hdr = (flags & GM_STREAM_HDR) != 0;
	if (hdr_content() && !vk->client_hdr) {
		wlr_log(WLR_INFO, "The composite is HDR but this client cannot show "
			"it; sending it as BT.709, which will look washed out until "
			"there is tone mapping");
	}
	pthread_mutex_unlock(&vk->lock);
}

void vkencode_stop(struct gm_vkencode *vk) {
	pthread_mutex_lock(&vk->lock);
	vk->active = false;
	job_clear(&vk->pending);
	pthread_mutex_unlock(&vk->lock);
}

void vkencode_set_rate(struct gm_vkencode *vk, int bitrate_kbps, int refresh_mhz) {
	pthread_mutex_lock(&vk->lock);
	if (bitrate_kbps > 0 && vk->bitrate_kbps != bitrate_kbps) {
		vk->bitrate_kbps = bitrate_kbps;
		vk->rate_gen++;
	}
	if (refresh_mhz > 0 && vk->refresh_mhz != refresh_mhz) {
		vk->refresh_mhz = refresh_mhz;
		vk->rate_gen++;
	}
	pthread_mutex_unlock(&vk->lock);
}

void vkencode_request_keyframe(struct gm_vkencode *vk) {
	pthread_mutex_lock(&vk->lock);
	vk->keyframe_wanted = true;
	pthread_mutex_unlock(&vk->lock);
}

void vkencode_take_stats(struct gm_vkencode *vk, unsigned *encoded,
		unsigned *dropped, uint64_t *encode_ns, unsigned *chunks,
		unsigned *patched, double *first_ms) {
	pthread_mutex_lock(&vk->lock);
	*encoded = vk->stats_encoded;
	*dropped = vk->stats_dropped;
	*encode_ns = vk->stats_encode_ns;
	*chunks = vk->stats_chunks;
	*patched = vk->stats_patched;
	*first_ms = vk->stats_encoded > 0
		? (double)vk->stats_first_ns / vk->stats_encoded / 1e6 : 0;
	vk->stats_encoded = vk->stats_dropped = vk->stats_chunks = vk->stats_patched = 0;
	vk->stats_encode_ns = vk->stats_first_ns = 0;
	pthread_mutex_unlock(&vk->lock);
}
