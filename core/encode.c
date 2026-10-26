// Video encoding: one GPU copy of gamescope's frame, then VA-API encode.
//
// gamescope composites into a fixed ring of three images and never waits for
// wl_buffer.release, so its buffer cannot be encoded in place. At present time
// the compositor thread imports the dma-buf as a VA surface (cached per
// buffer) and runs one VA video-processing blit into an NV12 surface: that is
// both the copy and the RGB→YUV conversion. The worker thread then encodes the
// NV12 surface with FFmpeg's VA-API encoder (no B-frames, async depth 1, an
// open-ended GOP with IDR frames on request) and hands the bitstream to the
// callback.
//
// The worker holds at most one pending frame; a newer frame replaces it
// (counted as dropped). Nothing on the compositor thread waits for the GPU.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <wlr/util/log.h>

#include "core.h"

// A gamescope buffer imported as a VA surface; lives as long as the buffer.
struct import {
	struct gm_encoder *enc;
	struct wlr_buffer *buffer;
	VASurfaceID surface;
	struct wl_listener destroy;
	struct wl_list link;
};

struct job {
	AVFrame *frame; // NV12 VA surface from `frames`
	uint64_t slot_ns;
};

struct gm_encoder {
	const struct gm_callbacks *cb;
	int drm_fd;
	VADisplay va;
	AVBufferRef *device; // AVHWDeviceContext wrapping `va`
	uint32_t supported;
	struct wl_list imports;

	// Compositor thread: the NV12 pool for the current size, and one VPP
	// context for all sizes. It is never destroyed while encoding: the worker
	// may still be waiting on a copy it queued (destroying it under a pending
	// copy left Mesa spinning with its lock held).
	AVBufferRef *frames;
	int frames_width, frames_height;
	VAConfigID vpp_config;
	VAContextID vpp_context;
	// Non-dma-buf frames (gamescope's single-pixel fill, shm) are copied
	// into this surface on the CPU first.
	VASurfaceID upload;
	int upload_width, upload_height;

	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	// Protected by `lock`.
	bool quit;
	bool active;
	struct job pending;
	enum gm_codec codec;
	int bitrate_kbps, refresh_mhz;
	unsigned settings_gen; // bumped whenever the encoder must be reopened
	bool keyframe;
	unsigned stats_encoded, stats_dropped;
	uint64_t stats_encode_ns;

	// PyroWave runs instead of VA-API while it is the session's codec.
	struct gm_pyrowave *pw;
	bool pw_active; // compositor thread

	// Worker thread only. Frame ids run on across encoder sessions so the
	// client can tell a gap from a restart.
	uint32_t next_frame_id;
};

static const char *codec_names[] = {
	[GM_CODEC_H264] = "h264_vaapi",
	[GM_CODEC_HEVC] = "hevc_vaapi",
	[GM_CODEC_AV1] = "av1_vaapi",
};

/* ---- import + copy (compositor thread) ---------------------------------- */

static uint32_t va_fourcc(uint32_t drm_format, unsigned *rt_format) {
	*rt_format = VA_RT_FORMAT_RGB32;
	switch (drm_format) {
	case DRM_FORMAT_XRGB8888: return VA_FOURCC_BGRX;
	case DRM_FORMAT_ARGB8888: return VA_FOURCC_BGRA;
	case DRM_FORMAT_XBGR8888: return VA_FOURCC_RGBX;
	case DRM_FORMAT_ABGR8888: return VA_FOURCC_RGBA;
	}
	*rt_format = VA_RT_FORMAT_RGB32_10;
	switch (drm_format) {
	case DRM_FORMAT_XRGB2101010: return VA_FOURCC_X2R10G10B10;
	case DRM_FORMAT_ARGB2101010: return VA_FOURCC_A2R10G10B10;
	case DRM_FORMAT_XBGR2101010: return VA_FOURCC_X2B10G10R10;
	case DRM_FORMAT_ABGR2101010: return VA_FOURCC_A2B10G10R10;
	}
	return 0;
}

static void import_destroy(struct import *imp) {
	vaDestroySurfaces(imp->enc->va, &imp->surface, 1);
	wl_list_remove(&imp->destroy.link);
	wl_list_remove(&imp->link);
	free(imp);
}

static void handle_import_buffer_destroy(struct wl_listener *listener,
		void *data) {
	struct import *imp = wl_container_of(listener, imp, destroy);
	import_destroy(imp);
}

static VASurfaceID import_buffer(struct gm_encoder *enc,
		struct wlr_buffer *buffer) {
	struct import *imp;
	wl_list_for_each(imp, &enc->imports, link) {
		if (imp->buffer == buffer) {
			return imp->surface;
		}
	}

	struct wlr_dmabuf_attributes attr;
	wlr_buffer_get_dmabuf(buffer, &attr); // checked by the caller
	unsigned rt_format;
	VADRMPRIMESurfaceDescriptor desc = {
		.fourcc = va_fourcc(attr.format, &rt_format),
		.width = attr.width,
		.height = attr.height,
		.num_layers = 1,
		.layers[0] = {
			.drm_format = attr.format,
			.num_planes = attr.n_planes,
		},
	};
	if (desc.fourcc == 0) {
		wlr_log(WLR_ERROR, "Unsupported frame format 0x%08x", attr.format);
		return VA_INVALID_SURFACE;
	}
	for (int i = 0; i < attr.n_planes; i++) {
		uint32_t obj = desc.num_objects;
		for (uint32_t j = 0; j < desc.num_objects; j++) {
			if (desc.objects[j].fd == attr.fd[i]) {
				obj = j;
			}
		}
		if (obj == desc.num_objects) {
			off_t size = lseek(attr.fd[i], 0, SEEK_END);
			desc.objects[obj].fd = attr.fd[i];
			desc.objects[obj].size = size > 0 ? (uint32_t)size : 0;
			desc.objects[obj].drm_format_modifier = attr.modifier;
			desc.num_objects++;
		}
		desc.layers[0].object_index[i] = obj;
		desc.layers[0].offset[i] = attr.offset[i];
		desc.layers[0].pitch[i] = attr.stride[i];
	}

	VASurfaceAttrib attribs[] = {
		{
			.type = VASurfaceAttribMemoryType,
			.flags = VA_SURFACE_ATTRIB_SETTABLE,
			.value = { .type = VAGenericValueTypeInteger,
				.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 },
		},
		{
			.type = VASurfaceAttribExternalBufferDescriptor,
			.flags = VA_SURFACE_ATTRIB_SETTABLE,
			.value = { .type = VAGenericValueTypePointer,
				.value.p = &desc },
		},
	};
	VASurfaceID surface;
	VAStatus st = vaCreateSurfaces(enc->va, rt_format, attr.width,
		attr.height, &surface, 1, attribs, 2);
	if (st != VA_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "Importing frame failed: %s (format 0x%08x, "
			"modifier 0x%016llx)", vaErrorStr(st), attr.format,
			(unsigned long long)attr.modifier);
		return VA_INVALID_SURFACE;
	}

	imp = calloc(1, sizeof(*imp));
	imp->enc = enc;
	imp->buffer = buffer;
	imp->surface = surface;
	imp->destroy.notify = handle_import_buffer_destroy;
	wl_signal_add(&buffer->events.destroy, &imp->destroy);
	wl_list_insert(&enc->imports, &imp->link);
	return surface;
}

// Copies a buffer we can only read on the CPU into `enc->upload`.
static VASurfaceID upload_buffer(struct gm_encoder *enc,
		struct wlr_buffer *buffer) {
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "Frame is neither a dma-buf nor readable");
		return VA_INVALID_SURFACE;
	}
	VASurfaceID result = VA_INVALID_SURFACE;
	bool swap = format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888;
	if (!swap && format != DRM_FORMAT_ARGB8888 &&
			format != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "Unsupported CPU frame format 0x%08x", format);
		goto out;
	}
	if (enc->upload != VA_INVALID_SURFACE && (enc->upload_width != buffer->width ||
			enc->upload_height != buffer->height)) {
		vaDestroySurfaces(enc->va, &enc->upload, 1);
		enc->upload = VA_INVALID_SURFACE;
	}
	if (enc->upload == VA_INVALID_SURFACE) {
		VASurfaceAttrib attrib = {
			.type = VASurfaceAttribPixelFormat,
			.flags = VA_SURFACE_ATTRIB_SETTABLE,
			.value = { .type = VAGenericValueTypeInteger,
				.value.i = VA_FOURCC_BGRA },
		};
		if (vaCreateSurfaces(enc->va, VA_RT_FORMAT_RGB32, buffer->width,
				buffer->height, &enc->upload, 1, &attrib, 1) != VA_STATUS_SUCCESS) {
			enc->upload = VA_INVALID_SURFACE;
			goto out;
		}
		enc->upload_width = buffer->width;
		enc->upload_height = buffer->height;
	}
	VAImage image;
	void *dst;
	if (vaDeriveImage(enc->va, enc->upload, &image) != VA_STATUS_SUCCESS) {
		goto out;
	}
	if (vaMapBuffer(enc->va, image.buf, &dst) == VA_STATUS_SUCCESS) {
		for (int y = 0; y < buffer->height; y++) {
			const uint8_t *s = (const uint8_t *)data + (size_t)y * stride;
			uint8_t *d = (uint8_t *)dst + image.offsets[0] +
				(size_t)y * image.pitches[0];
			for (int x = 0; x < buffer->width; x++) {
				// To B, G, R, A bytes, opaque.
				d[4 * x + 0] = s[4 * x + (swap ? 2 : 0)];
				d[4 * x + 1] = s[4 * x + 1];
				d[4 * x + 2] = s[4 * x + (swap ? 0 : 2)];
				d[4 * x + 3] = 0xff;
			}
		}
		vaUnmapBuffer(enc->va, image.buf);
		result = enc->upload;
	}
	vaDestroyImage(enc->va, image.image_id);
out:
	wlr_buffer_end_data_ptr_access(buffer);
	return result;
}

static void free_frames(struct gm_encoder *enc) {
	// Frames still queued or encoding keep their pool alive.
	av_buffer_unref(&enc->frames);
	enc->frames_width = enc->frames_height = 0;
}

static bool ensure_frames(struct gm_encoder *enc, int width, int height) {
	if (enc->frames != NULL && enc->frames_width == width &&
			enc->frames_height == height) {
		return true;
	}
	free_frames(enc);

	AVBufferRef *ref = av_hwframe_ctx_alloc(enc->device);
	AVHWFramesContext *fc = (AVHWFramesContext *)ref->data;
	fc->format = AV_PIX_FMT_VAAPI;
	fc->sw_format = AV_PIX_FMT_NV12;
	fc->width = width;
	fc->height = height;
	fc->initial_pool_size = 4; // pending + encoding + slack
	int ret = av_hwframe_ctx_init(ref);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "Creating %dx%d NV12 surfaces failed: %s",
			width, height, av_err2str(ret));
		av_buffer_unref(&ref);
		return false;
	}
	enc->frames = ref;
	enc->frames_width = width;
	enc->frames_height = height;
	return true;
}

static bool copy_frame(struct gm_encoder *enc, VASurfaceID src,
		VASurfaceID dst) {
	VAProcPipelineParameterBuffer params = {
		.surface = src,
		.surface_color_standard = VAProcColorStandardSRGB,
		.output_background_color = 0xff000000,
		.output_color_standard = VAProcColorStandardBT709,
		.filter_flags = VA_FRAME_PICTURE,
		.input_color_properties.color_range = VA_SOURCE_RANGE_FULL,
		.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED,
	};
	VABufferID buf;
	VAStatus st = vaCreateBuffer(enc->va, enc->vpp_context,
		VAProcPipelineParameterBufferType, sizeof(params), 1, &params, &buf);
	if (st != VA_STATUS_SUCCESS) {
		return false;
	}
	st = vaBeginPicture(enc->va, enc->vpp_context, dst);
	if (st == VA_STATUS_SUCCESS) {
		st = vaRenderPicture(enc->va, enc->vpp_context, &buf, 1);
		VAStatus end = vaEndPicture(enc->va, enc->vpp_context);
		if (st == VA_STATUS_SUCCESS) {
			st = end;
		}
	}
	vaDestroyBuffer(enc->va, buf);
	if (st != VA_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "VPP copy failed: %s", vaErrorStr(st));
		return false;
	}
	return true;
}

void encoder_submit(struct gm_encoder *enc, struct wlr_buffer *buffer,
		int width, int height, uint64_t slot_ns) {
	if (enc->pw_active) {
		pyrowave_submit(enc->pw, buffer, width, height, slot_ns);
		return;
	}
	pthread_mutex_lock(&enc->lock);
	bool active = enc->active;
	pthread_mutex_unlock(&enc->lock);
	if (!active) {
		return;
	}

	struct wlr_dmabuf_attributes attr;
	VASurfaceID src = wlr_buffer_get_dmabuf(buffer, &attr) ?
		import_buffer(enc, buffer) : upload_buffer(enc, buffer);
	if (src == VA_INVALID_SURFACE || !ensure_frames(enc, width, height)) {
		return;
	}
	AVFrame *frame = av_frame_alloc();
	if (av_hwframe_get_buffer(enc->frames, frame, 0) < 0) {
		// Every surface is in use: the encoder is behind; skip this frame.
		av_frame_free(&frame);
		pthread_mutex_lock(&enc->lock);
		enc->stats_dropped++;
		pthread_mutex_unlock(&enc->lock);
		return;
	}
	if (!copy_frame(enc, src, (VASurfaceID)(uintptr_t)frame->data[3])) {
		av_frame_free(&frame);
		return;
	}

	pthread_mutex_lock(&enc->lock);
	if (enc->pending.frame != NULL) {
		av_frame_free(&enc->pending.frame);
		enc->stats_dropped++;
	}
	enc->pending = (struct job){ .frame = frame, .slot_ns = slot_ns };
	pthread_cond_signal(&enc->cond);
	pthread_mutex_unlock(&enc->lock);
}

/* ---- encoding (worker thread) ------------------------------------------- */

struct session {
	AVCodecContext *ctx;
	AVPacket *packet;
	enum gm_codec codec;
	unsigned gen;
	AVBufferRef *frames; // the pool the context was opened for
};

static void session_close(struct session *s) {
	avcodec_free_context(&s->ctx);
	av_buffer_unref(&s->frames);
}

static bool session_open(struct gm_encoder *enc, struct session *s,
		AVFrame *frame, enum gm_codec codec, int bitrate_kbps,
		int refresh_mhz) {
	const AVCodec *codec_impl = avcodec_find_encoder_by_name(codec_names[codec]);
	if (codec_impl == NULL) {
		return false;
	}
	const char *rc_modes[] = { "VBR", "CBR" };
	for (size_t i = 0; i < sizeof(rc_modes) / sizeof(rc_modes[0]); i++) {
		AVCodecContext *ctx = avcodec_alloc_context3(codec_impl);
		ctx->width = frame->width;
		ctx->height = frame->height;
		ctx->pix_fmt = AV_PIX_FMT_VAAPI;
		ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
		ctx->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
		ctx->time_base = (AVRational){ 1, 1000000 }; // pts in µs
		ctx->framerate = (AVRational){ refresh_mhz, 1000 };
		ctx->max_b_frames = 0;
		ctx->gop_size = 1 << 30; // IDR only when asked for
		ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
		ctx->bit_rate = (int64_t)bitrate_kbps * 1000;
		ctx->rc_max_rate = ctx->bit_rate;
		// A VBV of about two frames keeps every frame close to its share.
		ctx->rc_buffer_size =
			(int)(ctx->bit_rate * 2000 / (refresh_mhz > 0 ? refresh_mhz : 60000));
		ctx->color_range = AVCOL_RANGE_MPEG;
		ctx->colorspace = AVCOL_SPC_BT709;
		ctx->color_primaries = AVCOL_PRI_BT709;
		ctx->color_trc = AVCOL_TRC_BT709;
		switch (codec) {
		case GM_CODEC_H264:
			ctx->profile = AV_PROFILE_H264_HIGH;
			av_opt_set(ctx->priv_data, "aud", "0", 0);
			av_opt_set(ctx->priv_data, "sei", "0", 0);
			break;
		case GM_CODEC_HEVC:
			ctx->profile = AV_PROFILE_HEVC_MAIN;
			av_opt_set(ctx->priv_data, "aud", "0", 0);
			break;
		case GM_CODEC_AV1:
			ctx->profile = AV_PROFILE_AV1_MAIN;
			break;
		case GM_CODEC_PYROWAVE:
			break; // not a VA-API codec (pyrowave.c)
		}
		av_opt_set(ctx->priv_data, "async_depth", "1", 0);
		av_opt_set(ctx->priv_data, "rc_mode", rc_modes[i], 0);

		int ret = avcodec_open2(ctx, codec_impl, NULL);
		if (ret == 0) {
			wlr_log(WLR_INFO, "Encoding %s %dx%d at %d kbps (%s)",
				codec_names[codec], frame->width, frame->height,
				bitrate_kbps, rc_modes[i]);
			s->ctx = ctx;
			s->codec = codec;
			s->frames = av_buffer_ref(frame->hw_frames_ctx);
			return true;
		}
		wlr_log(WLR_ERROR, "Opening %s (%s) failed: %s", codec_names[codec],
			rc_modes[i], av_err2str(ret));
		avcodec_free_context(&ctx);
	}
	return false;
}

static void encode_one(struct gm_encoder *enc, struct session *s,
		struct job *job, bool keyframe) {
	AVFrame *frame = job->frame;
	// The copy was queued on the GPU by the compositor thread.
	vaSyncSurface(enc->va, (VASurfaceID)(uintptr_t)frame->data[3]);

	frame->pts = (int64_t)(job->slot_ns / 1000);
	if (keyframe) {
		frame->pict_type = AV_PICTURE_TYPE_I;
		frame->flags |= AV_FRAME_FLAG_KEY;
	}
	uint64_t start = gm_now_ns();
	int ret = avcodec_send_frame(s->ctx, frame);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "avcodec_send_frame: %s", av_err2str(ret));
		return;
	}
	while ((ret = avcodec_receive_packet(s->ctx, s->packet)) == 0) {
		uint64_t done = gm_now_ns();
		struct gm_video_packet out = {
			.data = s->packet->data,
			.len = (size_t)s->packet->size,
			.frame_id = enc->next_frame_id++,
			.slot_ns = (uint64_t)s->packet->pts * 1000,
			.encoded_ns = done,
			.width = (uint16_t)s->ctx->width,
			.height = (uint16_t)s->ctx->height,
			.keyframe = (s->packet->flags & AV_PKT_FLAG_KEY) != 0,
			.codec = s->codec,
		};
		enc->cb->video(enc->cb->ctx, &out);
		av_packet_unref(s->packet);

		pthread_mutex_lock(&enc->lock);
		enc->stats_encoded++;
		enc->stats_encode_ns += done - start;
		pthread_mutex_unlock(&enc->lock);
	}
	if (ret != AVERROR(EAGAIN)) {
		wlr_log(WLR_ERROR, "avcodec_receive_packet: %s", av_err2str(ret));
	}
}

static void *encoder_thread(void *data) {
	struct gm_encoder *enc = data;
	struct session s = { .packet = av_packet_alloc() };

	pthread_mutex_lock(&enc->lock);
	while (!enc->quit) {
		if (!enc->active && s.ctx != NULL) {
			pthread_mutex_unlock(&enc->lock);
			session_close(&s);
			pthread_mutex_lock(&enc->lock);
			continue;
		}
		if (enc->pending.frame == NULL) {
			pthread_cond_wait(&enc->cond, &enc->lock);
			continue;
		}
		struct job job = enc->pending;
		enc->pending.frame = NULL;
		enum gm_codec codec = enc->codec;
		int bitrate = enc->bitrate_kbps, refresh = enc->refresh_mhz;
		unsigned gen = enc->settings_gen;
		bool keyframe = enc->keyframe;
		enc->keyframe = false;
		pthread_mutex_unlock(&enc->lock);

		if (s.ctx != NULL && (s.gen != gen ||
				s.frames->data != job.frame->hw_frames_ctx->data)) {
			session_close(&s);
		}
		if (s.ctx == NULL) {
			// A new session starts with an IDR frame anyway.
			if (session_open(enc, &s, job.frame, codec, bitrate, refresh)) {
				s.gen = gen;
			}
		}
		if (s.ctx != NULL) {
			encode_one(enc, &s, &job, keyframe);
		}
		av_frame_free(&job.frame);
		pthread_mutex_lock(&enc->lock);
	}
	pthread_mutex_unlock(&enc->lock);

	session_close(&s);
	av_packet_free(&s.packet);
	return NULL;
}

/* ---- setup -------------------------------------------------------------- */

static bool has_encoder(VADisplay va, VAProfile profile) {
	int n = vaMaxNumEntrypoints(va);
	VAEntrypoint *eps = calloc((size_t)n, sizeof(*eps));
	bool found = false;
	if (vaQueryConfigEntrypoints(va, profile, eps, &n) == VA_STATUS_SUCCESS) {
		for (int i = 0; i < n; i++) {
			if (eps[i] == VAEntrypointEncSlice ||
					eps[i] == VAEntrypointEncSliceLP) {
				found = true;
			}
		}
	}
	free(eps);
	return found;
}

static void log_ffmpeg(void *avcl, int level, const char *fmt, va_list args) {
	if (level > AV_LOG_WARNING) {
		return;
	}
	char line[1024];
	vsnprintf(line, sizeof(line), fmt, args);
	size_t len = strlen(line);
	if (len > 0 && line[len - 1] == '\n') {
		line[len - 1] = '\0';
	}
	wlr_log(level <= AV_LOG_ERROR ? WLR_ERROR : WLR_INFO, "ffmpeg: %s", line);
}

struct gm_encoder *encoder_create(int drm_fd, const struct gm_callbacks *cb) {
	struct gm_encoder *enc = calloc(1, sizeof(*enc));
	enc->cb = cb;
	enc->vpp_config = VA_INVALID_ID;
	enc->vpp_context = VA_INVALID_ID;
	enc->upload = VA_INVALID_SURFACE;
	wl_list_init(&enc->imports);
	av_log_set_callback(log_ffmpeg);

	enc->drm_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
	enc->va = vaGetDisplayDRM(enc->drm_fd);
	int major, minor;
	if (enc->va == NULL || vaInitialize(enc->va, &major, &minor) !=
			VA_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "VA-API initialization failed");
		goto err;
	}
	wlr_log(WLR_INFO, "VA-API %d.%d: %s", major, minor, vaQueryVendorString(enc->va));

	if (vaCreateConfig(enc->va, VAProfileNone, VAEntrypointVideoProc, NULL, 0,
			&enc->vpp_config) != VA_STATUS_SUCCESS ||
			vaCreateContext(enc->va, enc->vpp_config, 0, 0, VA_PROGRESSIVE,
				NULL, 0, &enc->vpp_context) != VA_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "VA-API has no video processing");
		goto err;
	}
	if (has_encoder(enc->va, VAProfileH264High) ||
			has_encoder(enc->va, VAProfileH264Main)) {
		enc->supported |= 1u << GM_CODEC_H264;
	}
	if (has_encoder(enc->va, VAProfileHEVCMain)) {
		enc->supported |= 1u << GM_CODEC_HEVC;
	}
	if (has_encoder(enc->va, VAProfileAV1Profile0)) {
		enc->supported |= 1u << GM_CODEC_AV1;
	}

	enc->device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
	AVHWDeviceContext *dc = (AVHWDeviceContext *)enc->device->data;
	((AVVAAPIDeviceContext *)dc->hwctx)->display = enc->va;
	if (av_hwdevice_ctx_init(enc->device) < 0) {
		wlr_log(WLR_ERROR, "FFmpeg VA-API device init failed");
		goto err;
	}

	enc->pw = pyrowave_create(cb);
	if (enc->pw != NULL) {
		enc->supported |= 1u << GM_CODEC_PYROWAVE;
	}

	pthread_mutex_init(&enc->lock, NULL);
	pthread_cond_init(&enc->cond, NULL);
	pthread_create(&enc->thread, NULL, encoder_thread, enc);
	return enc;

err:
	if (enc->device != NULL) {
		av_buffer_unref(&enc->device);
	}
	if (enc->va != NULL) {
		vaTerminate(enc->va);
	}
	close(enc->drm_fd);
	free(enc);
	return NULL;
}

void encoder_destroy(struct gm_encoder *enc) {
	if (enc == NULL) {
		return;
	}
	pthread_mutex_lock(&enc->lock);
	enc->quit = true;
	pthread_cond_signal(&enc->cond);
	pthread_mutex_unlock(&enc->lock);
	pthread_join(enc->thread, NULL);
	av_frame_free(&enc->pending.frame);
	pyrowave_destroy(enc->pw);

	struct import *imp, *tmp;
	wl_list_for_each_safe(imp, tmp, &enc->imports, link) {
		import_destroy(imp);
	}
	free_frames(enc);
	vaDestroyContext(enc->va, enc->vpp_context);
	if (enc->upload != VA_INVALID_SURFACE) {
		vaDestroySurfaces(enc->va, &enc->upload, 1);
	}
	vaDestroyConfig(enc->va, enc->vpp_config);
	// A device context given a display does not terminate it.
	av_buffer_unref(&enc->device);
	vaTerminate(enc->va);
	close(enc->drm_fd);
	free(enc);
}

uint32_t encoder_supported_codecs(struct gm_encoder *enc) {
	return enc->supported;
}

void encoder_start(struct gm_encoder *enc, enum gm_codec codec,
		int bitrate_kbps, int refresh_mhz) {
	enc->pw_active = codec == GM_CODEC_PYROWAVE && enc->pw != NULL;
	if (enc->pw_active) {
		pyrowave_start(enc->pw, bitrate_kbps, refresh_mhz);
		pthread_mutex_lock(&enc->lock);
		enc->active = false; // the VA-API worker closes its session
		av_frame_free(&enc->pending.frame);
		pthread_cond_signal(&enc->cond);
		pthread_mutex_unlock(&enc->lock);
		return;
	}
	if (enc->pw != NULL) {
		pyrowave_stop(enc->pw);
	}
	pthread_mutex_lock(&enc->lock);
	enc->active = true;
	enc->codec = codec;
	enc->bitrate_kbps = bitrate_kbps;
	enc->refresh_mhz = refresh_mhz;
	enc->settings_gen++;
	pthread_mutex_unlock(&enc->lock);
}

void encoder_stop(struct gm_encoder *enc) {
	if (enc->pw != NULL) {
		pyrowave_stop(enc->pw);
	}
	enc->pw_active = false;
	pthread_mutex_lock(&enc->lock);
	enc->active = false;
	av_frame_free(&enc->pending.frame);
	pthread_cond_signal(&enc->cond); // the worker closes its session
	pthread_mutex_unlock(&enc->lock);
}

void encoder_set_bitrate(struct gm_encoder *enc, int bitrate_kbps) {
	if (enc->pw != NULL) {
		pyrowave_set_rate(enc->pw, bitrate_kbps, 0);
	}
	pthread_mutex_lock(&enc->lock);
	if (enc->bitrate_kbps != bitrate_kbps) {
		enc->bitrate_kbps = bitrate_kbps;
		enc->settings_gen++;
	}
	pthread_mutex_unlock(&enc->lock);
}

void encoder_set_refresh(struct gm_encoder *enc, int refresh_mhz) {
	if (enc->pw != NULL) {
		pyrowave_set_rate(enc->pw, 0, refresh_mhz);
	}
	pthread_mutex_lock(&enc->lock);
	if (enc->refresh_mhz != refresh_mhz) {
		enc->refresh_mhz = refresh_mhz;
		enc->settings_gen++;
	}
	pthread_mutex_unlock(&enc->lock);
}

void encoder_request_keyframe(struct gm_encoder *enc) {
	pthread_mutex_lock(&enc->lock);
	enc->keyframe = true;
	pthread_mutex_unlock(&enc->lock);
}

void encoder_take_stats(struct gm_encoder *enc, unsigned *encoded,
		unsigned *dropped, double *avg_encode_ms) {
	pthread_mutex_lock(&enc->lock);
	*encoded = enc->stats_encoded;
	*dropped = enc->stats_dropped;
	if (enc->pw != NULL) {
		unsigned pe, pd;
		uint64_t pns;
		pyrowave_take_stats(enc->pw, &pe, &pd, &pns);
		enc->stats_encoded += pe;
		enc->stats_dropped += pd;
		enc->stats_encode_ns += pns;
		*encoded = enc->stats_encoded;
		*dropped = enc->stats_dropped;
	}
	*avg_encode_ms = enc->stats_encoded > 0 ?
		(double)enc->stats_encode_ns / enc->stats_encoded / 1e6 : 0;
	enc->stats_encoded = enc->stats_dropped = 0;
	enc->stats_encode_ns = 0;
	pthread_mutex_unlock(&enc->lock);
}
