// Vulkan encode test: encodes a Y4M clip through the C API, as gmstream's host
// does (inter frames: stripe by stripe, riptide_encoder_packetize_stripe), and
// writes the packets and the encoder's own decode for tools/rtdec.mm to check.
//
//   rtenc in.y4m out.pkt [-n frames] [-s bytes] [-p] [-rows stripe_rows] [-dec out.yuv]
//         [-loss frame:first:count] [-prof]
//
// out.pkt: per frame u32 frame, u32 predicted, u32 stripes, then per stripe u32
// y, u32 height, u32 n_packets and per packet u32 size and the bytes. -loss
// reports packets first..first+count-1 of a frame lost (after encoding it), so
// the next frames come from the replay. Prints each frame's PSNR-Y (the
// encoder's own decode against the source) with -p.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "riptide.h"

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int y4m_header(FILE *f, int *w, int *h)
{
	char line[512];
	if (!fgets(line, sizeof line, f) || strncmp(line, "YUV4MPEG2", 9))
		return -1;
	for (char *t = strtok(line, " \n"); t; t = strtok(NULL, " \n"))
	{
		if (t[0] == 'W') *w = atoi(t + 1);
		if (t[0] == 'H') *h = atoi(t + 1);
	}
	return *w > 0 && *h > 0 ? 0 : -1;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static void print_msg(void *userdata, const char *msg) { (void)userdata; fprintf(stderr, "  %s\n", msg); }

int main(int argc, char **argv)
{
	const char *in = NULL, *out = NULL, *dec_path = NULL;
	int frames = 30, predict = 0, prof = 0, rows = 0, first_rows = 0, loss_frame = -1, loss_first = 0, loss_count = 0;
	size_t size = 75000;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-n") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) size = strtoull(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "-p")) predict = 1;
		else if (!strcmp(argv[i], "-prof")) prof = 1;
		else if (!strcmp(argv[i], "-rows") && i + 1 < argc) rows = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-first") && i + 1 < argc) first_rows = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-dec") && i + 1 < argc) dec_path = argv[++i];
		else if (!strcmp(argv[i], "-loss") && i + 1 < argc) sscanf(argv[++i], "%d:%d:%d", &loss_frame, &loss_first, &loss_count);
		else if (!in) in = argv[i];
		else out = argv[i];
	}
	FILE *f = in ? fopen(in, "rb") : NULL;
	int w = 0, h = 0;
	if (!f || !out || y4m_header(f, &w, &h))
	{
		fprintf(stderr, "usage: rtenc in.y4m out.pkt [-n frames] [-s bytes] [-p] [-dec out.yuv]\n");
		return 1;
	}
	FILE *o = fopen(out, "wb"), *dec = dec_path ? fopen(dec_path, "wb") : NULL;
	size_t ysz = (size_t)w * h, csz = ysz / 4;
	uint8_t *frame = malloc(ysz + 2 * csz), *decoded = malloc(ysz + 2 * csz);

	riptide_device dev;
	if (riptide_create_default_device(&dev) != RIPTIDE_SUCCESS)
		return 1;
	riptide_encoder enc;
	riptide_encoder_create_info ei = {dev, w, h, RIPTIDE_CHROMA_SUBSAMPLING_420};
	if (riptide_encoder_create(&ei, &enc) != RIPTIDE_SUCCESS)
		return 1;
	if (predict)
	{
		riptide_inter_info ii = {true, 1100, 8, rows, first_rows};
		if (riptide_encoder_enable_inter(enc, &ii) != RIPTIDE_SUCCESS)
		{
			fprintf(stderr, "enable_inter failed\n");
			return 1;
		}
	}
	size_t cap = 1 << 16;
	riptide_packet *packets = malloc(cap * sizeof(*packets));
	size_t bits_cap = 16 << 20;
	uint8_t *bits = malloc(bits_cap), *ibits = malloc(1 << 20);
	double t_inter = 0, t_all = 0, sum_psnr = 0, t_record = 0;
	int n_psnr = 0;
	char line[256];
	for (int fi = 0; fi < frames; fi++)
	{
		if (!fgets(line, sizeof line, f) || fread(frame, 1, ysz + 2 * csz, f) != ysz + 2 * csz)
			break;
		riptide_cpu_buffer cb = {0};
		cb.data[0] = frame, cb.data[1] = frame + ysz, cb.data[2] = frame + ysz + csz;
		cb.row_stride_in_bytes[0] = w, cb.row_stride_in_bytes[1] = cb.row_stride_in_bytes[2] = w / 2;
		cb.plane_size_in_bytes[0] = ysz, cb.plane_size_in_bytes[1] = cb.plane_size_in_bytes[2] = csz;
		cb.width = w, cb.height = h, cb.format = RIPTIDE_CPU_BUFFER_FORMAT_YUV420P;
		riptide_rate_control rc = {size};
		double t0 = now_ms();
		if (riptide_encoder_encode_cpu(enc, &cb, &rc) != RIPTIDE_SUCCESS)
		{
			fprintf(stderr, "encode failed\n");
			return 1;
		}
		double t_rec = now_ms();
		if (fi > 0)
			t_record += t_rec - t0;
		uint32_t number = fi;
		bool predicted = false;
		size_t total = 0, count = 0;
		double t1 = 0;
		if (predict)
		{
			riptide_encoder_get_frame_info(enc, &number, &predicted);
			int stripes = riptide_encoder_get_stripe_count(enc);
			put32(o, number), put32(o, predicted), put32(o, (uint32_t)stripes);
			for (int k = 0; k < stripes; k++)
			{
				int y = 0, h = 0;
				size_t n = 0;
				riptide_encoder_get_stripe(enc, k, &y, &h);
				if (riptide_encoder_packetize_stripe(enc, k, 1100, packets, cap, &n, NULL, bits, bits_cap) != RIPTIDE_SUCCESS)
					return 1;
				if (k == 0)
					t1 = now_ms();
				put32(o, (uint32_t)y), put32(o, (uint32_t)h), put32(o, (uint32_t)n);
				for (size_t i = 0; i < n; i++)
				{
					put32(o, (uint32_t)packets[i].size);
					fwrite(bits + packets[i].offset, 1, packets[i].size, o);
					total += packets[i].size;
				}
				count += n;
				riptide_encoder_decode_stripe(enc, k);
			}
		}
		else
		{
			size_t n = 0;
			t1 = now_ms();
			if (riptide_encoder_packetize(enc, packets, 1100, &n, bits, bits_cap) != RIPTIDE_SUCCESS)
				return 1;
			put32(o, number), put32(o, 0), put32(o, 1), put32(o, 0), put32(o, (uint32_t)h), put32(o, (uint32_t)n);
			for (size_t i = 0; i < n; i++)
			{
				put32(o, (uint32_t)packets[i].size);
				fwrite(bits + packets[i].offset, 1, packets[i].size, o);
				total += packets[i].size;
			}
			count = n;
		}
		double t2 = now_ms();
		if (fi > 0)
			t_inter += t1 - t0, t_all += t2 - t0;
		double psnr = 0;
		if (predict)
		{
			riptide_cpu_buffer db = cb;
			db.data[0] = decoded, db.data[1] = decoded + ysz, db.data[2] = decoded + ysz + csz;
			riptide_encoder_read_last_decode(enc, &db);
			if (dec)
				fwrite(decoded, 1, ysz + 2 * csz, dec);
			double se = 0;
			for (size_t i = 0; i < ysz; i++)
			{
				int d = frame[i] - decoded[i];
				se += d * d;
			}
			psnr = 10 * log10(255.0 * 255.0 / (se / ysz > 1e-10 ? se / ysz : 1e-10));
			if (fi >= 10)
				sum_psnr += psnr, n_psnr++;
		}
		if (predict && fi == loss_frame)
		{
			uint8_t *received = malloc(count);
			memset(received, 1, count);
			for (int i = loss_first; i < loss_first + loss_count && i < (int)count; i++)
				received[i] = 0;
			riptide_result r = riptide_encoder_report_loss(enc, number, received, count);
			fprintf(stderr, "frame %d: reported %d of %zu packets lost: %d\n", fi, loss_count, count, r);
			free(received);
		}
		fprintf(stderr, "frame %d %s %zu bytes, %zu packets, PSNR-Y %.3f; first stripe ready %.2f ms, all %.2f ms\n",
		        fi, predicted ? "P" : "I", total, count, psnr, t1 - t0, t2 - t0);
	}
	if (frames > 1)
		fprintf(stderr, "mean after the first: encode call %.2f ms, ", t_record / (frames - 1));
		fprintf(stderr, "first stripe ready %.2f ms, all packets %.2f ms (CPU wall, upload included); "
		        "PSNR-Y from frame 10: %.3f\n", t_inter / (frames - 1), t_all / (frames - 1), n_psnr ? sum_psnr / n_psnr : 0.0);
	if (prof)
		riptide_device_report_performance_stats(dev, print_msg, NULL, true);
	fclose(o);
	if (dec)
		fclose(dec);
	riptide_encoder_destroy(enc);
	riptide_device_destroy(dev);
	return 0;
}
