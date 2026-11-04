// Rate-distortion and speed harness for the Metal Riptide encoder and
// decoder: encodes frames of a y4m clip at fixed sizes, decodes them on the
// GPU, and reports bytes and PSNR per size, plus GPU pass times.
//
//   rtrd clip.y4m -n FRAMES [-skip K] -s SIZE[,SIZE...]   (sizes in bytes per frame)
//
// Output, one line per size: size bytes psnr_y psnr_u psnr_v psnr_w.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "../metal/riptide_metal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <dispatch/dispatch.h>

static int y4m_header(FILE *f, int *w, int *h) {
	char line[512];
	if (!fgets(line, sizeof line, f) || strncmp(line, "YUV4MPEG2", 9)) return -1;
	for (char *t = strtok(line + 9, " \n"); t; t = strtok(NULL, " \n")) {
		if (t[0] == 'W') *w = atoi(t + 1);
		if (t[0] == 'H') *h = atoi(t + 1);
	}
	return 0;
}

static double psnr(double mse) { return mse <= 0 ? 100.0 : 10 * log10(255.0 * 255.0 / mse); }


// PSNR-HVS-M with the CSF for a viewing distance of H screen heights, as
// Riptide's own psnr.cpp and shaders/psnr_hvs_m.comp compute it (luma only,
// 8x8 DCT blocks every 4 pixels). Returns the total weighted square error;
// pixels are in [0, 1].
static float csf_fn(float cpd) { return 2.6f * (0.0192f + 0.114f * cpd) * expf(-powf(0.114f * cpd, 1.1f)); }

struct HvsTables {
	float csf[8][8], mask[8][8], inv_mask[8][8];
	HvsTables(int height, float H) {
		float nyq = height * H * (float)M_PI / 360.0f, mx = 0;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				float a = (x + 0.5f) / 8, b = (y + 0.5f) / 8;
				csf[y][x] = 2.6f * csf_fn(sqrtf(a * a + b * b) * nyq);
				mx = fmaxf(mx, csf[y][x]);
			}
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				mask[y][x] = csf[y][x] * csf[y][x] / (mx * mx);
				inv_mask[y][x] = 0;
			}
		mask[0][0] = 0;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				if (x || y) inv_mask[y][x] = 1.0f / mask[y][x];
	}
};

static void dct8(float *o, const float *in) {
	// Orthonormal DCT-II, direct form; the same cosines and order as computing
	// them inline, from a table.
	static const struct Table {
		double c[8][8];
		Table() {
			for (int k = 0; k < 8; k++)
				for (int n = 0; n < 8; n++) c[k][n] = cos(M_PI * (n + 0.5) * k / 8);
		}
	} T;
	for (int k = 0; k < 8; k++) {
		double s = 0;
		for (int n = 0; n < 8; n++) s += in[n] * T.c[k][n];
		o[k] = (float)(s * (k ? sqrt(2.0 / 8) : sqrt(1.0 / 8)));
	}
}

static void dct8x8(float d[8][8], const float s[8][8]) {
	float t[8][8], col[8], oc[8];
	for (int r = 0; r < 8; r++) dct8(t[r], s[r]);
	for (int c = 0; c < 8; c++) {
		for (int i = 0; i < 8; i++) col[i] = t[i][c];
		dct8(oc, col);
		for (int i = 0; i < 8; i++) d[i][c] = oc[i];
	}
}

static float vari(float e, float e2, int n) { return fmaxf(e2 * n - e * e, 0) / (n - 1); }

static float maskeff(const HvsTables &T, const float s[8][8], const float d[8][8]) {
	float m = 0, se[2][2] = {{0}}, se2[2][2] = {{0}}, te = 0, te2 = 0;
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			m += d[y][x] * d[y][x] * T.mask[y][x];
			se[y / 4][x / 4] += s[y][x], se2[y / 4][x / 4] += s[y][x] * s[y][x];
		}
	for (int i = 0; i < 4; i++) te += se[i / 2][i % 2], te2 += se2[i / 2][i % 2];
	float pop = vari(te, te2, 64);
	if (pop != 0)
		pop = (vari(se[0][0], se2[0][0], 16) + vari(se[1][0], se2[1][0], 16) + vari(se[0][1], se2[0][1], 16) +
		       vari(se[1][1], se2[1][1], 16)) / pop;
	return sqrtf(m * pop) / 32;
}

// Returns PSNR-HVS-M-H in dB for one luma plane pair.
static double psnr_hvs(const HvsTables &T, const uint8_t *a, const uint8_t *b, int w, int h) {
	int nbx = (w - 8) / 4 + 1, nby = (h - 8) / 4 + 1;
	std::vector<double> rowv(nby);
	double *rows = rowv.data();
	dispatch_apply(nby, DISPATCH_APPLY_AUTO, ^(size_t by) {
	  double acc = 0;
	  for (int bx = 0; bx < nbx; bx++) {
		  float sa[8][8], sb[8][8], da[8][8], db[8][8];
		  for (int y = 0; y < 8; y++)
			  for (int x = 0; x < 8; x++) {
				  size_t i = (by * 4 + y) * (size_t)w + bx * 4 + x;
				  sa[y][x] = a[i] / 255.0f, sb[y][x] = b[i] / 255.0f;
			  }
		  dct8x8(da, sa), dct8x8(db, sb);
		  float mk = fmaxf(maskeff(T, sa, da), maskeff(T, sb, db)), e = 0;
		  for (int y = 0; y < 8; y++)
			  for (int x = 0; x < 8; x++) {
				  float u = fmaxf(fabsf(da[y][x] - db[y][x]) - mk * T.inv_mask[y][x], 0) * T.csf[y][x];
				  e += u * u;
			  }
		  acc += e;
	  }
	  rows[by] = acc;
	});
	double tot = 0;
	for (double r : rowv) tot += r;
	return 10 * log10((double)nbx * nby * 64 / tot);
}

static void stats_cb(void *ud, const char *msg) { ((std::string *)ud)->append(msg).append("\n"); }

id<MTLTexture> riptide_encoder_debug_prediction(riptide_encoder encoder, int component);

int main(int argc, char **argv) {
	@autoreleasepool {
		const char *path = NULL, *dump = NULL;
		int nframes = 10, skip = 1, bench = 0, predict = 0, warm = 0;
		std::vector<size_t> sizes;
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "-n") && i + 1 < argc) nframes = atoi(argv[++i]);
			else if (!strcmp(argv[i], "-skip") && i + 1 < argc) skip = atoi(argv[++i]);
			else if (!strcmp(argv[i], "-dump") && i + 1 < argc) dump = argv[++i];
			else if (!strcmp(argv[i], "-bench") && i + 1 < argc) bench = atoi(argv[++i]);
			else if (!strcmp(argv[i], "-p")) predict = 1; // inter: each frame from the previous decoded one
			else if (!strcmp(argv[i], "-warm") && i + 1 < argc) warm = atoi(argv[++i]); // left out of the averages
			else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
				for (char *t = strtok(argv[++i], ","); t; t = strtok(NULL, ",")) sizes.push_back(strtoull(t, NULL, 10));
			} else path = argv[i];
		}
		FILE *f = path ? fopen(path, "rb") : NULL;
		int w = 0, h = 0;
		if (!f || y4m_header(f, &w, &h)) {
			fprintf(stderr, "cannot open %s\n", path ? path : "(none)");
			return 1;
		}
		size_t ysz = (size_t)w * h, csz = ysz / 4;
		std::vector<std::vector<uint8_t>> frames;
		std::vector<uint8_t> buf(ysz + 2 * csz);
		char line[64];
		for (int k = 0; (int)frames.size() < nframes; k++) {
			if (!fgets(line, sizeof line, f) || fread(buf.data(), 1, buf.size(), f) != buf.size()) break;
			if (k % skip == 0) frames.push_back(buf);
		}
		fclose(f);

		id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
		riptide_device dev;
		riptide_device_create_info di = {(__bridge void *)mtl, NULL, NULL};
		if (riptide_device_create(&di, &dev) != RIPTIDE_SUCCESS) return 1;
		riptide_encoder enc;
		riptide_encoder_create_info ei = {dev, w, h, RIPTIDE_CHROMA_SUBSAMPLING_420};
		if (riptide_encoder_create(&ei, &enc) != RIPTIDE_SUCCESS) return 1;
		riptide_decoder dec;
		riptide_decoder_create_info dci = {dev, w, h, RIPTIDE_CHROMA_SUBSAMPLING_420};
		if (riptide_decoder_create(&dci, &dec) != RIPTIDE_SUCCESS) return 1;
		// Loss test, RT_LOSS=frame:start:fraction: a second decoder, the client, misses
		// a contiguous run of that frame's packets (as a Wi-Fi burst drops them) and
		// carries on from its own decoded frames, while the encoder's reference stays
		// the clean decode. Each frame's client PSNR-Y goes to stderr ("loss" lines).
		int loss_frame = -1;
		double loss_start = 0, loss_fraction = 0; // of the frame's packets
		// The optional fourth field, report: the client tells the encoder which packets
		// it lost, and the report is in by the time frame loss_frame + report is
		// encoded. The encoder then replays the client's decode (it has the packets and
		// knows which were missing), so from that frame on it predicts from the client's
		// actual state. 0: never reported.
		int loss_report = 0;
		if (const char *e = getenv("RT_LOSS")) sscanf(e, "%d:%lf:%lf:%d", &loss_frame, &loss_start, &loss_fraction, &loss_report);
		riptide_decoder cdec = NULL;
		// RT_CLIENT_PRECISION=p: the client decodes on a device of another transform
		// precision (0 fp16, 1 fp16 storage, 2 fp32), standing in for a GPU whose
		// float arithmetic differs from the encoder's; with RT_LOSS=0:0:0 the loss
		// lines then show the drift that alone causes.
		riptide_decoder_create_info cdci = dci;
		if (const char *cp = getenv("RT_CLIENT_PRECISION")) {
			const char *was = getenv("RIPTIDE_PRECISION");
			std::string keep = was ? was : "";
			setenv("RIPTIDE_PRECISION", cp, 1);
			riptide_device cdev;
			if (riptide_device_create(&di, &cdev) != RIPTIDE_SUCCESS) return 1;
			if (was) setenv("RIPTIDE_PRECISION", keep.c_str(), 1); else unsetenv("RIPTIDE_PRECISION");
			cdci.device = cdev;
		}
		if (loss_frame >= 0 && riptide_decoder_create(&cdci, &cdec) != RIPTIDE_SUCCESS) return 1;
		// RT_CHECKPRED=1: a decoder given each inter frame's reference, map and vectors
		// but no coefficients outputs exactly its own prediction; it must match, byte
		// for byte, the plane the encoder subtracted.
		riptide_decoder pdec = NULL;
		if (getenv("RT_CHECKPRED") && riptide_decoder_create(&dci, &pdec) != RIPTIDE_SUCCESS) return 1;
		size_t pred_mismatch = 0, pred_checked = 0;
		// Decoded frames, two sets: with -p the last one is the next one's reference.
		id<MTLTexture> texs[2][3], ctexs[2][3], ptexs[3];
		id<MTLBuffer> crb[3], prb[2][3];
		int ti = 0; // the set being decoded into
		id<MTLBuffer> rb[3];
		for (int p = 0; p < 3; p++) {
			int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:pw height:ph mipmapped:NO];
			td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
			td.storageMode = MTLStorageModePrivate;
			texs[0][p] = [mtl newTextureWithDescriptor:td];
			texs[1][p] = [mtl newTextureWithDescriptor:td];
			ctexs[0][p] = [mtl newTextureWithDescriptor:td];
			ctexs[1][p] = [mtl newTextureWithDescriptor:td];
			crb[p] = [mtl newBufferWithLength:(size_t)pw * ph options:MTLResourceStorageModeShared];
			ptexs[p] = [mtl newTextureWithDescriptor:td];
			prb[0][p] = [mtl newBufferWithLength:(size_t)pw * ph options:MTLResourceStorageModeShared];
			prb[1][p] = [mtl newBufferWithLength:(size_t)pw * ph options:MTLResourceStorageModeShared];
			rb[p] = [mtl newBufferWithLength:(size_t)pw * ph options:MTLResourceStorageModeShared];
		}
		id<MTLCommandQueue> q = [mtl newCommandQueue];
		std::vector<uint8_t> bits(8 << 20);
		std::vector<riptide_packet> packets(1 << 16);
		std::string perf;
		std::vector<uint8_t> map(size_t((w + 31) / 32) * ((h + 31) / 32));
		const int vw = (w + 15) / 16, vh = (h + 15) / 16;
		std::vector<int16_t> mv(size_t(vw) * vh * 2), mv_prev(mv.size()), mv_state(mv.size());
		// RT_MVSTATS: what other parallel-decodable vector codes would cost on the same
		// vectors, per 4x4 tile and component (bits): A the current one (width +
		// zigzag delta from last frame), B the tile's mean delta coded first then
		// each delta minus it, C the tile's mean vector (coded against last frame's
		// tile mean) then each vector minus it, D the cheapest of the three per tile
		// plus 2 bits to say which.
		double mvs[4] = {0}, mvn[4] = {0}; // and counts: vectors, changed, nonzero, changed under an intra block
		auto width_of = [](const std::vector<int> &x) {
			uint32_t mx = 0;
			for (int d : x) mx = std::max(mx, (uint32_t)(d >= 0 ? 2 * d : -2 * d - 1));
			int b = 0;
			while (mx >> b) b++;
			return b;
		};
		auto mv_stats = [&]() {
			const int mw = (w + 31) / 32;
			for (int y = 0; y < vh; y++)
				for (int x = 0; x < vw; x++) {
					const int16_t *a = &mv[2 * (y * vw + x)], *b = &mv_prev[2 * (y * vw + x)];
					bool changed = a[0] != b[0] || a[1] != b[1];
					mvn[0]++, mvn[1] += changed, mvn[2] += a[0] || a[1];
					mvn[3] += changed && map[(y / 2) * mw + x / 2] == 0;
				}
			for (int ty = 0; ty < vh; ty += 4)
				for (int tx = 0; tx < vw; tx += 4)
					for (int c = 0; c < 2; c++) {
						std::vector<int> d, v, vp;
						for (int y = ty; y < std::min(ty + 4, vh); y++)
							for (int x = tx; x < std::min(tx + 4, vw); x++) {
								int a = mv[2 * (y * vw + x) + c], b = mv_prev[2 * (y * vw + x) + c];
								d.push_back(a - b), v.push_back(a), vp.push_back(b);
							}
						int n = (int)d.size();
						auto mean = [&](const std::vector<int> &x) {
							long s = 0;
							for (int e : x) s += e;
							return (int)lround((double)s / n);
						};
						double A = 4 + n * width_of(d);
						int md = mean(d);
						std::vector<int> r;
						for (int e : d) r.push_back(e - md);
						double B = 4 + width_of({md}) + 4 + n * width_of(r);
						int mvv = mean(v), mvp = mean(vp);
						r.clear();
						for (int e : v) r.push_back(e - mvv);
						double Cc = 4 + width_of({mvv - mvp}) + 4 + n * width_of(r);
						mvs[0] += A, mvs[1] += B, mvs[2] += Cc, mvs[3] += 2 + std::min(A, std::min(B, Cc));
					}
		};
		HvsTables T(h, getenv("RT_H") ? (float)atof(getenv("RT_H")) : 2.0f);
		bool useHvs = !getenv("RT_NOHVS");
		riptide_device_report_performance_stats(dev, stats_cb, &perf, true); // arm timestamps
		perf.clear();

		for (size_t target : sizes) {
			double se[3] = {0}, bytes = 0, hvs = 0;
			int fi = 0;
			riptide_encoder_set_reference(enc, NULL);
			riptide_decoder_set_reference(dec, NULL);
			if (cdec) riptide_decoder_set_reference(cdec, NULL);
			for (auto &fr : frames) {
				ti = fi & 1;
				if (predict && fi > 0) {
					int r = (fi - 1) & 1;
					riptide_gpu_buffers rg = {{(__bridge void *)texs[r][0], (__bridge void *)texs[r][1], (__bridge void *)texs[r][2]}};
					riptide_gpu_buffers crg = {{(__bridge void *)ctexs[r][0], (__bridge void *)ctexs[r][1], (__bridge void *)ctexs[r][2]}};
					// After the report, the encoder's state is the client's (replayed):
					// its planes and the vectors it codes the next ones against.
					bool replayed = cdec && loss_report > 0 && fi >= loss_frame + loss_report;
					riptide_encoder_set_reference(enc, replayed ? &crg : &rg);
					riptide_decoder_set_reference(dec, replayed ? &crg : &rg);
					if (cdec) riptide_decoder_set_reference(cdec, &crg);
					if (replayed) {
						riptide_decoder_get_motion_state(cdec, mv_state.data(), mv_state.size() / 2);
						riptide_encoder_set_motion_state(enc, mv_state.data(), mv_state.size() / 2);
						riptide_decoder_set_motion_state(dec, mv_state.data(), mv_state.size() / 2);
					}
				}
				bool counted = fi++ >= warm;
				riptide_cpu_buffer in = {};
				in.data[0] = fr.data(), in.data[1] = fr.data() + ysz, in.data[2] = fr.data() + ysz + csz;
				in.row_stride_in_bytes[0] = w, in.row_stride_in_bytes[1] = in.row_stride_in_bytes[2] = w / 2;
				in.plane_size_in_bytes[0] = ysz, in.plane_size_in_bytes[1] = in.plane_size_in_bytes[2] = csz;
				in.width = w, in.height = h, in.format = RIPTIDE_CPU_BUFFER_FORMAT_YUV420P;
				riptide_rate_control rc = {target};
				if (riptide_encoder_encode_cpu(enc, &in, &rc) != RIPTIDE_SUCCESS) return 1;
				size_t np = 0;
				if (riptide_encoder_packetize(enc, packets.data(), cdec ? 1400 : 1 << 20, &np, bits.data(), bits.size()) != RIPTIDE_SUCCESS) return 1;
				// The frame's bytes are its packets: inter packets (map and vectors)
				// first, then the coefficients.
				size_t total = 0, msz = 0, nv = 0, mvb = 0, side = 0;
				riptide_encoder_get_prediction_map(enc, map.data(), map.size(), &msz);
				riptide_encoder_get_motion(enc, mv.data(), mv.size(), &nv);
				if (nv) {
					if (getenv("RT_MVSTATS") && counted) mv_stats();
					mv_prev = mv;
				} else
					std::fill(mv_prev.begin(), mv_prev.end(), 0);
				size_t n_inter = 0;
				riptide_decoder_clear(dec);
				if (cdec) riptide_decoder_clear(cdec);
				for (size_t i = 0; i < np; i++) {
					total += packets[i].size;
					uint32_t w1;
					memcpy(&w1, bits.data() + packets[i].offset + 4, 4);
					bool inter = (bits[packets[i].offset + 3] & 0x80) && ((w1 >> 24) & 3) == 1;
					if (inter) n_inter++, mvb += packets[i].size;
					if (riptide_decoder_push_packet(dec, bits.data() + packets[i].offset, packets[i].size) != RIPTIDE_SUCCESS) return 1;
					bool lost = fi - 1 == loss_frame && i >= size_t(loss_start * np) && i < size_t((loss_start + loss_fraction) * np);
					if (cdec && !lost && riptide_decoder_push_packet(cdec, bits.data() + packets[i].offset, packets[i].size) != RIPTIDE_SUCCESS) return 1;
				}
				if (counted) bytes += total;
				if (dump) {
					// Each frame's packets, as [u32 size][bytes] records.
					FILE *df = fopen(dump, "ab");
					for (size_t i = 0; i < np; i++) {
						uint32_t sz = (uint32_t)packets[i].size;
						fwrite(&sz, 4, 1, df), fwrite(bits.data() + packets[i].offset, 1, sz, df);
					}
					fclose(df);
				}
				if (pdec && predict && fi > 1) {
					int r = (fi - 2) & 1; // this frame's reference
					riptide_gpu_buffers rg = {{(__bridge void *)texs[r][0], (__bridge void *)texs[r][1], (__bridge void *)texs[r][2]}};
					riptide_decoder_set_reference(pdec, &rg);
					riptide_decoder_get_motion_state(dec, mv_state.data(), mv_state.size() / 2);
					riptide_decoder_set_motion_state(pdec, mv_state.data(), mv_state.size() / 2);
					riptide_decoder_clear(pdec);
					for (size_t i = 0; i < n_inter; i++)
						if (riptide_decoder_push_packet(pdec, bits.data() + packets[i].offset, packets[i].size) != RIPTIDE_SUCCESS) return 1;
					id<MTLCommandBuffer> pc = [q commandBuffer];
					riptide_gpu_buffers pg = {{(__bridge void *)ptexs[0], (__bridge void *)ptexs[1], (__bridge void *)ptexs[2]}};
					if (riptide_decoder_decode_gpu_buffer(pdec, (__bridge void *)pc, &pg) != RIPTIDE_SUCCESS) return 1;
					id<MTLBlitCommandEncoder> pb = [pc blitCommandEncoder];
					for (int p = 0; p < 3; p++) {
						int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
						id<MTLTexture> srcs[2] = {ptexs[p], riptide_encoder_debug_prediction(enc, p)};
						for (int k = 0; k < 2; k++)
							[pb copyFromTexture:srcs[k] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
							         sourceSize:MTLSizeMake(pw, ph, 1) toBuffer:prb[k][p] destinationOffset:0
							destinationBytesPerRow:pw destinationBytesPerImage:(size_t)pw * ph];
					}
					[pb endEncoding];
					[pc commit];
					[pc waitUntilCompleted];
					for (int p = 0; p < 3; p++) {
						const uint8_t *x = (const uint8_t *)prb[0][p].contents, *y = (const uint8_t *)prb[1][p].contents;
						size_t n = p ? csz : ysz;
						for (size_t i = 0; i < n; i++) pred_mismatch += x[i] != y[i];
						pred_checked += n;
					}
				}
				id<MTLCommandBuffer> cb = [q commandBuffer];
				riptide_gpu_buffers gb = {{(__bridge void *)texs[ti][0], (__bridge void *)texs[ti][1], (__bridge void *)texs[ti][2]}};
				if (riptide_decoder_decode_gpu_buffer(dec, (__bridge void *)cb, &gb) != RIPTIDE_SUCCESS) return 1;
				riptide_gpu_buffers cgb = {{(__bridge void *)ctexs[ti][0], (__bridge void *)ctexs[ti][1], (__bridge void *)ctexs[ti][2]}};
				if (cdec && riptide_decoder_decode_gpu_buffer(cdec, (__bridge void *)cb, &cgb) != RIPTIDE_SUCCESS) return 1;
				id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
				for (int p = 0; p < 3; p++) {
					int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
					[bl copyFromTexture:texs[ti][p] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
					         sourceSize:MTLSizeMake(pw, ph, 1) toBuffer:rb[p] destinationOffset:0
					destinationBytesPerRow:pw destinationBytesPerImage:(size_t)pw * ph];
					if (cdec)
						[bl copyFromTexture:ctexs[ti][p] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
						         sourceSize:MTLSizeMake(pw, ph, 1) toBuffer:crb[p] destinationOffset:0
						destinationBytesPerRow:pw destinationBytesPerImage:(size_t)pw * ph];
				}
				[bl endEncoding];
				[cb commit];
				[cb waitUntilCompleted];
				if (getenv("RT_PERFRAME")) {
					// Per frame: index, total bytes, of which inter packets, 0, PSNR-Y.
					const uint8_t *b = (const uint8_t *)rb[0].contents;
					uint64_t s = 0;
					for (size_t i = 0; i < ysz; i++) { int d = fr.data()[i] - b[i]; s += (uint64_t)(d * d); }
					fprintf(stderr, "frame %d %zu %zu %zu %.3f\n", fi - 1, total, mvb, side, psnr((double)s / ysz));
				}
				if (cdec) {
					auto py = [&](id<MTLBuffer> b) {
						const uint8_t *d = (const uint8_t *)b.contents;
						uint64_t s = 0;
						for (size_t i = 0; i < ysz; i++) { int e = fr.data()[i] - d[i]; s += (uint64_t)(e * e); }
						return psnr((double)s / ysz);
					};
					// And the drift: the client's mean squared difference from the clean decode.
					const uint8_t *x = (const uint8_t *)rb[0].contents, *y = (const uint8_t *)crb[0].contents;
					uint64_t d2 = 0;
					for (size_t i = 0; i < ysz; i++) { int e = x[i] - y[i]; d2 += (uint64_t)(e * e); }
					fprintf(stderr, "loss %zu %d %.3f %.3f %.5f\n", target, fi - 1, py(rb[0]), py(crb[0]), (double)d2 / ysz);
				}
				if (!counted) continue;
				if (useHvs) hvs += psnr_hvs(T, fr.data(), (const uint8_t *)rb[0].contents, w, h);
				for (int p = 0; p < 3; p++) {
					const uint8_t *a = fr.data() + (p ? ysz + (p - 1) * csz : 0), *b = (const uint8_t *)rb[p].contents;
					size_t n = p ? csz : ysz;
					uint64_t s = 0;
					for (size_t i = 0; i < n; i++) {
						int d = a[i] - b[i];
						s += (uint64_t)(d * d);
					}
					se[p] += psnr((double)s / n);
				}
			}
			int n = (int)frames.size() - warm;
			double py = se[0] / n, pu = se[1] / n, pv = se[2] / n;
			if (getenv("RT_MVSTATS"))
				fprintf(stderr, "mvstats %zu A %.0f B %.0f C %.0f D %.0f bytes/frame\n", target, mvs[0] / 8 / n, mvs[1] / 8 / n,
				        mvs[2] / 8 / n, mvs[3] / 8 / n);
			if (getenv("RT_MVSTATS"))
				fprintf(stderr, "mvcount %zu changed %.1f%% nonzero %.1f%% changed under intra %.1f%%\n", target,
				        100 * mvn[1] / mvn[0], 100 * mvn[2] / mvn[0], 100 * mvn[3] / mvn[0]);
			for (double &m : mvs) m = 0;
			for (double &m : mvn) m = 0;
			printf("%zu %.1f %.4f %.4f %.4f %.4f %.4f\n", target, bytes / n, py, pu, pv, (6 * py + pu + pv) / 8, hvs / n);
			fflush(stdout);
		}
		if (pdec)
			fprintf(stderr, "prediction check: %zu of %zu samples differ between encoder and decoder\n", pred_mismatch, pred_checked);
		if (bench) {
			// Back-to-back encodes, then back-to-back decodes of the last frame at
			// the first size: the GPU pass times Riptide reports, best of `bench`.
			auto &fr = frames.back();
			riptide_cpu_buffer in = {};
			in.data[0] = fr.data(), in.data[1] = fr.data() + ysz, in.data[2] = fr.data() + ysz + csz;
			in.row_stride_in_bytes[0] = w, in.row_stride_in_bytes[1] = in.row_stride_in_bytes[2] = w / 2;
			in.plane_size_in_bytes[0] = ysz, in.plane_size_in_bytes[1] = in.plane_size_in_bytes[2] = csz;
			in.width = w, in.height = h, in.format = RIPTIDE_CPU_BUFFER_FORMAT_YUV420P;
			riptide_rate_control rc = {sizes[0]};
			riptide_device_report_performance_stats(dev, stats_cb, &perf, true);
			perf.clear();
			double t0 = CACurrentMediaTime();
			size_t np = 0;
			for (int i = 0; i < bench; i++) {
				riptide_encoder_encode_cpu(enc, &in, &rc);
				riptide_encoder_packetize(enc, packets.data(), cdec ? 1400 : 1 << 20, &np, bits.data(), bits.size());
			}
			double t1 = CACurrentMediaTime();
			riptide_gpu_buffers gb = {{(__bridge void *)texs[ti][0], (__bridge void *)texs[ti][1], (__bridge void *)texs[ti][2]}};
			id<MTLCommandBuffer> last = nil;
			double dec_gpu = 1e9;
			for (int i = 0; i < bench; i++) {
				for (size_t k = 0; k < np; k++) riptide_decoder_push_packet(dec, bits.data() + packets[k].offset, packets[k].size);
				id<MTLCommandBuffer> cb = [q commandBuffer];
				riptide_decoder_decode_gpu_buffer(dec, (__bridge void *)cb, &gb);
				[cb commit];
				[cb waitUntilCompleted];
				dec_gpu = fmin(dec_gpu, cb.GPUEndTime - cb.GPUStartTime);
				last = cb;
			}
			riptide_device_report_performance_stats(dev, stats_cb, &perf, true);
			fprintf(stderr, "bench: encode %.3f ms per frame (wall, incl. packetize), decode GPU best %.3f ms\n%s",
			        (t1 - t0) * 1e3 / bench, dec_gpu * 1e3, perf.c_str());
		}
		if (getenv("PERF")) {
			riptide_device_report_performance_stats(dev, stats_cb, &perf, true);
			fprintf(stderr, "%s", perf.c_str());
		}
		riptide_decoder_destroy(dec);
		riptide_encoder_destroy(enc);
		riptide_device_destroy(dev);
	}
	return 0;
}
