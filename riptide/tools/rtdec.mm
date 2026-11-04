// Metal decode check for tools/rtenc.c's output: decodes the packets in order
// (inter frames from the previous decoded frame; a frame in stripes, a decoder
// per stripe, as the client does), and reports per frame the PSNR-Y against the
// source and how far the decode is from the encoder's own (a different GPU's)
// decode.
//
//   rtdec in.pkt source.y4m [encoder_decode.yuv] [-drop frame:first:count]

#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "riptide_metal.h"

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

static uint32_t get32(FILE *f)
{
	uint32_t v = 0;
	if (fread(&v, 4, 1, f) != 1)
		return UINT32_MAX;
	return v;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		const char *pkt = NULL, *src = NULL, *ref = NULL;
		int drop_frame = -1, drop_first = 0, drop_count = 0;
		for (int i = 1; i < argc; i++)
		{
			if (!strcmp(argv[i], "-drop") && i + 1 < argc) sscanf(argv[++i], "%d:%d:%d", &drop_frame, &drop_first, &drop_count);
			else if (!pkt) pkt = argv[i];
			else if (!src) src = argv[i];
			else ref = argv[i];
		}
		FILE *pf = pkt ? fopen(pkt, "rb") : NULL, *sf = src ? fopen(src, "rb") : NULL, *rf = ref ? fopen(ref, "rb") : NULL;
		int w = 0, h = 0;
		if (!pf || !sf || y4m_header(sf, &w, &h))
		{
			fprintf(stderr, "usage: rtdec in.pkt source.y4m [encoder_decode.yuv] [-drop frame:first:count]\n");
			return 1;
		}
		size_t ysz = size_t(w) * h, csz = ysz / 4;
		std::vector<uint8_t> source(ysz + 2 * csz), theirs(ysz + 2 * csz);

		id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
		riptide_device dev;
		riptide_device_create_info di = {(__bridge void *)mtl, NULL, NULL};
		if (riptide_device_create(&di, &dev) != RIPTIDE_SUCCESS) return 1;
		std::vector<riptide_decoder> decs; // one per stripe, created as the stripes appear
		std::vector<std::vector<id<MTLTexture>>> stripe_out;
		id<MTLTexture> texs[2][3];
		id<MTLBuffer> rb[3];
		for (int p = 0; p < 3; p++)
		{
			int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:pw height:ph mipmapped:NO];
			td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
			td.storageMode = MTLStorageModePrivate;
			texs[0][p] = [mtl newTextureWithDescriptor:td];
			texs[1][p] = [mtl newTextureWithDescriptor:td];
			rb[p] = [mtl newBufferWithLength:size_t(pw) * ph options:MTLResourceStorageModeShared];
		}
		id<MTLCommandQueue> q = [mtl newCommandQueue];
		std::vector<uint8_t> packet(1 << 20);
		char line[256];
		double sum_psnr = 0, worst_diff = 0;
		int frames = 0;
		for (int fi = 0;; fi++)
		{
			uint32_t number = get32(pf), predicted = get32(pf), stripes = get32(pf);
			if (stripes == UINT32_MAX)
				break;
			int ti = fi & 1;
			id<MTLCommandBuffer> cb = [q commandBuffer];
			int global = 0;
			for (uint32_t k = 0; k < stripes; k++)
			{
				uint32_t sy = get32(pf), sh = get32(pf), n = get32(pf);
				if (k >= decs.size())
				{
					riptide_decoder d;
					riptide_decoder_create_info dci = {dev, w, int(sh), RIPTIDE_CHROMA_SUBSAMPLING_420};
					if (riptide_decoder_create(&dci, &d) != RIPTIDE_SUCCESS) return 1;
					decs.push_back(d);
					std::vector<id<MTLTexture>> planes;
					for (int p = 0; p < 3; p++)
					{
						MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
						    width:p ? w / 2 : w height:p ? sh / 2 : sh mipmapped:NO];
						td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
						td.storageMode = MTLStorageModePrivate;
						planes.push_back([mtl newTextureWithDescriptor:td]);
					}
					stripe_out.push_back(planes);
				}
				riptide_decoder dec = decs[k];
				riptide_gpu_buffers rg = {{(__bridge void *)texs[ti ^ 1][0], (__bridge void *)texs[ti ^ 1][1], (__bridge void *)texs[ti ^ 1][2]}};
				riptide_decoder_set_reference_stripe(dec, predicted ? &rg : NULL, int(sy));
				riptide_decoder_clear(dec);
				for (uint32_t i = 0; i < n; i++, global++)
				{
					uint32_t size = get32(pf);
					if (fread(packet.data(), 1, size, pf) != size)
						return 1;
					bool dropped = int(fi) == drop_frame && global >= drop_first && global < drop_first + drop_count;
					if (!dropped && riptide_decoder_push_packet(dec, packet.data(), size) != RIPTIDE_SUCCESS)
					{
						fprintf(stderr, "frame %d stripe %u packet %u rejected\n", fi, k, i);
						return 1;
					}
				}
				auto &so = stripe_out[k];
				riptide_gpu_buffers gb = {{(__bridge void *)so[0], (__bridge void *)so[1], (__bridge void *)so[2]}};
				if (riptide_decoder_decode_gpu_buffer(dec, (__bridge void *)cb, &gb) != RIPTIDE_SUCCESS)
					return 1;
				// The stripe into the frame.
				id<MTLBlitCommandEncoder> pl = [cb blitCommandEncoder];
				for (int p = 0; p < 3; p++)
					[pl copyFromTexture:so[p] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
					         sourceSize:MTLSizeMake(so[p].width, so[p].height, 1) toTexture:texs[ti][p]
					   destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, p ? sy / 2 : sy, 0)];
				[pl endEncoding];
			}
			id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
			for (int p = 0; p < 3; p++)
			{
				int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
				[bl copyFromTexture:texs[ti][p] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				         sourceSize:MTLSizeMake(pw, ph, 1) toBuffer:rb[p] destinationOffset:0
				destinationBytesPerRow:pw destinationBytesPerImage:size_t(pw) * ph];
			}
			[bl endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			if (getenv("RT_GPUTIME"))
				fprintf(stderr, "frame %d GPU %.3f ms\n", fi, (cb.GPUEndTime - cb.GPUStartTime) * 1e3);
			if (!fgets(line, sizeof line, sf) || fread(source.data(), 1, source.size(), sf) != source.size())
				break;
			bool have_theirs = rf && fread(theirs.data(), 1, theirs.size(), rf) == theirs.size();
			const uint8_t *y = (const uint8_t *)rb[0].contents;
			double se = 0, de = 0;
			size_t differ = 0;
			for (size_t i = 0; i < ysz; i++)
			{
				int d = source[i] - y[i];
				se += d * d;
				if (have_theirs)
				{
					int e = theirs[i] - y[i];
					de += e * e;
					differ += e != 0;
				}
			}
			double psnr = 10 * log10(255.0 * 255.0 / std::max(se / ysz, 1e-10));
			sum_psnr += psnr;
			frames++;
			if (have_theirs)
				worst_diff = std::max(worst_diff, de / ysz);
			printf("frame %d (%u) %s %u stripes PSNR-Y %.3f", fi, number, predicted ? "P" : "I", stripes, psnr);
			if (have_theirs)
				printf("  vs encoder's decode: MSE %.4f, %.2f%% of luma samples differ", de / ysz, 100.0 * differ / ysz);
			printf("\n");
		}
		printf("%d frames, mean PSNR-Y %.3f, worst MSE against the encoder's decode %.4f\n", frames, sum_psnr / frames, worst_diff);
	}
	return 0;
}
