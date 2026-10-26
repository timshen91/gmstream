// Audio: capture the default sink's monitor through PipeWire and encode it as
// 48 kHz stereo Opus in 5 ms frames. Runs on PipeWire's own thread loop and
// only while a client is streaming.

#include <stdlib.h>
#include <string.h>

#include <opus.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <wlr/util/log.h>

#include "core.h"

#define RATE 48000
#define CHANNELS 2
#define FRAME_SAMPLES (RATE / 200) // 5 ms

struct gm_audio {
	const struct gm_callbacks *cb;
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	OpusEncoder *opus;

	float pcm[FRAME_SAMPLES * CHANNELS];
	int pcm_samples;
	uint64_t pcm_start_ns; // capture time of pcm[0]
	uint8_t packet[1500];
};

static void on_process(void *data) {
	struct gm_audio *audio = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(audio->stream);
	if (b == NULL) {
		return;
	}
	struct spa_data *d = &b->buffer->datas[0];
	if (d->data == NULL || d->chunk == NULL) {
		pw_stream_queue_buffer(audio->stream, b);
		return;
	}
	const float *in = SPA_PTROFF(d->data, d->chunk->offset, const float);
	int samples = (int)(d->chunk->size / (sizeof(float) * CHANNELS));
	uint64_t now = gm_now_ns();
	// The newest sample in this buffer was captured about now.
	uint64_t first_ns = now - (uint64_t)samples * 1000000000ull / RATE;

	for (int i = 0; i < samples;) {
		if (audio->pcm_samples == 0) {
			audio->pcm_start_ns = first_ns +
				(uint64_t)i * 1000000000ull / RATE;
		}
		int n = FRAME_SAMPLES - audio->pcm_samples;
		if (n > samples - i) {
			n = samples - i;
		}
		memcpy(&audio->pcm[audio->pcm_samples * CHANNELS], &in[i * CHANNELS],
			(size_t)n * CHANNELS * sizeof(float));
		audio->pcm_samples += n;
		i += n;
		if (audio->pcm_samples == FRAME_SAMPLES) {
			int len = opus_encode_float(audio->opus, audio->pcm, FRAME_SAMPLES,
				audio->packet, sizeof(audio->packet));
			if (len > 0) {
				audio->cb->audio(audio->cb->ctx, audio->packet, (size_t)len,
					audio->pcm_start_ns);
			}
			audio->pcm_samples = 0;
		}
	}
	pw_stream_queue_buffer(audio->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

struct gm_audio *audio_create(const struct gm_callbacks *cb) {
	pw_init(NULL, NULL);
	struct gm_audio *audio = calloc(1, sizeof(*audio));
	audio->cb = cb;
	int err;
	audio->opus = opus_encoder_create(RATE, CHANNELS,
		OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
	if (audio->opus == NULL) {
		wlr_log(WLR_ERROR, "opus_encoder_create: %s", opus_strerror(err));
		free(audio);
		return NULL;
	}
	opus_encoder_ctl(audio->opus, OPUS_SET_BITRATE(128000));
	audio->loop = pw_thread_loop_new("gm-audio", NULL);
	if (audio->loop == NULL || pw_thread_loop_start(audio->loop) < 0) {
		wlr_log(WLR_ERROR, "Starting the PipeWire loop failed");
		if (audio->loop != NULL) {
			pw_thread_loop_destroy(audio->loop);
		}
		opus_encoder_destroy(audio->opus);
		free(audio);
		return NULL;
	}
	return audio;
}

void audio_start(struct gm_audio *audio) {
	if (audio == NULL) {
		return;
	}
	pw_thread_loop_lock(audio->loop);
	if (audio->stream == NULL) {
		struct pw_properties *props = pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Game",
			PW_KEY_STREAM_CAPTURE_SINK, "true",
			PW_KEY_NODE_NAME, "gmstream",
			NULL);
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
			FRAME_SAMPLES, RATE);
		audio->stream = pw_stream_new_simple(
			pw_thread_loop_get_loop(audio->loop), "gmstream", props,
			&stream_events, audio);

		uint8_t buf[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		const struct spa_pod *params[1];
		params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&SPA_AUDIO_INFO_RAW_INIT(
				.format = SPA_AUDIO_FORMAT_F32,
				.rate = RATE,
				.channels = CHANNELS,
				.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }));
		int ret = pw_stream_connect(audio->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS, params, 1);
		if (ret < 0) {
			wlr_log(WLR_ERROR, "pw_stream_connect: %s", spa_strerror(ret));
			pw_stream_destroy(audio->stream);
			audio->stream = NULL;
		}
		audio->pcm_samples = 0;
		opus_encoder_ctl(audio->opus, OPUS_RESET_STATE);
	}
	pw_thread_loop_unlock(audio->loop);
}

void audio_stop(struct gm_audio *audio) {
	if (audio == NULL) {
		return;
	}
	pw_thread_loop_lock(audio->loop);
	if (audio->stream != NULL) {
		pw_stream_destroy(audio->stream);
		audio->stream = NULL;
	}
	pw_thread_loop_unlock(audio->loop);
}

void audio_destroy(struct gm_audio *audio) {
	if (audio == NULL) {
		return;
	}
	audio_stop(audio);
	pw_thread_loop_stop(audio->loop);
	pw_thread_loop_destroy(audio->loop);
	opus_encoder_destroy(audio->opus);
	free(audio);
}
