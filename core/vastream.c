// Streaming a frame's AV1 bitstream while the GPU is still writing it
// (DESIGN.md §12).
//
// VA-API hands back a frame's bitstream only when the whole frame is done:
// one fence and one feedback buffer per picture, and mapping the coded buffer
// waits for the GPU. But Mesa allocates that buffer's BO once and reuses it,
// and VCN fills it in order, so a mapping taken while the buffer was idle
// stays a live view of the bitstream as it appears. We keep one coded buffer
// for the encoder's lifetime, hold its mapping, and send each piece as it is
// written. The bytes on the wire are the encoder's own: the same single
// encode call, byte for byte.
//
// Two things make that exact. The buffer is pre-filled with a position-keyed
// pattern, so where the pattern resumes is where the GPU has not written yet,
// and nothing unwritten is ever sent. And VCN does come back to patch a few
// bytes it has already written: about nine in the frame header, and the three
// bytes of each tile group's OBU header and size, once that tile group's
// length is known. So we keep a copy of what we sent, compare it with the
// bitstream FFmpeg hands back, and send the handful of bytes that changed in
// the last chunks. The client applies chunks in order, so those win.
//
// `tiles`/`tile_groups` on the encoder is what makes the pieces worth having:
// each tile group is its own OBU, so the stream has real cut points rather
// than one opaque frame (and, later, a decoder could start on a tile).
//
// FFmpeg's and libva's calls are redirected at link time (build.rs passes
// --wrap), so neither library is patched. All of this runs on the encoder
// thread, inside the wait FFmpeg already does.
//
// It rests on one picture being in flight at a time (`async_depth` 1 in
// encode.c): one coded buffer is kept and handed back for every picture, so a
// second picture encoding at the same time would write over the first.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <va/va.h>
#include <wlr/util/log.h>

#include "core.h"

// Never stream the first bytes of a frame: the frame header is patched after
// the fact, and this is far more than it needs (a temporal delimiter, a
// sequence header on keyframes, a frame header and the first tile group's
// three bytes).
#define HOLD_PREFIX 128
// How long to wait on the GPU between two looks at the buffer.
#define POLL_NS 200000
// What to keep patterned, as a multiple of the largest recent frame: enough
// that a frame fits, little enough that laying it down is cheap. A keyframe
// larger than this streams only its first part early, which costs nothing but
// the overlap on that one frame.
#define PREFILL_SLACK 2
#define PREFILL_MIN (1u << 18)
#define PREFILL_MAX (2u << 20)

VAStatus __real_vaCreateBuffer(VADisplay dpy, VAContextID context,
	VABufferType type, unsigned int size, unsigned int num_elements,
	void *data, VABufferID *buf_id);
VAStatus __real_vaDestroyBuffer(VADisplay dpy, VABufferID buf_id);
VAStatus __real_vaMapBuffer(VADisplay dpy, VABufferID buf_id, void **pbuf);
VAStatus __real_vaEndPicture(VADisplay dpy, VAContextID context);
VAStatus __real_vaSyncBuffer(VADisplay dpy, VABufferID buf_id, uint64_t timeout_ns);

static struct {
	const struct gm_callbacks *cb;
	bool on; // the client takes chunks
	unsigned chunk; // most payload bytes per chunk

	// The one coded buffer, and the mapping we took while it was idle.
	VADisplay dpy;
	VABufferID coded;
	unsigned coded_size;
	volatile uint8_t *map;

	uint8_t *shadow; // what we sent, as we first saw it
	unsigned shadow_size;
	unsigned prefill; // how much of the buffer carries the pattern
	unsigned largest; // the largest frame so far

	// The frame being encoded.
	bool streaming; // between vastream_begin and vastream_end
	bool submitted; // and the GPU has been given it
	uint32_t frame_id;
	uint64_t slot_ns;
	uint64_t submit_ns;
	uint64_t first_ns; // when the first chunk went out
	uint16_t width, height;
	bool keyframe;
	enum gm_codec codec;
	unsigned sent; // bytes of the frame streamed so far
	unsigned index; // next chunk index

	bool debug;
	unsigned prefill_override;
	unsigned stats_frames, stats_chunks, stats_patched;
	uint64_t stats_first_ns, stats_last_ns;
} st = { .coded = VA_INVALID_ID };

// FNV-1a, so a debug run can check that what a client puts back together is
// the bitstream the encoder produced.
static uint64_t hash64(const uint8_t *data, size_t len) {
	uint64_t h = 0xcbf29ce484222325ull;
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
static unsigned frontier(unsigned from, unsigned to) {
	const volatile uint8_t *p = st.map;
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

static void lay_pattern(void) {
	if (st.map == NULL) {
		return;
	}
	unsigned want = st.largest * PREFILL_SLACK;
	if (want < PREFILL_MIN) {
		want = PREFILL_MIN;
	}
	// Testing: a small pattern leaves most of each frame to be sent after the
	// encode, which is also what an unusually large keyframe does.
	if (st.prefill_override > 0) {
		want = st.prefill_override;
	}
	if (want > PREFILL_MAX) {
		want = PREFILL_MAX;
	}
	if (want > st.coded_size) {
		want = st.coded_size;
	}
	volatile uint8_t *p = st.map;
	for (unsigned i = 0; i < want; i++) {
		p[i] = pattern(i);
	}
	st.prefill = want;
}

/* ---- sending ------------------------------------------------------------ */

// Sends [offset, offset + len) of the frame, split so each piece fits a
// datagram. `last` marks the frame's final piece, which is the only one that
// knows how many pieces and bytes the frame has.
static void emit(unsigned offset, unsigned len, bool last, unsigned total,
		uint64_t encoded_ns) {
	if (st.cb->video_chunk == NULL || len == 0) {
		return;
	}
	while (len > 0) {
		unsigned n = len < st.chunk ? len : st.chunk;
		bool final = last && n == len;
		struct gm_video_chunk c = {
			.data = st.shadow + offset,
			.len = n,
			.frame_id = st.frame_id,
			.offset = offset,
			.index = (uint16_t)st.index++,
			.count = final ? (uint16_t)st.index : 0,
			.total = final ? total : 0,
			.slot_ns = st.slot_ns,
			.encoded_ns = final ? encoded_ns : 0,
			.first_ns = final ? st.first_ns : 0,
			.width = st.width,
			.height = st.height,
			// Only the frame's last piece says whether it is a keyframe:
			// until the encoder hands the frame back, all we know is what was
			// asked for, not what it produced.
			.keyframe = final && st.keyframe,
			.last = final,
			.codec = st.codec,
		};
		if (st.first_ns == 0) {
			st.first_ns = gm_now_ns();
		}
		st.stats_chunks++;
		st.cb->video_chunk(st.cb->ctx, &c);
		offset += n;
		len -= n;
	}
}

// Streams whole chunks of everything the GPU has written since last time.
static void push(void) {
	if (st.map == NULL || st.shadow == NULL) {
		return;
	}
	unsigned from = st.sent > HOLD_PREFIX ? st.sent : HOLD_PREFIX;
	if (from + st.chunk > st.prefill) {
		return;
	}
	unsigned f = frontier(from, st.prefill);
	while (from + st.chunk <= f) {
		for (unsigned i = from; i < from + st.chunk; i++) {
			st.shadow[i] = st.map[i];
		}
		emit(from, st.chunk, false, 0, 0);
		from += st.chunk;
	}
	st.sent = from;
}

/* ---- the wrappers ------------------------------------------------------- */

// One coded buffer for the encoder's lifetime, so its BO and our mapping of
// it outlive each frame. A bigger one replaces it between frames.
VAStatus __wrap_vaCreateBuffer(VADisplay dpy, VAContextID context,
		VABufferType type, unsigned int size, unsigned int num_elements,
		void *data, VABufferID *buf_id) {
	if (type != VAEncCodedBufferType || !st.on) {
		return __real_vaCreateBuffer(dpy, context, type, size, num_elements,
			data, buf_id);
	}
	unsigned want = size * (num_elements > 0 ? num_elements : 1);
	if (st.coded != VA_INVALID_ID && st.coded_size < want) {
		__real_vaDestroyBuffer(st.dpy, st.coded);
		st.coded = VA_INVALID_ID;
		st.map = NULL;
	}
	if (st.coded == VA_INVALID_ID) {
		VAStatus status = __real_vaCreateBuffer(dpy, context, type, size,
			num_elements, data, &st.coded);
		if (status != VA_STATUS_SUCCESS) {
			return status;
		}
		st.dpy = dpy;
		st.coded_size = want;
		st.map = NULL;
		free(st.shadow);
		st.shadow = malloc(want);
		st.shadow_size = st.shadow != NULL ? want : 0;
	}
	*buf_id = st.coded;
	return VA_STATUS_SUCCESS;
}

VAStatus __wrap_vaDestroyBuffer(VADisplay dpy, VABufferID buf_id) {
	if (buf_id == st.coded) {
		return VA_STATUS_SUCCESS; // ours; it outlives the encoder's pictures
	}
	return __real_vaDestroyBuffer(dpy, buf_id);
}

// The first map of the coded buffer gives us its address. It is the BO's own
// mapping, which Mesa keeps, so it stays a live view of the bitstream.
VAStatus __wrap_vaMapBuffer(VADisplay dpy, VABufferID buf_id, void **pbuf) {
	VAStatus status = __real_vaMapBuffer(dpy, buf_id, pbuf);
	if (status != VA_STATUS_SUCCESS || buf_id != st.coded || !st.on) {
		return status;
	}
	VACodedBufferSegment *first = *pbuf;
	uint8_t *base = NULL;
	for (VACodedBufferSegment *s = first; s != NULL; s = s->next) {
		if (base == NULL || (uint8_t *)s->buf < base) {
			base = s->buf;
		}
	}
	if (base == NULL) {
		return status;
	}
	if (st.map == NULL) {
		st.map = base;
		wlr_log(WLR_INFO, "Streaming the bitstream as it is written "
			"(%u-byte buffer, %u-byte chunks)", st.coded_size, st.chunk);
	} else if ((volatile uint8_t *)base != st.map) {
		// Mesa would have to have remapped the BO; stop trusting the old view.
		wlr_log(WLR_ERROR, "Coded buffer moved; not streaming this frame");
		st.map = base;
	}
	return status;
}

VAStatus __wrap_vaEndPicture(VADisplay dpy, VAContextID context) {
	uint64_t at = gm_now_ns();
	VAStatus status = __real_vaEndPicture(dpy, context);
	if (st.streaming && status == VA_STATUS_SUCCESS) {
		st.submitted = true;
		st.submit_ns = at;
	}
	return status;
}

// FFmpeg waits here for the frame. We wait in short steps and send what the
// GPU has written between them.
VAStatus __wrap_vaSyncBuffer(VADisplay dpy, VABufferID buf_id,
		uint64_t timeout_ns) {
	if (!st.streaming || !st.submitted || buf_id != st.coded) {
		return __real_vaSyncBuffer(dpy, buf_id, timeout_ns);
	}
	for (;;) {
		push();
		VAStatus status = __real_vaSyncBuffer(dpy, buf_id, POLL_NS);
		if (status != VA_STATUS_ERROR_TIMEDOUT) {
			push();
			return status;
		}
	}
}

/* ---- what the encoder calls -------------------------------------------- */

void vastream_configure(const struct gm_callbacks *cb, bool on,
		unsigned chunk_bytes) {
	st.cb = cb;
	st.on = on && cb != NULL && cb->video_chunk != NULL;
	st.chunk = chunk_bytes > 0 ? chunk_bytes : 1100;
	st.debug = getenv("GM_DEBUG_CHUNKS") != NULL;
	const char *prefill = getenv("GM_CHUNK_PREFILL");
	st.prefill_override = prefill != NULL ? (unsigned)strtoul(prefill, NULL, 10) : 0;
	if (!st.on) {
		st.streaming = false;
	}
}

bool vastream_enabled(void) {
	return st.on;
}

void vastream_begin(uint32_t frame_id, uint64_t slot_ns, uint16_t width,
		uint16_t height, enum gm_codec codec) {
	if (!st.on) {
		return;
	}
	st.streaming = true;
	st.submitted = false;
	st.frame_id = frame_id;
	st.slot_ns = slot_ns;
	st.width = width;
	st.height = height;
	st.keyframe = false;
	st.codec = codec;
	st.sent = 0;
	st.index = 0;
	st.first_ns = 0;
}

// The encoder's own bitstream, once FFmpeg has it. Everything already sent
// that the GPU went back and changed goes out again, then the bytes we held
// back, so the client ends with exactly these bytes. Returns false if the
// frame was not streamed and must be sent whole.
bool vastream_end(const uint8_t *data, size_t len, uint64_t encoded_ns,
		bool keyframe) {
	if (!st.on || !st.streaming) {
		return false;
	}
	st.streaming = false;
	st.keyframe = keyframe; // what the encoder produced, not what was asked
	// The largest frame lately: it follows a keyframe up at once and comes
	// back down over a couple of seconds, so the pattern covers what the next
	// frame needs without laying down a keyframe's worth every time.
	st.largest = len > st.largest ? (unsigned)len : st.largest - st.largest / 64;
	if (!st.submitted || st.index == 0 || st.shadow == NULL || len == 0 ||
			len > st.shadow_size) {
		// Nothing went out, or the frame does not fit our copy: it is sent
		// whole, and the buffer is readied for the next one.
		st.index = 0;
		lay_pattern();
		return false;
	}

	unsigned total = (unsigned)len;
	unsigned sent = st.sent < total ? st.sent : total;
	unsigned patched = 0;
	// The bytes the GPU changed after we had sent them. They come in runs of
	// three at each tile group, so send each run as it is found.
	unsigned i = HOLD_PREFIX;
	while (i < sent) {
		if (st.shadow[i] == data[i]) {
			i++;
			continue;
		}
		unsigned start = i;
		while (i < sent && st.shadow[i] != data[i]) {
			i++;
		}
		memcpy(st.shadow + start, data + start, i - start);
		emit(start, i - start, false, 0, 0);
		patched += i - start;
	}
	// What the GPU wrote after our last whole chunk, and then the held-back
	// start of the frame, which is also the frame's last chunk.
	if (sent < total) {
		memcpy(st.shadow + sent, data + sent, total - sent);
		emit(sent, total - sent, false, 0, 0);
	}
	unsigned prefix = HOLD_PREFIX < total ? HOLD_PREFIX : total;
	memcpy(st.shadow, data, prefix);
	emit(0, prefix, true, total, encoded_ns);

	st.stats_frames++;
	st.stats_patched += patched;
	st.stats_first_ns += st.first_ns > st.submit_ns ? st.first_ns - st.submit_ns : 0;
	st.stats_last_ns += encoded_ns > st.submit_ns ? encoded_ns - st.submit_ns : 0;
	if (st.debug) {
		fprintf(stderr, "chunks frame %u %u bytes in %u chunks, first %.2f ms, "
			"bitstream done %.2f ms, %u bytes patched, %u sent early, hash %016llx\n",
			st.frame_id, total, st.index,
			(st.first_ns - st.submit_ns) / 1e6,
			(encoded_ns - st.submit_ns) / 1e6, patched, sent,
			(unsigned long long)hash64(data, len));
	}
	// Ready the buffer for the next frame while nothing is using it.
	lay_pattern();
	return true;
}

void vastream_take_stats(unsigned *frames, unsigned *chunks, unsigned *patched,
		double *first_ms, double *last_ms) {
	*frames = st.stats_frames;
	*chunks = st.stats_chunks;
	*patched = st.stats_patched;
	*first_ms = st.stats_frames > 0 ? (double)st.stats_first_ns / st.stats_frames / 1e6 : 0;
	*last_ms = st.stats_frames > 0 ? (double)st.stats_last_ns / st.stats_frames / 1e6 : 0;
	st.stats_frames = st.stats_chunks = st.stats_patched = 0;
	st.stats_first_ns = st.stats_last_ns = 0;
}
