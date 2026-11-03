// What this GPU's VA-API AV1 encoder can do: entrypoints, tiles, packed
// headers, and every config attribute it reports. The tile support and tile
// count are what streamable AV1 rests on (DESIGN.md §12).
//
// Build in the gmstream build image (build/Containerfile):
//   gcc -O2 -o caps caps.c $(pkg-config --cflags libva-drm libva libdrm) \
//     -Wl,--start-group $(pkg-config --libs --static libva-drm libva libdrm) \
//     -Wl,--end-group
// Run on the device: ./caps  (GM_DRM=/dev/dri/renderD129 for another GPU)
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_av1.h>

static const char *attr_name(VAConfigAttribType t) {
	switch (t) {
	case VAConfigAttribRTFormat: return "RTFormat";
	case VAConfigAttribRateControl: return "RateControl";
	case VAConfigAttribEncPackedHeaders: return "EncPackedHeaders";
	case VAConfigAttribEncMaxRefFrames: return "EncMaxRefFrames";
	case VAConfigAttribEncMaxSlices: return "EncMaxSlices";
	case VAConfigAttribEncSliceStructure: return "EncSliceStructure";
	case VAConfigAttribEncQualityRange: return "EncQualityRange";
	case VAConfigAttribEncQuantization: return "EncQuantization";
	case VAConfigAttribEncIntraRefresh: return "EncIntraRefresh";
	case VAConfigAttribEncSkipFrame: return "EncSkipFrame";
	case VAConfigAttribEncROI: return "EncROI";
	case VAConfigAttribEncRateControlExt: return "EncRateControlExt";
	case VAConfigAttribEncTileSupport: return "EncTileSupport";
	case VAConfigAttribEncDirtyRect: return "EncDirtyRect";
	case VAConfigAttribEncParallelRateControl: return "EncParallelRateControl";
	case VAConfigAttribEncDynamicScaling: return "EncDynamicScaling";
	case VAConfigAttribMaxFrameSize: return "MaxFrameSize";
	case VAConfigAttribPredictionDirection: return "PredictionDirection";
	case VAConfigAttribMaxPictureWidth: return "MaxPictureWidth";
	case VAConfigAttribMaxPictureHeight: return "MaxPictureHeight";
	case VAConfigAttribEncAV1: return "EncAV1";
	case VAConfigAttribEncAV1Ext1: return "EncAV1Ext1";
	case VAConfigAttribEncAV1Ext2: return "EncAV1Ext2";
	default: return NULL;
	}
}

static void dump_profile(VADisplay va, VAProfile p, const char *name) {
	int n = vaMaxNumEntrypoints(va);
	VAEntrypoint *eps = calloc(n, sizeof(*eps));
	if (vaQueryConfigEntrypoints(va, p, eps, &n) != VA_STATUS_SUCCESS) {
		free(eps);
		return;
	}
	for (int i = 0; i < n; i++) {
		if (eps[i] != VAEntrypointEncSlice && eps[i] != VAEntrypointEncSliceLP) {
			continue;
		}
		printf("%s / %s\n", name, eps[i] == VAEntrypointEncSlice ? "EncSlice" : "EncSliceLP");
		VAConfigAttrib attrs[VAConfigAttribTypeMax];
		int m = 0;
		for (int t = 0; t < VAConfigAttribTypeMax; t++) {
			attrs[m].type = (VAConfigAttribType)t;
			attrs[m].value = 0;
			m++;
		}
		if (vaGetConfigAttributes(va, p, eps[i], attrs, m) != VA_STATUS_SUCCESS) {
			printf("  (attribute query failed)\n");
			continue;
		}
		for (int j = 0; j < m; j++) {
			if (attrs[j].value == VA_ATTRIB_NOT_SUPPORTED) continue;
			const char *an = attr_name(attrs[j].type);
			printf("  %-24s 0x%08x (%u)\n", an ? an : "?", attrs[j].value, attrs[j].value);
			if (attrs[j].type == VAConfigAttribEncAV1) {
				VAConfigAttribValEncAV1 v = { .value = attrs[j].value };
				printf("    av1: 128sb %u filter_intra %u cdef_strength %u superres %u restoration %u intrabc %u\n",
					v.bits.support_128x128_superblock, v.bits.support_filter_intra,
					v.bits.support_cdef_channel_strength, v.bits.support_superres,
					v.bits.support_restoration, v.bits.support_allow_intrabc);
			}
			if (attrs[j].type == VAConfigAttribEncAV1Ext2) {
				VAConfigAttribValEncAV1Ext2 v = { .value = attrs[j].value };
				printf("    ext2: tile_size_bytes_minus1 %u obu_size_bytes_minus1 %u "
					"tx_mode_support 0x%x max_tile_num_minus1 %u\n",
					v.bits.tile_size_bytes_minus1, v.bits.obu_size_bytes_minus1,
					v.bits.tx_mode_support, v.bits.max_tile_num_minus1);
			}
			if (attrs[j].type == VAConfigAttribEncTileSupport) {
				printf("    tile support: %s\n", attrs[j].value ? "yes" : "no");
			}
		}
		// Surface attributes for a config, to see max sizes.
		VAConfigID cfg;
		VAConfigAttrib want[] = {
			{ .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
		};
		if (vaCreateConfig(va, p, eps[i], want, 1, &cfg) == VA_STATUS_SUCCESS) {
			unsigned na = 0;
			vaQuerySurfaceAttributes(va, cfg, NULL, &na);
			VASurfaceAttrib *sa = calloc(na, sizeof(*sa));
			if (vaQuerySurfaceAttributes(va, cfg, sa, &na) == VA_STATUS_SUCCESS) {
				for (unsigned k = 0; k < na; k++) {
					if (sa[k].type == VASurfaceAttribMaxWidth)
						printf("  max width  %d\n", sa[k].value.value.i);
					if (sa[k].type == VASurfaceAttribMaxHeight)
						printf("  max height %d\n", sa[k].value.value.i);
					if (sa[k].type == VASurfaceAttribMinWidth)
						printf("  min width  %d\n", sa[k].value.value.i);
					if (sa[k].type == VASurfaceAttribMinHeight)
						printf("  min height %d\n", sa[k].value.value.i);
				}
			}
			free(sa);
			vaDestroyConfig(va, cfg);
		} else {
			printf("  (vaCreateConfig YUV420 failed)\n");
		}
	}
	free(eps);
}

int main(void) {
	const char *dev = getenv("GM_DRM") ? getenv("GM_DRM") : "/dev/dri/renderD128";
	int fd = open(dev, O_RDWR);
	if (fd < 0) { perror(dev); return 1; }
	VADisplay va = vaGetDisplayDRM(fd);
	int major, minor;
	if (vaInitialize(va, &major, &minor) != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize failed\n");
		return 1;
	}
	printf("VA-API %d.%d: %s\n", major, minor, vaQueryVendorString(va));
	printf("libva build %s\n", VA_VERSION_S);
	dump_profile(va, VAProfileAV1Profile0, "AV1Profile0");
	dump_profile(va, VAProfileH264High, "H264High");
	dump_profile(va, VAProfileHEVCMain, "HEVCMain");
	vaTerminate(va);
	close(fd);
	return 0;
}
