// What this GPU's Vulkan AV1 encoder can do: which queue families encode,
// the AV1 encode capabilities, the input formats, and whether the encoder
// will take an RGB image and convert it itself (VK_VALVE_video_encode_rgb_
// conversion, i.e. VCN's EFC). This is the ground the Vulkan encode path
// stands on; see DESIGN.md §12.
//
// Build in the gmstream build image (build/Containerfile):
//   gcc -O2 -o vkenc vkenc.c -I/opt/gm/include -ldl
// Run on the device: ./vkenc
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr getproc;
#define LOAD(inst, name) ((PFN_##name)getproc(inst, #name))

struct bit { uint64_t bit; const char *name; };

static void print_bits(const char *label, uint64_t flags, const struct bit *bits) {
	printf("  %-34s", label);
	if (flags == 0) {
		printf(" none\n");
		return;
	}
	for (const struct bit *b = bits; b->name != NULL; b++) {
		if (flags & b->bit) {
			printf(" %s", b->name);
			flags &= ~b->bit;
		}
	}
	if (flags != 0) {
		printf(" +%#llx", (unsigned long long)flags);
	}
	printf("\n");
}

static const struct bit video_caps[] = {
	{ VK_VIDEO_CAPABILITY_PROTECTED_CONTENT_BIT_KHR, "protected-content" },
	{ VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR, "separate-reference-images" },
	{ 0, NULL },
};
static const struct bit enc_caps[] = {
	{ VK_VIDEO_ENCODE_CAPABILITY_PRECEDING_EXTERNALLY_ENCODED_BYTES_BIT_KHR, "preceding-external-bytes" },
	{ VK_VIDEO_ENCODE_CAPABILITY_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_DETECTION_BIT_KHR, "short-buffer-detection" },
	{ VK_VIDEO_ENCODE_CAPABILITY_QUANTIZATION_DELTA_MAP_BIT_KHR, "quantization-delta-map" },
	{ VK_VIDEO_ENCODE_CAPABILITY_EMPHASIS_MAP_BIT_KHR, "emphasis-map" },
	{ 0, NULL },
};
static const struct bit rate_modes[] = {
	{ VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR, "disabled(explicit-q)" },
	{ VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR, "cbr" },
	{ VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR, "vbr" },
	{ 0, NULL },
};
static const struct bit feedback[] = {
	{ VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR, "offset" },
	{ VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR, "bytes-written" },
	{ VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_HAS_OVERRIDES_BIT_KHR, "has-overrides" },
	{ VK_VIDEO_ENCODE_FEEDBACK_AVERAGE_QUANTIZATION_BIT_KHR, "average-q" },
	{ VK_VIDEO_ENCODE_FEEDBACK_MIN_QUANTIZATION_BIT_KHR, "min-q" },
	{ VK_VIDEO_ENCODE_FEEDBACK_MAX_QUANTIZATION_BIT_KHR, "max-q" },
	{ VK_VIDEO_ENCODE_FEEDBACK_INTRA_PIXELS_BIT_KHR, "intra-pixels" },
	{ VK_VIDEO_ENCODE_FEEDBACK_INTER_PIXELS_BIT_KHR, "inter-pixels" },
	{ VK_VIDEO_ENCODE_FEEDBACK_SKIPPED_PIXELS_BIT_KHR, "skipped-pixels" },
	{ VK_VIDEO_ENCODE_FEEDBACK_PICTURE_PARTITION_COUNT_BIT_KHR, "partition-count" },
	{ 0, NULL },
};
static const struct bit av1_caps[] = {
	{ VK_VIDEO_ENCODE_AV1_CAPABILITY_PER_RATE_CONTROL_GROUP_MIN_MAX_Q_INDEX_BIT_KHR, "per-group-min-max-q" },
	{ VK_VIDEO_ENCODE_AV1_CAPABILITY_GENERATE_OBU_EXTENSION_HEADER_BIT_KHR, "generate-obu-extension-header" },
	{ VK_VIDEO_ENCODE_AV1_CAPABILITY_PRIMARY_REFERENCE_CDF_ONLY_BIT_KHR, "primary-reference-cdf-only" },
	{ VK_VIDEO_ENCODE_AV1_CAPABILITY_FRAME_SIZE_OVERRIDE_BIT_KHR, "frame-size-override" },
	{ VK_VIDEO_ENCODE_AV1_CAPABILITY_MOTION_VECTOR_SCALING_BIT_KHR, "motion-vector-scaling" },
	{ VK_VIDEO_ENCODE_AV1_CAPABILITY_COMPOUND_PREDICTION_INTRA_REFRESH_BIT_KHR, "compound-prediction-intra-refresh" },
	{ 0, NULL },
};
static const struct bit av1_std[] = {
	{ VK_VIDEO_ENCODE_AV1_STD_UNIFORM_TILE_SPACING_FLAG_SET_BIT_KHR, "uniform-tile-spacing" },
	{ VK_VIDEO_ENCODE_AV1_STD_SKIP_MODE_PRESENT_UNSET_BIT_KHR, "skip-mode-present-unset" },
	{ VK_VIDEO_ENCODE_AV1_STD_PRIMARY_REF_FRAME_BIT_KHR, "primary-ref-frame" },
	{ VK_VIDEO_ENCODE_AV1_STD_DELTA_Q_BIT_KHR, "delta-q" },
	{ 0, NULL },
};
static const struct bit superblocks[] = {
	{ VK_VIDEO_ENCODE_AV1_SUPERBLOCK_SIZE_64_BIT_KHR, "64" },
	{ VK_VIDEO_ENCODE_AV1_SUPERBLOCK_SIZE_128_BIT_KHR, "128" },
	{ 0, NULL },
};
static const struct bit rgb_models[] = {
	{ VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_RGB_IDENTITY_BIT_VALVE, "rgb-identity" },
	{ VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_IDENTITY_BIT_VALVE, "ycbcr-identity" },
	{ VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_709_BIT_VALVE, "709" },
	{ VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_601_BIT_VALVE, "601" },
	{ VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_2020_BIT_VALVE, "2020" },
	{ 0, NULL },
};
static const struct bit rgb_ranges[] = {
	{ VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_FULL_RANGE_BIT_VALVE, "full" },
	{ VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_NARROW_RANGE_BIT_VALVE, "narrow" },
	{ 0, NULL },
};
static const struct bit rgb_offsets[] = {
	{ VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_COSITED_EVEN_BIT_VALVE, "cosited-even" },
	{ VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_MIDPOINT_BIT_VALVE, "midpoint" },
	{ 0, NULL },
};

// The extensions the Vulkan encode path needs, and why.
static const char *wanted[] = {
	VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
	VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
	VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME,
	VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
	VK_VALVE_VIDEO_ENCODE_RGB_CONVERSION_EXTENSION_NAME,
	VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
	VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
	VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
	NULL,
};

static bool av1_encode_caps_sub(VkPhysicalDevice dev, VkInstance inst, bool rgb,
	unsigned depth, VkVideoChromaSubsamplingFlagsKHR chroma, const char *cname);

static const char *fmt_name(VkFormat f) {
	switch (f) {
	case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return "NV12 (G8_B8R8_2PLANE_420)";
	case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: return "I420 (G8_B8_R8_3PLANE_420)";
	case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return "P010";
	case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "A8B8G8R8_UNORM_PACK32";
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
	default: return NULL;
	}
}

static void print_formats(VkPhysicalDevice dev, VkInstance inst,
		const VkVideoProfileInfoKHR *profile, VkImageUsageFlags usage,
		const char *label) {
	PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR fn =
		LOAD(inst, vkGetPhysicalDeviceVideoFormatPropertiesKHR);
	VkVideoProfileListInfoKHR list = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
		.profileCount = 1, .pProfiles = profile };
	VkPhysicalDeviceVideoFormatInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
		.pNext = &list, .imageUsage = usage };
	uint32_t n = 0;
	VkResult r = fn(dev, &info, &n, NULL);
	if (r != VK_SUCCESS || n == 0) {
		printf("  %-34s none (%d)\n", label, r);
		return;
	}
	VkVideoFormatPropertiesKHR *props = calloc(n, sizeof(*props));
	for (uint32_t i = 0; i < n; i++) {
		props[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	}
	fn(dev, &info, &n, props);
	printf("  %-34s", label);
	for (uint32_t i = 0; i < n; i++) {
		const char *name = fmt_name(props[i].format);
		if (name != NULL) {
			printf(" %s", name);
		} else {
			printf(" fmt%d", props[i].format);
		}
		printf("%s", props[i].imageTiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ? "[drm]" :
			props[i].imageTiling == VK_IMAGE_TILING_LINEAR ? "[linear]" : "");
	}
	printf("\n");
	free(props);
}

// AV1 encode capabilities for this profile; `rgb` chains the VALVE
// RGB-conversion profile, so the answer says whether the encoder will take an
// RGB image directly.
static bool av1_encode_caps(VkPhysicalDevice dev, VkInstance inst, bool rgb,
		unsigned depth) {
	return av1_encode_caps_sub(dev, inst, rgb, depth,
		VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR, "4:2:0");
}

static bool av1_encode_caps_sub(VkPhysicalDevice dev, VkInstance inst, bool rgb,
		unsigned depth, VkVideoChromaSubsamplingFlagsKHR chroma, const char *cname) {
	VkVideoEncodeProfileRgbConversionInfoVALVE rgb_profile = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_PROFILE_RGB_CONVERSION_INFO_VALVE,
		.performEncodeRgbConversion = VK_TRUE };
	VkVideoEncodeUsageInfoKHR usage = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR,
		.pNext = rgb ? (void *)&rgb_profile : NULL,
		.videoUsageHints = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR,
		.videoContentHints = VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR,
		.tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR };
	VkVideoEncodeAV1ProfileInfoKHR av1 = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR,
		.pNext = &usage, .stdProfile = STD_VIDEO_AV1_PROFILE_MAIN };
	VkVideoProfileInfoKHR profile = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, .pNext = &av1,
		.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
		.chromaSubsampling = chroma,
		.lumaBitDepth = depth == 10 ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR
			: VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
		.chromaBitDepth = depth == 10 ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR
			: VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR };

	VkVideoEncodeRgbConversionCapabilitiesVALVE rgb_cap = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RGB_CONVERSION_CAPABILITIES_VALVE };
	VkVideoEncodeAV1CapabilitiesKHR a = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR,
		.pNext = rgb ? (void *)&rgb_cap : NULL };
	VkVideoEncodeCapabilitiesKHR e = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR, .pNext = &a };
	VkVideoCapabilitiesKHR c = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, .pNext = &e };

	printf("\nAV1 main %u-bit %s%s\n", depth, cname,
		rgb ? ", RGB conversion in the encoder" : "");
	VkResult r = LOAD(inst, vkGetPhysicalDeviceVideoCapabilitiesKHR)(dev, &profile, &c);
	if (r != VK_SUCCESS) {
		printf("  not supported (%d)\n", r);
		return false;
	}
	print_bits("video flags", c.flags, video_caps);
	printf("  %-34s %llu offset, %llu size\n", "bitstream alignment",
		(unsigned long long)c.minBitstreamBufferOffsetAlignment,
		(unsigned long long)c.minBitstreamBufferSizeAlignment);
	printf("  %-34s %ux%u .. %ux%u, granularity %ux%u\n", "coded extent",
		c.minCodedExtent.width, c.minCodedExtent.height,
		c.maxCodedExtent.width, c.maxCodedExtent.height,
		c.pictureAccessGranularity.width, c.pictureAccessGranularity.height);
	printf("  %-34s %u slots, %u active references\n", "dpb",
		c.maxDpbSlots, c.maxActiveReferencePictures);
	printf("  %-34s %s v%u.%u.%u\n", "std header", c.stdHeaderVersion.extensionName,
		VK_API_VERSION_MAJOR(c.stdHeaderVersion.specVersion),
		VK_API_VERSION_MINOR(c.stdHeaderVersion.specVersion),
		VK_API_VERSION_PATCH(c.stdHeaderVersion.specVersion));
	print_bits("encode flags", e.flags, enc_caps);
	print_bits("rate control", e.rateControlModes, rate_modes);
	print_bits("feedback", e.supportedEncodeFeedbackFlags, feedback);
	printf("  %-34s %u layers, %llu bit/s max, %u quality levels, input granularity %ux%u\n",
		"rate control limits", e.maxRateControlLayers,
		(unsigned long long)e.maxBitrate, e.maxQualityLevels,
		e.encodeInputPictureGranularity.width, e.encodeInputPictureGranularity.height);
	print_bits("av1 flags", a.flags, av1_caps);
	print_bits("av1 std syntax we may set", a.stdSyntaxFlags, av1_std);
	print_bits("superblock sizes", a.superblockSizes, superblocks);
	printf("  %-34s max %ux%u, tile %ux%u .. %ux%u, picture alignment %ux%u\n", "tiles",
		a.maxTiles.width, a.maxTiles.height,
		a.minTileSize.width, a.minTileSize.height,
		a.maxTileSize.width, a.maxTileSize.height,
		a.codedPictureAlignment.width, a.codedPictureAlignment.height);
	printf("  %-34s level %u, q index %u..%u, %u temporal / %u spatial layers\n", "level and q",
		a.maxLevel, a.minQIndex, a.maxQIndex,
		a.maxTemporalLayerCount, a.maxSpatialLayerCount);
	printf("  %-34s single %u, unidir %u, bidir %u, gop-remaining %s%s\n", "references",
		a.maxSingleReferenceCount, a.maxUnidirectionalCompoundReferenceCount,
		a.maxBidirectionalCompoundReferenceCount,
		a.prefersGopRemainingFrames ? "preferred" : "not preferred",
		a.requiresGopRemainingFrames ? ", required" : "");
	if (rgb) {
		print_bits("rgb models", rgb_cap.rgbModels, rgb_models);
		print_bits("rgb ranges", rgb_cap.rgbRanges, rgb_ranges);
		print_bits("rgb chroma offsets x", rgb_cap.xChromaOffsets, rgb_offsets);
		print_bits("rgb chroma offsets y", rgb_cap.yChromaOffsets, rgb_offsets);
	}
	print_formats(dev, inst, &profile, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, "input formats");
	print_formats(dev, inst, &profile, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, "dpb formats");

	for (uint32_t q = 0; q < e.maxQualityLevels; q++) {
		VkVideoEncodeAV1QualityLevelPropertiesKHR aq = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUALITY_LEVEL_PROPERTIES_KHR };
		VkVideoEncodeQualityLevelPropertiesKHR qp = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR,
			.pNext = &aq };
		VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR qi = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,
			.pVideoProfile = &profile, .qualityLevel = q };
		if (LOAD(inst, vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR)(dev, &qi, &qp)
				!= VK_SUCCESS) {
			continue;
		}
		printf("  quality level %u%-20s prefers rate control %#x, gop %u, key period %u, "
			"const q %u/%u/%u\n", q, "", qp.preferredRateControlMode,
			aq.preferredGopFrameCount, aq.preferredKeyFramePeriod,
			aq.preferredConstantQIndex.intraQIndex,
			aq.preferredConstantQIndex.predictiveQIndex,
			aq.preferredConstantQIndex.bipredictiveQIndex);
	}
	return true;
}

// Which DRM modifiers this format can be encoded from. This has to use the
// "2" structures: VkDrmFormatModifierPropertiesEXT carries the old 32-bit
// VkFormatFeatureFlags, which has no video bits at all, so asking with it
// always answers no.
static void print_modifiers(VkPhysicalDevice dev, VkInstance inst, VkFormat f,
		const char *name) {
	PFN_vkGetPhysicalDeviceFormatProperties2 fn =
		(PFN_vkGetPhysicalDeviceFormatProperties2)getproc(inst,
			"vkGetPhysicalDeviceFormatProperties2");
	VkDrmFormatModifierPropertiesList2EXT list = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT };
	VkFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &list };
	fn(dev, f, &props);
	if (list.drmFormatModifierCount == 0) {
		printf("\n%s: no DRM modifiers\n", name);
		return;
	}
	VkDrmFormatModifierProperties2EXT *mods =
		calloc(list.drmFormatModifierCount, sizeof(*mods));
	list.pDrmFormatModifierProperties = mods;
	fn(dev, f, &props);
	printf("\n%s: %u modifiers\n", name, list.drmFormatModifierCount);
	for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
		VkFormatFeatureFlags2 ff = mods[i].drmFormatModifierTilingFeatures;
		printf("  0x%016llx %u plane%s%s%s%s%s\n",
			(unsigned long long)mods[i].drmFormatModifier,
			mods[i].drmFormatModifierPlaneCount,
			mods[i].drmFormatModifierPlaneCount == 1 ? " " : "s",
			ff & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR ? " encode-input" : "",
			ff & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT ? " sampled" : "",
			ff & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT ? " transfer-src" : "",
			ff & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT ? " render-target" : "");
	}
	free(mods);
}

int main(void) {
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (lib == NULL) {
		fprintf(stderr, "libvulkan.so.1: %s\n", dlerror());
		return 1;
	}
	getproc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	if (getproc == NULL) {
		fprintf(stderr, "no vkGetInstanceProcAddr\n");
		return 1;
	}
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkenc", .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	VkInstance inst = VK_NULL_HANDLE;
	VkResult r = LOAD(NULL, vkCreateInstance)(&ici, NULL, &inst);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "vkCreateInstance: %d\n", r);
		return 1;
	}
	uint32_t n = 0;
	LOAD(inst, vkEnumeratePhysicalDevices)(inst, &n, NULL);
	VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
	LOAD(inst, vkEnumeratePhysicalDevices)(inst, &n, devs);

	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceDriverProperties drv = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
		VkPhysicalDeviceProperties2 props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
		LOAD(inst, vkGetPhysicalDeviceProperties2)(devs[i], &props);
		printf("=== %s (%s %s, Vulkan %u.%u.%u)\n", props.properties.deviceName,
			drv.driverName, drv.driverInfo,
			VK_API_VERSION_MAJOR(props.properties.apiVersion),
			VK_API_VERSION_MINOR(props.properties.apiVersion),
			VK_API_VERSION_PATCH(props.properties.apiVersion));

		uint32_t qn = 0;
		LOAD(inst, vkGetPhysicalDeviceQueueFamilyProperties2)(devs[i], &qn, NULL);
		VkQueueFamilyProperties2 *qf = calloc(qn, sizeof(*qf));
		VkQueueFamilyVideoPropertiesKHR *qv = calloc(qn, sizeof(*qv));
		for (uint32_t q = 0; q < qn; q++) {
			qv[q].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
			qf[q].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
			qf[q].pNext = &qv[q];
		}
		LOAD(inst, vkGetPhysicalDeviceQueueFamilyProperties2)(devs[i], &qn, qf);
		for (uint32_t q = 0; q < qn; q++) {
			VkQueueFlags f = qf[q].queueFamilyProperties.queueFlags;
			printf("  family %u: %u queue%s%s%s%s%s%s%s\n", q,
				qf[q].queueFamilyProperties.queueCount,
				qf[q].queueFamilyProperties.queueCount == 1 ? "" : "s",
				f & VK_QUEUE_GRAPHICS_BIT ? " graphics" : "",
				f & VK_QUEUE_COMPUTE_BIT ? " compute" : "",
				f & VK_QUEUE_TRANSFER_BIT ? " transfer" : "",
				f & VK_QUEUE_VIDEO_ENCODE_BIT_KHR ? " video-encode" : "",
				f & VK_QUEUE_VIDEO_DECODE_BIT_KHR ? " video-decode" : "",
				qv[q].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR
					? " [encodes av1]" : "");
		}
		free(qf);
		free(qv);

		uint32_t en = 0;
		LOAD(inst, vkEnumerateDeviceExtensionProperties)(devs[i], NULL, &en, NULL);
		VkExtensionProperties *ext = calloc(en, sizeof(*ext));
		LOAD(inst, vkEnumerateDeviceExtensionProperties)(devs[i], NULL, &en, ext);
		printf("  extensions we need:\n");
		for (const char **w = wanted; *w != NULL; w++) {
			bool have = false;
			for (uint32_t e = 0; e < en; e++) {
				have |= strcmp(ext[e].extensionName, *w) == 0;
			}
			printf("    %-50s %s\n", *w, have ? "yes" : "MISSING");
		}
		free(ext);

		print_modifiers(devs[i], inst, VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM");
		print_modifiers(devs[i], inst, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
			"A2B10G10R10_UNORM_PACK32 (gamescope's XB30)");
		av1_encode_caps(devs[i], inst, false, 8);
		av1_encode_caps(devs[i], inst, true, 8);
		av1_encode_caps(devs[i], inst, true, 10);
		av1_encode_caps_sub(devs[i], inst, false, 8,
			VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR, "4:4:4");
		av1_encode_caps_sub(devs[i], inst, false, 8,
			VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR, "monochrome");
		printf("\n");
	}
	free(devs);
	return 0;
}
