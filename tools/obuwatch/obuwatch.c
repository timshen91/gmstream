// Can we read a frame's AV1 bitstream while the GPU is still writing it?
//
// VA-API says no: one fence per picture, and mapping the coded buffer waits
// for it. But Mesa allocates that buffer's BO once and reuses it, and VCN
// writes the tile-group OBUs into it in order, after the sequence and frame
// headers that the driver itself writes on the CPU at submit time. So if we
// keep one coded buffer alive and hold the mapping we took while it was idle,
// we can watch the bitstream appear.
//
// This measures that: the buffer is pre-filled with a position-keyed pattern,
// so the write frontier is where the pattern resumes; the frontier is polled
// while the frame encodes, and checked afterwards against the bitstream
// FFmpeg hands back, byte for byte. With `tile_groups` > 1 each tile group is
// its own OBU, so the frontier tells us when a whole tile group is ready to
// send.
//
// FFmpeg's and libva's calls are redirected at link time (--wrap), so nothing
// in either library is patched.
//
// Build in the gmstream build image (build/Containerfile):
//   gcc -O2 -o obuwatch obuwatch.c \
//     $(pkg-config --cflags libavcodec libavutil libva-drm libva libdrm) \
//     -Wl,--wrap=vaCreateBuffer -Wl,--wrap=vaDestroyBuffer \
//     -Wl,--wrap=vaMapBuffer -Wl,--wrap=vaEndPicture \
//     -Wl,--start-group $(pkg-config --libs --static libavcodec libavutil \
//     libva-drm libva libdrm) -Wl,--end-group -lm -lpthread
// Run on the device: ./obuwatch [width height bitrate_kbps frames]

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
#include <va/va.h>
#include <va/va_drm.h>

#define MAX_SAMPLES 4096
#define MAX_SEGMENTS 16

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ---- the one coded buffer we keep ---------------------------------------- */

VAStatus __real_vaCreateBuffer(VADisplay, VAContextID, VABufferType, unsigned int,
	unsigned int, void *, VABufferID *);
VAStatus __real_vaDestroyBuffer(VADisplay, VABufferID);
VAStatus __real_vaMapBuffer(VADisplay, VABufferID, void **);
VAStatus __real_vaEndPicture(VADisplay, VAContextID);

static struct {
	VADisplay dpy;
	VABufferID id;
	unsigned size;
	volatile uint8_t *map; // the BO, mapped while it was idle
	// The segments of the frame FFmpeg last mapped: where its bytes live in
	// the BO. The last one is what the GPU wrote.
	struct { unsigned offset, size; } seg[MAX_SEGMENTS];
	unsigned segments;
} coded = { .id = VA_INVALID_ID };

static uint8_t pattern(unsigned i) {
	return (uint8_t)(0x5a ^ (i * 31u) ^ (i >> 8) ^ (i >> 17));
}

// Where the pattern resumes for good: everything before it has been written.
// 32 bytes of agreement is proof enough that nothing was written there.
static unsigned frontier(unsigned from, unsigned to) {
	const volatile uint8_t *p = coded.map;
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

VAStatus __wrap_vaCreateBuffer(VADisplay dpy, VAContextID ctx, VABufferType type,
		unsigned int size, unsigned int num, void *data, VABufferID *out) {
	if (type != VAEncCodedBufferType) {
		return __real_vaCreateBuffer(dpy, ctx, type, size, num, data, out);
	}
	if (coded.id == VA_INVALID_ID) {
		VAStatus st = __real_vaCreateBuffer(dpy, ctx, type, size, num, data, &coded.id);
		if (st != VA_STATUS_SUCCESS) {
			return st;
		}
		coded.dpy = dpy;
		coded.size = size * (num ? num : 1);
		fprintf(stderr, "coded buffer %u, %u bytes\n", coded.id, coded.size);
	}
	*out = coded.id;
	return VA_STATUS_SUCCESS;
}

VAStatus __wrap_vaDestroyBuffer(VADisplay dpy, VABufferID id) {
	if (id == coded.id) {
		return VA_STATUS_SUCCESS; // ours; it outlives the encoder
	}
	return __real_vaDestroyBuffer(dpy, id);
}

VAStatus __wrap_vaMapBuffer(VADisplay dpy, VABufferID id, void **p) {
	VAStatus st = __real_vaMapBuffer(dpy, id, p);
	if (st != VA_STATUS_SUCCESS || id != coded.id) {
		return st;
	}
	// The segment list points into the BO; the first segment's address is its
	// base (the driver writes the headers from offset 0).
	VACodedBufferSegment *s = *p;
	uint8_t *base = s->buf;
	coded.segments = 0;
	for (VACodedBufferSegment *n = s; n != NULL && coded.segments < MAX_SEGMENTS;
			n = n->next) {
		if ((uint8_t *)n->buf < base) {
			base = n->buf;
		}
		coded.segments++;
	}
	if (coded.map == NULL) {
		coded.map = base;
		base[0] = 0x5a;
		base[1] = 0xa5;
		fprintf(stderr, "bitstream at %p, writable: %s\n", (void *)base,
			base[0] == 0x5a && base[1] == 0xa5 ? "yes" : "NO");
	} else if ((volatile uint8_t *)base != coded.map) {
		fprintf(stderr, "WARNING: mapping moved: %p now %p\n",
			(void *)coded.map, (void *)base);
	}
	unsigned i = 0;
	for (VACodedBufferSegment *n = s; n != NULL && i < MAX_SEGMENTS; n = n->next, i++) {
		coded.seg[i].offset = (unsigned)((uint8_t *)n->buf - (uint8_t *)coded.map);
		coded.seg[i].size = n->size;
	}
	return st;
}

/* ---- the watcher -------------------------------------------------------- */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool watching, quit, reported;
	uint64_t submitted;
	unsigned from; // where the GPU's part starts (the last segment's offset)
	unsigned to;   // and how far a frame can reach past it
	// (time since submit, frontier) while the frame encodes.
	struct { uint64_t t; unsigned at; } sample[MAX_SAMPLES];
	unsigned samples;
	// Every byte as it first appeared. If the GPU ever goes back and changes
	// one, this and the final bitstream disagree, and streaming a byte as
	// soon as it is written would send the wrong one.
	uint8_t *shadow;
	unsigned shadow_len;
} watch = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

VAStatus __wrap_vaEndPicture(VADisplay dpy, VAContextID ctx) {
	// The previous frame is long done, so the buffer is idle: lay the pattern
	// down before the driver writes this frame's headers into it.
	if (coded.map != NULL) {
		volatile uint8_t *p = coded.map;
		for (unsigned i = watch.from; i < watch.to; i++) {
			p[i] = pattern(i);
		}
	}
	uint64_t t0 = now_ns();
	VAStatus st = __real_vaEndPicture(dpy, ctx);
	if (coded.map != NULL && st == VA_STATUS_SUCCESS) {
		pthread_mutex_lock(&watch.lock);
		watch.submitted = t0;
		watch.samples = 0;
		watch.watching = true;
		pthread_cond_signal(&watch.cond);
		pthread_mutex_unlock(&watch.lock);
	}
	return st;
}

static void *watcher(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&watch.lock);
		while (!watch.watching && !watch.quit) {
			pthread_cond_wait(&watch.cond, &watch.lock);
		}
		if (watch.quit) {
			pthread_mutex_unlock(&watch.lock);
			return NULL;
		}
		uint64_t t0 = watch.submitted;
		unsigned from = watch.from, to = watch.to;
		pthread_mutex_unlock(&watch.lock);

		unsigned at = from, n = 0;
		for (;;) {
			unsigned f = frontier(at, to);
			if (f != at || n == 0) {
				// Copy what just appeared before the GPU can touch it again.
				for (unsigned i = at; i < f; i++) {
					watch.shadow[i] = coded.map[i];
				}
				watch.shadow_len = f;
				at = f;
				if (n < MAX_SAMPLES) {
					watch.sample[n].t = now_ns() - t0;
					watch.sample[n].at = f;
					n++;
				}
			}
			pthread_mutex_lock(&watch.lock);
			bool done = !watch.watching;
			pthread_mutex_unlock(&watch.lock);
			if (done) {
				break;
			}
		}
		pthread_mutex_lock(&watch.lock);
		watch.samples = n;
		watch.reported = true;
		pthread_cond_broadcast(&watch.cond);
		pthread_mutex_unlock(&watch.lock);
	}
}

// Ends the watch and waits for its samples.
static unsigned watch_stop(void) {
	pthread_mutex_lock(&watch.lock);
	if (!watch.watching) {
		pthread_mutex_unlock(&watch.lock);
		return 0;
	}
	watch.watching = false;
	watch.reported = false;
	while (!watch.reported) {
		pthread_cond_wait(&watch.cond, &watch.lock);
	}
	unsigned n = watch.samples;
	pthread_mutex_unlock(&watch.lock);
	return n;
}

/* ---- content ------------------------------------------------------------ */

static VADisplay va;
static AVBufferRef *device;

static void fill(uint8_t *y, uint8_t *uv, int width, int height, int stride, int frame) {
	double t = frame * 0.11;
	for (int r = 0; r < height; r++) {
		uint8_t *line = y + (size_t)r * stride;
		for (int x = 0; x < width; x++) {
			double v = sin(x * 0.03 + t) + sin(r * 0.021 - t * 1.3) +
				sin((x + r) * 0.017 + t * 0.7);
			unsigned n = ((x * 1103515245u + r * 12345u + frame * 2654435761u) >> 13) & 0x1f;
			int p = (int)((v + 3) * 36) + (int)n;
			line[x] = (uint8_t)(p < 16 ? 16 : p > 235 ? 235 : p);
		}
	}
	for (int r = 0; r < height / 2; r++) {
		uint8_t *line = uv + (size_t)r * stride;
		for (int x = 0; x < width; x += 2) {
			line[x] = (uint8_t)(128 + 40 * sin(r * 0.02 + t));
			line[x + 1] = (uint8_t)(128 + 40 * sin(x * 0.01 - t));
		}
	}
}

static bool upload(AVFrame *frame, int width, int height, int n) {
	VAImage image;
	if (vaDeriveImage(va, (VASurfaceID)(uintptr_t)frame->data[3], &image) !=
			VA_STATUS_SUCCESS) {
		return false;
	}
	void *p;
	bool ok = false;
	if (vaMapBuffer(va, image.buf, &p) == VA_STATUS_SUCCESS) {
		fill((uint8_t *)p + image.offsets[0], (uint8_t *)p + image.offsets[1],
			width, height, image.pitches[0], n);
		vaUnmapBuffer(va, image.buf);
		ok = true;
	}
	vaDestroyImage(va, image.image_id);
	return ok;
}

/* ---- one run ------------------------------------------------------------ */

// OBUs in `data`: their end offsets, so we can say when each tile group is
// whole. Returns how many were found.
static int obu_ends(const uint8_t *data, unsigned len, unsigned *ends, int max) {
	int n = 0;
	unsigned i = 0;
	while (i < len && n < max) {
		uint8_t h = data[i];
		if (h & 0x80) {
			return n;
		}
		unsigned p = i + ((h & 0x04) ? 2 : 1);
		unsigned size = 0;
		if (h & 0x02) {
			for (int shift = 0; shift < 8; shift++) {
				if (p >= len) {
					return n;
				}
				size |= (unsigned)(data[p] & 0x7f) << (7 * shift);
				bool more = data[p] & 0x80;
				p++;
				if (!more) {
					break;
				}
			}
		} else {
			size = len - p;
		}
		if (p + size > len) {
			return n;
		}
		i = p + size;
		ends[n++] = i;
	}
	return n;
}

static void run(int groups, int tile_cols, int tile_rows, int width, int height, int kbps, int frames) {
	AVBufferRef *pool = av_hwframe_ctx_alloc(device);
	AVHWFramesContext *fc = (AVHWFramesContext *)pool->data;
	fc->format = AV_PIX_FMT_VAAPI;
	fc->sw_format = AV_PIX_FMT_NV12;
	fc->width = width;
	fc->height = height;
	fc->initial_pool_size = 3;
	if (av_hwframe_ctx_init(pool) < 0) {
		printf("%d groups: no surfaces\n", groups);
		return;
	}

	const AVCodec *impl = avcodec_find_encoder_by_name("av1_vaapi");
	AVCodecContext *ctx = avcodec_alloc_context3(impl);
	ctx->width = width;
	ctx->height = height;
	ctx->pix_fmt = AV_PIX_FMT_VAAPI;
	ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
	ctx->hw_frames_ctx = av_buffer_ref(pool);
	ctx->time_base = (AVRational){ 1, 1000000 };
	ctx->framerate = (AVRational){ 60, 1 };
	ctx->max_b_frames = 0;
	ctx->gop_size = 1 << 30;
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx->bit_rate = (int64_t)kbps * 1000;
	ctx->rc_max_rate = ctx->bit_rate;
	ctx->rc_buffer_size = (int)(ctx->bit_rate * 2 / 60);
	ctx->color_range = AVCOL_RANGE_MPEG;
	ctx->colorspace = AVCOL_SPC_BT709;
	ctx->profile = AV_PROFILE_AV1_MAIN;
	av_opt_set(ctx->priv_data, "async_depth", "1", 0);
	av_opt_set(ctx->priv_data, "rc_mode", "VBR", 0);
	if (groups > 1) {
		char tiles[32], g[8];
		snprintf(tiles, sizeof(tiles), "%dx%d", tile_cols, tile_rows);
		snprintf(g, sizeof(g), "%d", groups);
		int a = av_opt_set(ctx->priv_data, "tiles", tiles, 0);
		int b = av_opt_set(ctx->priv_data, "tile_groups", g, 0);
		printf("  tiles=%s (%s), tile_groups=%s (%s)\n", tiles,
			a == 0 ? "ok" : av_err2str(a), g, b == 0 ? "ok" : av_err2str(b));
	}
	int ret = avcodec_open2(ctx, impl, NULL);
	if (ret < 0) {
		printf("%d groups: %s\n", groups, av_err2str(ret));
		avcodec_free_context(&ctx);
		av_buffer_unref(&pool);
		return;
	}

	AVFrame *src[2] = { 0 };
	for (int c = 0; c < 2; c++) {
		src[c] = av_frame_alloc();
		if (av_hwframe_get_buffer(pool, src[c], 0) < 0 || !upload(src[c], width, height, c)) {
			printf("%d groups: no source\n", groups);
			return;
		}
	}
	AVPacket *packet = av_packet_alloc();

	double sum_last = 0, sum_first = 0;
	uint64_t watched_sum = 0;
	int runs = 0, mismatches = 0, rewrites = 0;
	unsigned rw_first = ~0u, rw_last = 0, rw_bytes = 0, rw_at[256], rw_seen = 0;
	unsigned bytes = 0;
	double group_at[64] = { 0 };
	int group_n[64] = { 0 };

	for (int n = 0; n < frames; n++) {
		AVFrame *f = src[n % 2];
		f->pts = n * 16666;
		if (avcodec_send_frame(ctx, f) < 0) {
			break;
		}
		if (avcodec_receive_packet(ctx, packet) < 0) {
			continue;
		}
		unsigned samples = watch_stop();

		// Where the GPU's part of the bitstream starts, for the next frame.
		unsigned gpu_from = 0, gpu_at = 0;
		for (unsigned i = 0; i < coded.segments; i++) {
			if (coded.seg[i].offset > gpu_from) {
				gpu_from = coded.seg[i].offset;
			}
		}
		for (unsigned i = 0; i < coded.segments; i++) {
			if (coded.seg[i].offset < gpu_from) {
				gpu_at += coded.seg[i].size;
			}
		}
		pthread_mutex_lock(&watch.lock);
		watch.from = gpu_from;
		watch.to = gpu_from + (coded.size - gpu_from < 4u << 20 ?
			coded.size - gpu_from : 4u << 20);
		pthread_mutex_unlock(&watch.lock);

		if (n < 8 || coded.map == NULL || samples == 0) {
			av_packet_unref(packet);
			continue; // the first frames are keyframes, and set things up
		}

		// Everything we watched must match the bitstream FFmpeg handed back.
		// The GPU's part sits at gpu_from in the BO and at gpu_at in the packet.
		unsigned gpu_len = packet->size - gpu_at;
		for (unsigned i = 0; i < gpu_len; i++) {
			if (coded.map[gpu_from + i] != packet->data[gpu_at + i]) {
				mismatches++;
				break;
			}
		}
		// And so must every byte as we first saw it. Where it does not, the
		// GPU came back and changed a byte it had already written.
		unsigned watched = watch.shadow_len > gpu_from ? watch.shadow_len - gpu_from : 0;
		if (watched > gpu_len) {
			watched = gpu_len;
		}
		unsigned first = ~0u, last = 0, n_rw = 0;
		for (unsigned i = 0; i < watched; i++) {
			if (watch.shadow[gpu_from + i] != packet->data[gpu_at + i]) {
				if (first == ~0u) {
					first = i;
				}
				last = i;
				n_rw++;
				if (n_rw <= 24) {
					rw_at[rw_seen % 256] = i;
					rw_seen++;
				}
			}
		}
		if (n_rw > 0) {
			rewrites++;
			if (first < rw_first) {
				rw_first = first;
			}
			if (last > rw_last) {
				rw_last = last;
			}
			rw_bytes += n_rw;
		}
		watched_sum += watched;

		// When each tile group OBU was whole: the first sample whose frontier
		// reached its end.
		unsigned ends[64];
		int nobu = obu_ends(packet->data + gpu_at, gpu_len, ends, 64);
		for (int g = 0; g < nobu; g++) {
			for (unsigned s = 0; s < samples; s++) {
				if (watch.sample[s].at >= gpu_from + ends[g]) {
					group_at[g] += watch.sample[s].t / 1e6;
					group_n[g]++;
					break;
				}
			}
		}
		if (samples > 0) {
			sum_first += watch.sample[0].t / 1e6;
			sum_last += watch.sample[samples - 1].t / 1e6;
		}
		bytes += packet->size;
		runs++;
		if (runs == 1) {
			printf("  %d groups: %u segments, GPU part at %u (%u in the packet), "
				"%d OBUs, %u bytes\n", groups, coded.segments, gpu_from, gpu_at,
				nobu, packet->size);
			printf("    frontier:");
			for (unsigned s = 0; s < samples && s < 12; s++) {
				printf(" %.2fms:%u", watch.sample[s].t / 1e6,
					watch.sample[s].at - gpu_from);
			}
			printf("%s\n", samples > 12 ? " ..." : "");
		}
		av_packet_unref(packet);
	}

	printf("  %d groups: %d frames, %.0f kB/frame, first write %.2f ms, "
		"last %.2f ms, %d mismatches, %d rewrites, %.0f%% watched as written\n",
		groups, runs, runs ? (double)bytes / runs / 1024 : 0,
		runs ? sum_first / runs : 0, runs ? sum_last / runs : 0, mismatches,
		rewrites, bytes ? 100.0 * watched_sum / bytes : 0);
	if (rewrites > 0) {
		printf("    rewritten bytes: %u in %d frames, offsets %u..%u; first few:",
			rw_bytes, rewrites, rw_first, rw_last);
		for (unsigned i = 0; i < rw_seen && i < 24; i++) {
			printf(" %u", rw_at[i]);
		}
		printf("\n");
	}
	for (int g = 0; g < 64; g++) {
		if (group_n[g] > 0) {
			printf("    tile group %-2d whole at %.2f ms (%d of %d frames)\n", g,
				group_at[g] / group_n[g], group_n[g], runs);
		}
	}

	av_packet_free(&packet);
	for (int c = 0; c < 2; c++) {
		av_frame_free(&src[c]);
	}
	avcodec_free_context(&ctx);
	av_buffer_unref(&pool);
}

int main(int argc, char **argv) {
	int width = argc > 1 ? atoi(argv[1]) : 2940;
	int height = argc > 2 ? atoi(argv[2]) : 1846;
	int kbps = argc > 3 ? atoi(argv[3]) : 100000;
	int frames = argc > 4 ? atoi(argv[4]) : 60;
	av_log_set_level(getenv("GM_VERBOSE") ? AV_LOG_VERBOSE : AV_LOG_ERROR);

	int fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) { perror("renderD128"); return 1; }
	va = vaGetDisplayDRM(fd);
	int major, minor;
	if (vaInitialize(va, &major, &minor) != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize failed\n");
		return 1;
	}
	device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
	((AVVAAPIDeviceContext *)((AVHWDeviceContext *)device->data)->hwctx)->display = va;
	if (av_hwdevice_ctx_init(device) < 0) {
		fprintf(stderr, "hwdevice init failed\n");
		return 1;
	}
	watch.shadow = calloc(1, 16u << 20);
	pthread_t thread;
	pthread_create(&thread, NULL, watcher, NULL);

	printf("%s\nAV1 %dx%d at %d kbps, %d frames each\n", vaQueryVendorString(va),
		width, height, kbps, frames);
	run(1, 1, 1, width, height, kbps, frames);
	run(2, 1, 2, width, height, kbps, frames);
	run(4, 1, 4, width, height, kbps, frames);
	run(4, 2, 2, width, height, kbps, frames);
	run(8, 1, 8, width, height, kbps, frames);
	run(8, 2, 4, width, height, kbps, frames);

	pthread_mutex_lock(&watch.lock);
	watch.quit = true;
	pthread_cond_signal(&watch.cond);
	pthread_mutex_unlock(&watch.lock);
	pthread_join(thread, NULL);
	return 0;
}
