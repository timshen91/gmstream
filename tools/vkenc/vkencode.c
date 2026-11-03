// Encodes frames of a test pattern with the Vulkan AV1 encoder, straight from
// an RGB image (VK_VALVE_video_encode_rgb_conversion, i.e. VCN's own format
// conversion), and writes the bitstream out. This is the experiment the Vulkan
// encode path rests on: it settles the session setup, the explicit tile
// spacing this driver demands, where the sequence header comes from, and what
// the feedback query reports. See DESIGN.md §12.
//
// Build in the gmstream build image (build/Containerfile):
//   gcc -O2 -o vkencode vkencode.c -I/opt/gm/include -ldl
// Run on the device:
//   ./vkencode [-s WxH] [-n frames] [-t COLSxROWS] [-b kbit/s] out.obu
//
// What it settled on this hardware (Radeon 8060S, radv Mesa 26.2.0-devel):
// - The encoder takes the BGRA image directly and converts it itself, so
//   gamescope's buffer needs no blit and no NV12 pool.
// - 1x4 tiles are honoured (same content, same q: 11649 bytes with them,
//   10997 without), although FFmpeg's own Vulkan encoder does not pass tile
//   info at all yet.
// - CBR rate control hangs the encode ring, every time, after a handful of
//   frames ("ring vcn_unified_0 timeout"; the kernel resets it and recovers).
//   VBR and rate control off (a constant q index) are both solid. So the
//   bitrate has to be held by our own choice of q per frame, or by VBR.
// - The encode operation emits only the frame OBU. The temporal delimiter is
//   ours to write, and the sequence header comes from
//   vkGetEncodedVideoSessionParametersKHR (19 bytes). Both are known before
//   the encode, which suits streaming: they are the frame's first bytes.
// - Two DPB slots have to take turns. Encoding into the slot being referenced
//   hangs the ring as well.
// - dav1d decodes what comes out, 12 of 12 frames, tiled and not.
// - 1920x1088 (1080 rounded up to the 64x16 coded alignment) takes 1.2-1.7 ms
//   per inter frame, 5-8 ms for the first keyframe.
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>

#define CHECK(expr) do { \
	VkResult _r = (expr); \
	if (_r != VK_SUCCESS) { \
		fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #expr, _r); \
		exit(1); \
	} \
} while (0)

static PFN_vkGetInstanceProcAddr getproc;
static VkInstance inst;
static VkDevice dev;
static VkPhysicalDevice phys;
#define IPROC(name) ((PFN_##name)getproc(inst, #name))
static PFN_vkVoidFunction dproc_(const char *name) {
	static PFN_vkGetDeviceProcAddr get;
	if (get == NULL) {
		get = IPROC(vkGetDeviceProcAddr);
	}
	PFN_vkVoidFunction f = get(dev, name);
	if (f == NULL) {
		fprintf(stderr, "no %s\n", name);
		exit(1);
	}
	return f;
}
#define DPROC(name) ((PFN_##name)dproc_(#name))

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

static uint32_t align_up(uint32_t v, uint32_t a) {
	return (v + a - 1) / a * a;
}

static uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	IPROC(vkGetPhysicalDeviceMemoryProperties)(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((mask & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	fprintf(stderr, "no memory type for %#x / %#x\n", mask, want);
	exit(1);
}

/* ---- the profile ---------------------------------------------------------- */

// One profile, chained once and pointed at from everywhere: the AV1 encode
// profile with the VALVE RGB conversion asked for, so the encoder takes our
// BGRA image and converts it itself.
static VkVideoEncodeProfileRgbConversionInfoVALVE rgb_profile = {
	.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_PROFILE_RGB_CONVERSION_INFO_VALVE,
	.performEncodeRgbConversion = VK_TRUE };
static VkVideoEncodeUsageInfoKHR usage_info = {
	.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR,
	.pNext = &rgb_profile,
	.videoUsageHints = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR,
	.videoContentHints = VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR,
	.tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR };
static VkVideoEncodeAV1ProfileInfoKHR av1_profile = {
	.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR,
	.pNext = &usage_info, .stdProfile = STD_VIDEO_AV1_PROFILE_MAIN };
static VkVideoProfileInfoKHR profile = {
	.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, .pNext = &av1_profile,
	.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
	.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
	.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
	.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR };
static VkVideoProfileListInfoKHR profile_list = {
	.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
	.profileCount = 1, .pProfiles = &profile };

/* ---- setup ---------------------------------------------------------------- */

static uint32_t find_device_and_queue(void) {
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkencode", .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	CHECK(((PFN_vkCreateInstance)getproc(NULL, "vkCreateInstance"))(&ici, NULL, &inst));

	uint32_t n = 0;
	IPROC(vkEnumeratePhysicalDevices)(inst, &n, NULL);
	VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
	IPROC(vkEnumeratePhysicalDevices)(inst, &n, devs);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t qn = 0;
		IPROC(vkGetPhysicalDeviceQueueFamilyProperties2)(devs[i], &qn, NULL);
		VkQueueFamilyProperties2 *qf = calloc(qn, sizeof(*qf));
		VkQueueFamilyVideoPropertiesKHR *qv = calloc(qn, sizeof(*qv));
		for (uint32_t q = 0; q < qn; q++) {
			qv[q].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
			qf[q].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
			qf[q].pNext = &qv[q];
		}
		IPROC(vkGetPhysicalDeviceQueueFamilyProperties2)(devs[i], &qn, qf);
		for (uint32_t q = 0; q < qn; q++) {
			if ((qf[q].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) &&
					(qv[q].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR)) {
				phys = devs[i];
				free(qf);
				free(qv);
				free(devs);
				return q;
			}
		}
		free(qf);
		free(qv);
	}
	fprintf(stderr, "no AV1 encode queue\n");
	exit(1);
}

static void create_device(uint32_t family) {
	float prio = 1.0f;
	VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio };
	const char *exts[] = {
		VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
		VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME,
		VK_VALVE_VIDEO_ENCODE_RGB_CONVERSION_EXTENSION_NAME,
	};
	VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE rgb = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_RGB_CONVERSION_FEATURES_VALVE,
		.videoEncodeRgbConversion = VK_TRUE };
	VkPhysicalDeviceSynchronization2Features sync2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
		.pNext = &rgb, .synchronization2 = VK_TRUE };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &sync2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
		.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]),
		.ppEnabledExtensionNames = exts };
	CHECK(IPROC(vkCreateDevice)(phys, &dci, NULL, &dev));
}

struct image {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	void *map;
	VkSubresourceLayout layout;
};

static struct image create_image(VkFormat format, uint32_t w, uint32_t h,
		uint32_t layers, VkImageUsageFlags usage, bool host, uint32_t family) {
	struct image im = { 0 };
	VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &profile_list, .imageType = VK_IMAGE_TYPE_2D, .format = format,
		.extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = layers,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = host ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
		.usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 1, .pQueueFamilyIndices = &family,
		.initialLayout = host ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED };
	CHECK(DPROC(vkCreateImage)(dev, &ici, NULL, &im.image));

	VkMemoryRequirements mr;
	DPROC(vkGetImageMemoryRequirements)(dev, im.image, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = memory_type(mr.memoryTypeBits, host
			? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
			: VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	CHECK(DPROC(vkAllocateMemory)(dev, &mai, NULL, &im.memory));
	CHECK(DPROC(vkBindImageMemory)(dev, im.image, im.memory, 0));

	VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = im.image,
		.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1, .layerCount = layers } };
	CHECK(DPROC(vkCreateImageView)(dev, &vci, NULL, &im.view));

	if (host) {
		VkImageSubresource sub = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
		DPROC(vkGetImageSubresourceLayout)(dev, im.image, &sub, &im.layout);
		CHECK(DPROC(vkMapMemory)(dev, im.memory, 0, VK_WHOLE_SIZE, 0, &im.map));
	}
	return im;
}

/* ---- the AV1 syntax we hand over ------------------------------------------ */

static StdVideoAV1ColorConfig color_config = {
	.flags = { .color_range = 1, .color_description_present_flag = 1 },
	.BitDepth = 8, .subsampling_x = 1, .subsampling_y = 1,
	.color_primaries = STD_VIDEO_AV1_COLOR_PRIMARIES_BT_709,
	.transfer_characteristics = STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_BT_709,
	.matrix_coefficients = STD_VIDEO_AV1_MATRIX_COEFFICIENTS_BT_709,
	.chroma_sample_position = STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_UNKNOWN };

static StdVideoAV1SequenceHeader sequence_header(uint32_t w, uint32_t h) {
	StdVideoAV1SequenceHeader sh = {
		.flags = { .enable_order_hint = 1, .enable_cdef = 1,
			.enable_intra_edge_filter = 1 },
		.seq_profile = STD_VIDEO_AV1_PROFILE_MAIN,
		.frame_width_bits_minus_1 = 15, .frame_height_bits_minus_1 = 15,
		.max_frame_width_minus_1 = (uint16_t)(w - 1),
		.max_frame_height_minus_1 = (uint16_t)(h - 1),
		.order_hint_bits_minus_1 = 6,
		.pColorConfig = &color_config };
	return sh;
}

// Tiles, spelled out: this driver reports no std syntax flags, so
// uniform_tile_spacing_flag cannot be set and every tile's start and size has
// to be given. Superblocks are 64 px (16 mi units) on this hardware.
#define MAX_TILES 16
struct tiles {
	StdVideoAV1TileInfo info;
	uint16_t col_starts[MAX_TILES + 1], row_starts[MAX_TILES + 1];
	uint16_t col_sizes[MAX_TILES], row_sizes[MAX_TILES];
};

static void make_tiles(struct tiles *t, uint32_t w, uint32_t h,
		uint32_t cols, uint32_t rows) {
	uint32_t sb_cols = align_up(w, 64) / 64, sb_rows = align_up(h, 64) / 64;
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

// The source and reconstruction formats, eight-bit unless -d 10.
static VkFormat src_format = VK_FORMAT_B8G8R8A8_UNORM;
static VkFormat dpb_format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

/* ---- the test pattern ----------------------------------------------------- */

// A gradient with a moving block, so every frame differs from the last and an
// inter frame has something to predict.
static void paint(struct image *im, uint32_t w, uint32_t h, uint32_t frame) {
	if (src_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
		uint32_t bx = (frame * 37) % (w > 256 ? w - 256 : 1);
		uint32_t by = (frame * 23) % (h > 256 ? h - 256 : 1);
		for (uint32_t y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)((uint8_t *)im->map + im->layout.offset +
				y * im->layout.rowPitch);
			for (uint32_t x = 0; x < w; x++) {
				bool in_block = x >= bx && x < bx + 256 && y >= by && y < by + 256;
				uint32_t b = in_block ? 960 : x * 1023 / w;
				uint32_t g = in_block ? 240 : y * 1023 / h;
				uint32_t r = in_block ? 240 : (x + y) * 1023 / (w + h);
				row[x] = (3u << 30) | (b << 20) | (g << 10) | r;
			}
		}
		return;
	}
	uint32_t bx = (frame * 37) % (w > 256 ? w - 256 : 1);
	uint32_t by = (frame * 23) % (h > 256 ? h - 256 : 1);
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = (uint8_t *)im->map + im->layout.offset + y * im->layout.rowPitch;
		for (uint32_t x = 0; x < w; x++) {
			bool in_block = x >= bx && x < bx + 256 && y >= by && y < by + 256;
			row[x * 4 + 0] = in_block ? 240 : (uint8_t)(x * 255 / w);       // B
			row[x * 4 + 1] = in_block ? 60 : (uint8_t)(y * 255 / h);        // G
			row[x * 4 + 2] = in_block ? 60 : (uint8_t)((x + y) * 255 / (w + h)); // R
			row[x * 4 + 3] = 255;
		}
	}
}

int main(int argc, char **argv) {
	uint32_t want_w = 1920, want_h = 1080, frames = 8, cols = 1, rows = 4;
	uint32_t bitrate_kbps = 50000, const_q = 0, gop = 60, key_period = 0, depth = 8;
	const char *mode = "cbr";
	const char *out_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%ux%u", &want_w, &want_h);
		} else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			frames = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%ux%u", &cols, &rows);
		} else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
			bitrate_kbps = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
			const_q = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			depth = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
			mode = argv[++i];
		} else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
			gop = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
			key_period = (uint32_t)atoi(argv[++i]);
		} else {
			out_path = argv[i];
		}
	}
	if (out_path == NULL) {
		fprintf(stderr, "usage: vkencode [-s WxH] [-n frames] [-t COLSxROWS] "
			"[-b kbit/s] [-q qindex] [-m cbr|vbr|off] [-g gop] [-k keyperiod] "
			"out.obu\n");
		return 1;
	}

	if (depth == 10) {
		profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
		profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
		src_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		dpb_format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
	}
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (lib == NULL) {
		fprintf(stderr, "libvulkan.so.1: %s\n", dlerror());
		return 1;
	}
	getproc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	uint32_t family = find_device_and_queue();
	create_device(family);

	// Capabilities decide the coded size and the DPB.
	printf("input %u-bit\n", depth);
	VkVideoEncodeAV1CapabilitiesKHR av1_caps = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR };
	VkVideoEncodeCapabilitiesKHR enc_caps = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR, .pNext = &av1_caps };
	VkVideoCapabilitiesKHR caps = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, .pNext = &enc_caps };
	CHECK(IPROC(vkGetPhysicalDeviceVideoCapabilitiesKHR)(phys, &profile, &caps));

	uint32_t w = align_up(want_w, av1_caps.codedPictureAlignment.width);
	uint32_t h = align_up(want_h, av1_caps.codedPictureAlignment.height);
	printf("coded %ux%u for %ux%u, tiles %ux%u, dpb %u slots\n",
		w, h, want_w, want_h, cols, rows, caps.maxDpbSlots);

	// Input: a linear BGRA image we paint with the CPU (the real path imports
	// gamescope's dma-buf instead). DPB: NV12, two slots in one image, because
	// this driver does not support separate reference images.
	struct image src = create_image(src_format, w, h, 1,
		VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, true, family);
	struct image dpb = create_image(dpb_format, w, h, 2,
		VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, false, family);

	// The session, and the RGB conversion it should do on the way in.
	VkVideoEncodeSessionRgbConversionCreateInfoVALVE rgb_create = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_RGB_CONVERSION_CREATE_INFO_VALVE,
		.rgbModel = VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_709_BIT_VALVE,
		.rgbRange = VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_FULL_RANGE_BIT_VALVE,
		.xChromaOffset = VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_COSITED_EVEN_BIT_VALVE,
		.yChromaOffset = VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_COSITED_EVEN_BIT_VALVE };
	VkVideoSessionCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
		.pNext = &rgb_create, .queueFamilyIndex = family, .pVideoProfile = &profile,
		.pictureFormat = src_format,
		.maxCodedExtent = { w, h },
		.referencePictureFormat = dpb_format,
		.maxDpbSlots = 2, .maxActiveReferencePictures = 1,
		.pStdHeaderVersion = &caps.stdHeaderVersion };
	VkVideoSessionKHR session;
	CHECK(DPROC(vkCreateVideoSessionKHR)(dev, &sci, NULL, &session));

	uint32_t binds = 0;
	CHECK(DPROC(vkGetVideoSessionMemoryRequirementsKHR)(dev, session, &binds, NULL));
	VkVideoSessionMemoryRequirementsKHR *reqs = calloc(binds, sizeof(*reqs));
	for (uint32_t i = 0; i < binds; i++) {
		reqs[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	}
	CHECK(DPROC(vkGetVideoSessionMemoryRequirementsKHR)(dev, session, &binds, reqs));
	VkBindVideoSessionMemoryInfoKHR *bindinfo = calloc(binds, sizeof(*bindinfo));
	for (uint32_t i = 0; i < binds; i++) {
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = reqs[i].memoryRequirements.size,
			.memoryTypeIndex = memory_type(reqs[i].memoryRequirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
		VkDeviceMemory mem;
		CHECK(DPROC(vkAllocateMemory)(dev, &mai, NULL, &mem));
		bindinfo[i] = (VkBindVideoSessionMemoryInfoKHR){
			.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
			.memoryBindIndex = reqs[i].memoryBindIndex, .memory = mem,
			.memorySize = reqs[i].memoryRequirements.size };
	}
	CHECK(DPROC(vkBindVideoSessionMemoryKHR)(dev, session, binds, bindinfo));
	printf("session memory: %u bind%s\n", binds, binds == 1 ? "" : "s");

	StdVideoAV1SequenceHeader seq = sequence_header(w, h);
	VkVideoEncodeAV1SessionParametersCreateInfoKHR av1_params = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR,
		.pStdSequenceHeader = &seq };
	VkVideoEncodeQualityLevelInfoKHR quality = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,
		.pNext = &av1_params, .qualityLevel = 0 };
	VkVideoSessionParametersCreateInfoKHR pci = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
		.pNext = &quality, .videoSession = session };
	VkVideoSessionParametersKHR params;
	CHECK(DPROC(vkCreateVideoSessionParametersKHR)(dev, &pci, NULL, &params));

	// The sequence header OBU: the encode operation does not emit it, so it is
	// ours to put in front of every keyframe.
	VkVideoEncodeSessionParametersGetInfoKHR gi = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
		.videoSessionParameters = params };
	VkVideoEncodeSessionParametersFeedbackInfoKHR fb = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR };
	size_t seq_size = 0;
	CHECK(DPROC(vkGetEncodedVideoSessionParametersKHR)(dev, &gi, &fb, &seq_size, NULL));
	uint8_t *seq_obu = malloc(seq_size);
	CHECK(DPROC(vkGetEncodedVideoSessionParametersKHR)(dev, &gi, &fb, &seq_size, seq_obu));
	printf("sequence header: %zu bytes%s\n", seq_size,
		fb.hasOverrides ? " (driver overrode syntax)" : "");

	// Where the bitstream lands: host-visible, so the real path can watch it
	// being written.
	VkDeviceSize bs_size = 8u << 20;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = &profile_list, .size = bs_size,
		.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 1, .pQueueFamilyIndices = &family };
	VkBuffer bs;
	CHECK(DPROC(vkCreateBuffer)(dev, &bci, NULL, &bs));
	VkMemoryRequirements bmr;
	DPROC(vkGetBufferMemoryRequirements)(dev, bs, &bmr);
	VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = bmr.size,
		.memoryTypeIndex = memory_type(bmr.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VkDeviceMemory bs_mem;
	CHECK(DPROC(vkAllocateMemory)(dev, &bmai, NULL, &bs_mem));
	CHECK(DPROC(vkBindBufferMemory)(dev, bs, bs_mem, 0));
	uint8_t *bs_map = NULL;
	CHECK(DPROC(vkMapMemory)(dev, bs_mem, 0, VK_WHOLE_SIZE, 0, (void **)&bs_map));

	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR qfi = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
		.pNext = &profile,
		.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
			VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR };
	VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.pNext = &qfi, .queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR,
		.queryCount = 1 };
	VkQueryPool queries;
	CHECK(DPROC(vkCreateQueryPool)(dev, &qpi, NULL, &queries));

	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = family };
	VkCommandPool pool;
	CHECK(DPROC(vkCreateCommandPool)(dev, &cpi, NULL, &pool));
	VkCommandBufferAllocateInfo cbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CHECK(DPROC(vkAllocateCommandBuffers)(dev, &cbi, &cmd));
	VkQueue queue;
	DPROC(vkGetDeviceQueue)(dev, family, 0, &queue);
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(DPROC(vkCreateFence)(dev, &fci, NULL, &fence));

	struct tiles tiles;
	make_tiles(&tiles, w, h, cols, rows);
	StdVideoAV1Quantization quant = { .base_q_idx = (uint8_t)(const_q > 0 ? const_q : 128) };
	StdVideoAV1LoopFilter lf = { 0 };
	StdVideoAV1CDEF cdef = { .cdef_damping_minus_3 = 0, .cdef_bits = 0 };
	StdVideoAV1LoopRestoration lr = { .FrameRestorationType = {
		STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE,
		STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE,
		STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE } };
	StdVideoAV1GlobalMotion gm = { 0 };

	FILE *out = fopen(out_path, "wb");
	if (out == NULL) {
		perror(out_path);
		return 1;
	}

	// The reference name the hardware will take for single-reference
	// prediction (the lowest it allows; LAST_FRAME on this one).
	uint32_t ref_name = 0;
	for (uint32_t i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; i++) {
		if (av1_caps.singleReferenceNameMask & (1u << i)) {
			ref_name = i;
			break;
		}
	}
	printf("single-reference name mask %#x, using name %u\n",
		av1_caps.singleReferenceNameMask, ref_name);

	bool dpb_ready = false, src_first = true;
	uint32_t ref_slot_index = 0;
	uint64_t total = 0;
	for (uint32_t f = 0; f < frames; f++) {
		bool key = f == 0;
		paint(&src, w, h, f);

		CHECK(DPROC(vkResetCommandBuffer)(cmd, 0));
		VkCommandBufferBeginInfo bi = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		CHECK(DPROC(vkBeginCommandBuffer)(cmd, &bi));

		VkImageMemoryBarrier2 barriers[2];
		uint32_t nbarriers = 0;
		barriers[nbarriers++] = (VkImageMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
			.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
			.oldLayout = src_first ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
			.image = src.image,
			.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1, .layerCount = 1 } };
		if (!dpb_ready) {
			barriers[nbarriers++] = (VkImageMemoryBarrier2){
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
				.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
				.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
				.image = dpb.image,
				.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.levelCount = 1, .layerCount = 2 } };
		}
		VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = nbarriers, .pImageMemoryBarriers = barriers };
		DPROC(vkCmdPipelineBarrier2)(cmd, &dep);
		src_first = false;
		dpb_ready = true;

		DPROC(vkCmdResetQueryPool)(cmd, queries, 0, 1);

		// Two DPB slots, taking turns: the frame being encoded reconstructs
		// into one while the last frame is read from the other. Writing the
		// slot it is also referencing hangs the encoder.
		uint32_t slot = key ? 0 : 1 - ref_slot_index;
		VkVideoPictureResourceInfoKHR setup_res = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
			.codedExtent = { w, h }, .baseArrayLayer = slot,
			.imageViewBinding = dpb.view };
		VkVideoPictureResourceInfoKHR ref_res = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
			.codedExtent = { w, h }, .baseArrayLayer = ref_slot_index,
			.imageViewBinding = dpb.view };
		StdVideoEncodeAV1ReferenceInfo setup_std = {
			.RefFrameId = slot,
			.frame_type = key ? STD_VIDEO_AV1_FRAME_TYPE_KEY : STD_VIDEO_AV1_FRAME_TYPE_INTER,
			.OrderHint = (uint8_t)f };
		VkVideoEncodeAV1DpbSlotInfoKHR setup_av1 = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR,
			.pStdReferenceInfo = &setup_std };
		VkVideoReferenceSlotInfoKHR setup = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
			.pNext = &setup_av1, .slotIndex = (int32_t)slot,
			.pPictureResource = &setup_res };
		StdVideoEncodeAV1ReferenceInfo ref_std = {
			.RefFrameId = ref_slot_index,
			.frame_type = f == 1 ? STD_VIDEO_AV1_FRAME_TYPE_KEY : STD_VIDEO_AV1_FRAME_TYPE_INTER,
			.OrderHint = (uint8_t)(f - 1) };
		VkVideoEncodeAV1DpbSlotInfoKHR ref_av1 = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR,
			.pStdReferenceInfo = &ref_std };
		VkVideoReferenceSlotInfoKHR ref = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
			.pNext = &ref_av1, .slotIndex = (int32_t)ref_slot_index,
			.pPictureResource = &ref_res };
		// Begin-coding declares every slot the frame touches: the references
		// by index, and the frame's own output slot as -1 (not yet in use).
		VkVideoReferenceSlotInfoKHR begin_slots[2];
		uint32_t nslots = 0;
		if (!key) {
			begin_slots[nslots++] = ref;
		}
		begin_slots[nslots] = setup;
		begin_slots[nslots].slotIndex = -1;
		begin_slots[nslots].pNext = NULL;
		nslots++;
		VkVideoBeginCodingInfoKHR begin = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
			.videoSession = session, .videoSessionParameters = params,
			.referenceSlotCount = nslots, .pReferenceSlots = begin_slots };
		DPROC(vkCmdBeginVideoCodingKHR)(cmd, &begin);

		if (f == 0) {
			VkVideoEncodeAV1RateControlLayerInfoKHR av1_layer = {
				.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_LAYER_INFO_KHR };
			VkVideoEncodeRateControlLayerInfoKHR layer = {
				.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR,
				.pNext = &av1_layer,
				.averageBitrate = (uint64_t)bitrate_kbps * 1000,
				.maxBitrate = (uint64_t)bitrate_kbps * 1000,
				.frameRateNumerator = 60, .frameRateDenominator = 1 };
			VkVideoEncodeAV1RateControlInfoKHR av1_rc = {
				.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR,
				.gopFrameCount = gop, .keyFramePeriod = key_period,
				.temporalLayerCount = 1 };
			// Constant q (rate control off) or CBR, so the two can be told
			// apart: this hardware hangs its encode ring on some combinations.
			bool off = strcmp(mode, "off") == 0 || const_q > 0;
			VkVideoEncodeRateControlInfoKHR rc = off ? (VkVideoEncodeRateControlInfoKHR){
					.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
					.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR,
				} : (VkVideoEncodeRateControlInfoKHR){
					.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
					.pNext = &av1_rc,
					.rateControlMode = strcmp(mode, "vbr") == 0
						? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR
						: VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR,
					.layerCount = 1, .pLayers = &layer,
					.virtualBufferSizeInMs = 100,
					.initialVirtualBufferSizeInMs = 50 };
			printf("rate control %s, gop %u, key period %u\n",
				off ? "off (constant q)" : mode, gop, key_period);
			VkVideoEncodeQualityLevelInfoKHR ql = {
				.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,
				.pNext = &rc, .qualityLevel = 0 };
			VkVideoCodingControlInfoKHR control = {
				.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
				.pNext = &ql,
				.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR |
					VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR |
					VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR };
			DPROC(vkCmdControlVideoCodingKHR)(cmd, &control);
		}

		StdVideoEncodeAV1PictureInfo std_pic = {
			.flags = { .render_and_frame_size_different = (w != want_w || h != want_h),
				.show_frame = 1, .showable_frame = !key },
			.frame_type = key ? STD_VIDEO_AV1_FRAME_TYPE_KEY : STD_VIDEO_AV1_FRAME_TYPE_INTER,
			.current_frame_id = slot,
			.order_hint = (uint8_t)f,
			.primary_ref_frame = key ? STD_VIDEO_AV1_PRIMARY_REF_NONE : ref_name,
			.refresh_frame_flags = key ? 0xff : (uint8_t)(1u << slot),
			.render_width_minus_1 = (uint16_t)(want_w - 1),
			.render_height_minus_1 = (uint16_t)(want_h - 1),
			.interpolation_filter = STD_VIDEO_AV1_INTERPOLATION_FILTER_EIGHTTAP,
			.TxMode = STD_VIDEO_AV1_TX_MODE_SELECT,
			.pTileInfo = &tiles.info, .pQuantization = &quant,
			.pLoopFilter = &lf, .pCDEF = &cdef, .pLoopRestoration = &lr,
			.pGlobalMotion = &gm };
		// Every reference name points at the one frame we keep.
		for (int i = 0; i < STD_VIDEO_AV1_REFS_PER_FRAME; i++) {
			std_pic.ref_frame_idx[i] = key ? -1 : (int8_t)ref_slot_index;
		}
		if (!key) {
			std_pic.ref_order_hint[ref_slot_index] = (uint8_t)(f - 1);
		}
		VkVideoEncodeAV1PictureInfoKHR av1_pic = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_KHR,
			.predictionMode = key ? VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_INTRA_ONLY_KHR
				: VK_VIDEO_ENCODE_AV1_PREDICTION_MODE_SINGLE_REFERENCE_KHR,
			.rateControlGroup = key ? VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_INTRA_KHR
				: VK_VIDEO_ENCODE_AV1_RATE_CONTROL_GROUP_PREDICTIVE_KHR,
			.constantQIndex = const_q > 0 ? const_q : 128,
			.pStdPictureInfo = &std_pic };
		// Only the reference name the hardware allows carries the slot.
		for (int i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; i++) {
			av1_pic.referenceNameSlotIndices[i] =
				!key && i == ref_name ? (int32_t)ref_slot_index : -1;
		}
		VkVideoPictureResourceInfoKHR src_res = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
			.codedExtent = { w, h }, .imageViewBinding = src.view };
		VkVideoEncodeInfoKHR info = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR, .pNext = &av1_pic,
			.dstBuffer = bs, .dstBufferOffset = 0, .dstBufferRange = bs_size,
			.srcPictureResource = src_res,
			.pSetupReferenceSlot = &setup,
			.referenceSlotCount = key ? 0 : 1,
			.pReferenceSlots = key ? NULL : &ref };

		DPROC(vkCmdBeginQuery)(cmd, queries, 0, 0);
		DPROC(vkCmdEncodeVideoKHR)(cmd, &info);
		DPROC(vkCmdEndQuery)(cmd, queries, 0);

		VkVideoEndCodingInfoKHR end = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
		DPROC(vkCmdEndVideoCodingKHR)(cmd, &end);

		VkImageMemoryBarrier2 back = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
			.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
			.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
			.oldLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.image = src.image,
			.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1, .layerCount = 1 } };
		VkDependencyInfo dep2 = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back };
		DPROC(vkCmdPipelineBarrier2)(cmd, &dep2);
		CHECK(DPROC(vkEndCommandBuffer)(cmd));

		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1, .pCommandBuffers = &cmd };
		uint64_t t0 = now_ns();
		CHECK(DPROC(vkResetFences)(dev, 1, &fence));
		CHECK(DPROC(vkQueueSubmit)(queue, 1, &si, fence));
		CHECK(DPROC(vkWaitForFences)(dev, 1, &fence, VK_TRUE, 1000000000));
		uint64_t t1 = now_ns();

		uint32_t result[2] = { 0, 0 };
		VkResult qr = DPROC(vkGetQueryPoolResults)(dev, queries, 0, 1, sizeof(result),
			result, sizeof(result), VK_QUERY_RESULT_WAIT_BIT);
		if (qr != VK_SUCCESS) {
			fprintf(stderr, "frame %u: query result %d\n", f, qr);
			return 1;
		}
		uint32_t offset = result[0], size = result[1];
		// The encode operation emits only the frame OBU: a temporal delimiter
		// in front of each temporal unit is ours to write, and so is the
		// sequence header on a keyframe. Both are known before the encode, so
		// in the streaming path they are the frame's first bytes on the wire.
		static const uint8_t td[2] = { 0x12, 0x00 };
		fwrite(td, 1, sizeof(td), out);
		total += sizeof(td);
		if (key && seq_size > 0) {
			fwrite(seq_obu, 1, seq_size, out);
			total += seq_size;
		}
		fwrite(bs_map + offset, 1, size, out);
		total += size;
		printf("frame %u %-5s %7u bytes at %u, %.2f ms, slot %u ref %d\n", f,
			key ? "key" : "inter", size, offset, (t1 - t0) / 1e6, slot,
			key ? -1 : (int)ref_slot_index);
		ref_slot_index = slot;
	}
	fclose(out);
	printf("wrote %s, %llu bytes\n", out_path, (unsigned long long)total);
	return 0;
}
