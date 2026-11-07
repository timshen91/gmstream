// The scanout compositor (compositor.h). Raw Vulkan on Granite's device: the
// compute queue is ours (Granite only aliases its transfers onto it), taken
// under the device's queue lock.

#include "compositor.h"
#include "gpu.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <ctime>

#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "conn.h" // now_ns
#include "log.h"

namespace gm {

static const uint32_t compose_comp[] = {
#include "shaders/compose_comp.inc"
};
static const uint32_t compose_ycbcr_comp[] = {
#include "shaders/compose_ycbcr_comp.inc"
};

static constexpr int ring_count = 3;
static constexpr int job_count = 4;

// When a sync fd signalled (CLOCK_MONOTONIC), waiting up to wait_ms; 0 if not.
static uint64_t fence_time(int fd, int wait_ms)
{
	if (fd < 0)
		return 0;
	pollfd p{fd, POLLIN, 0};
	if (poll(&p, 1, wait_ms) <= 0)
		return 0;
	sync_file_info info{};
	if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.num_fences == 0)
		return now_ns();
	std::vector<sync_fence_info> fences(info.num_fences);
	info.sync_fence_info = reinterpret_cast<uint64_t>(fences.data());
	if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0)
		return now_ns();
	uint64_t t = 0;
	for (auto &f : fences)
		if (f.status == 1 && f.timestamp_ns > t)
			t = f.timestamp_ns;
	return t ? t : now_ns();
}

static bool debug_stream()
{
	char v[PROP_VALUE_MAX] = {};
	return __system_property_get("debug.gmstream.stream", v) > 0 && v[0] == '1';
}

static void sleep_until(uint64_t t)
{
	timespec ts{time_t(t / 1000000000ull), long(t % 1000000000ull)};
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
	}
}

struct PushConstants {
	int32_t size[2], origin[2];
	float scale[2];
	int32_t rotation, pad;
};

struct Compositor::Impl {
	Compositor &owner;
	Presenter &presenter;
	Gpu gpu_store;
	Gpu *gpu = &gpu_store;
	VkDevice vk = VK_NULL_HANDLE;

	// A picture handed in.
	struct Source {
		uint64_t id = 0;
		bool image = false;
		VkImageView planes[3] = {};
		AHardwareBuffer *ahb = nullptr;
		int w = 0, h = 0; // planes: the picture; image: the crop
		int fence = -1;
		uint64_t ready = 0;
		FrameTiming t;
		std::function<void()> release;
		int uses = 0;          // composes in flight reading it
		bool sampled = false;  // a sample point took it
		bool emitted = false;  // told shown / torn / dropped
	};
	std::mutex src_lock;
	std::deque<std::shared_ptr<Source>> sources; // oldest first
	uint64_t next_id = 0;

	// The ring's buffers.
	struct Out {
		AHardwareBuffer *ahb = nullptr;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		uint64_t content = 0; // the source it holds
	};
	Out outs[ring_count];
	int out_w = 0, out_h = 0, rotation = 0, pic_w = 0, pic_h = 0;
	bool turned = false; // the picture comes already turned to the panel (Riptide, rotation asked of the host)

	// The video decoder's buffers, imported once each.
	struct Imported {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		uint64_t external_format = 0;
		uint32_t coded_w = 0, coded_h = 0;
	};
	std::map<AHardwareBuffer *, Imported> imported;
	uint64_t ycbcr_format = ~0ull; // what the conversion below was made for
	VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
	VkSampler ycbcr_sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout image_layout = VK_NULL_HANDLE;
	VkPipelineLayout image_pl = VK_NULL_HANDLE;
	VkPipeline image_pipe = VK_NULL_HANDLE;
	VkDescriptorPool image_pool = VK_NULL_HANDLE;

	VkSampler linear = VK_NULL_HANDLE;
	VkDescriptorSetLayout planes_layout = VK_NULL_HANDLE;
	VkPipelineLayout planes_pl = VK_NULL_HANDLE;
	VkPipeline planes_pipe = VK_NULL_HANDLE;
	VkDescriptorPool planes_pool = VK_NULL_HANDLE;
	VkCommandPool cmd_pool = VK_NULL_HANDLE;
	bool objects = false;

	struct Job {
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkSemaphore wait = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
		VkDescriptorSet planes_set = VK_NULL_HANDLE, image_set = VK_NULL_HANDLE;
		bool busy = false;
		std::shared_ptr<Source> src;
	};
	Job jobs[job_count];
	int next_job = 0;
	std::mutex gpu_lock; // the jobs and the GPU objects

	// Composes, by the commit they were for, until its present is known.
	struct Record {
		std::shared_ptr<Source> src;
		uint64_t sample = 0, woke = 0, submitted = 0;
		int done_fd = -1;
	};
	std::mutex rec_lock;
	std::map<uint64_t, Record> records;

	// Timing of the composes: sample point -> done.
	std::vector<double> lat, wake;
	double budget = 1.5e6;
	int composes = 0, skipped_late = 0;
	std::atomic<int> torn{0};
	std::vector<double> window_lat; // since the last summary

	// Streaming (stream_stripe): a frame's stripes into the buffer of the
	// refresh claimed for it, each as it is decoded.
	struct Streamed {
		uint32_t frame = 0;
		uint64_t seq = 0;      // the commit (refresh) it goes out with
		int buffer = -1;
		uint64_t cutoff = 0;   // that refresh's predicted prefetch start
		uint64_t first = 0;    // stripe 0 handed in
		double first_est = 0;  // what claiming it allowed stripe 0 (from being handed in to composed)
		std::vector<int> ys, done_fds; // each stripe's first row, and the sync fd of its compose
		std::shared_ptr<Source> src;
		bool skip = false;     // no refresh could be claimed: composed whole later instead
		// Judged once both are in: the last stripe handed in (the transfer can
		// start before it), and the refresh presented.
		bool complete = false, presented = false;
		uint64_t present = 0;
		void close_fds()
		{
			for (int &fd : done_fds)
				if (fd >= 0) {
					close(fd);
					fd = -1;
				}
		}
	};
	std::shared_ptr<Streamed> streaming; // the frame being streamed (gpu_lock)
	std::map<uint64_t, std::shared_ptr<Streamed>> streamed; // by commit, from its claim until judged (rec_lock)
	void judge_streams(); // the compositor's thread
	std::map<uint64_t, uint32_t> claimed;  // commits a stream writes (gpu_lock)
	std::vector<double> first_lat; // stripe 0 handed in -> composed, ns
	double first_est = 2e6;
	int stream_torn = 0, stream_frames = 0, stream_unclaimed = 0;
	std::vector<double> slack; // per streamed frame: the least slack of its stripes, ns

	std::thread thread;
	std::atomic<bool> run{false};
	uint64_t last_seq = 0;

	Impl(Compositor &o, Presenter &p) : owner(o), presenter(p) {}
	bool make_objects();
	void destroy_objects();
	bool make_ring(int w, int h);
	void free_ring();
	bool ensure_image_pipeline(const VkAndroidHardwareBufferFormatPropertiesANDROID &fmt);
	Imported *import(AHardwareBuffer *ahb);
	void free_imported();
	void loop();
	void reap(bool wait);
	int pass(Job &j, const std::shared_ptr<Source> &s, Out &o, int y0, int rows, bool preserve, int wait_fence);
	void compose(Job &j, const std::shared_ptr<Source> &s, const Presenter::Commit &c, uint64_t sample, uint64_t woke);
	void retire(); // sources superseded and idle
	void emit_dropped(std::vector<std::shared_ptr<Source>> &v);
};

// ---- GPU objects ------------------------------------------------------------------

static VkShaderModule module(VkDevice vk, const uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = bytes;
	ci.pCode = code;
	VkShaderModule m = VK_NULL_HANDLE;
	vkCreateShaderModule(vk, &ci, nullptr, &m);
	return m;
}

static VkPipeline compute_pipeline(VkDevice vk, VkPipelineLayout pl, const uint32_t *code, size_t bytes)
{
	VkShaderModule m = module(vk, code, bytes);
	if (!m)
		return VK_NULL_HANDLE;
	VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	ci.stage.module = m;
	ci.stage.pName = "main";
	ci.layout = pl;
	VkPipeline p = VK_NULL_HANDLE;
	vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
	vkDestroyShaderModule(vk, m, nullptr);
	return p;
}

bool Compositor::Impl::make_objects()
{
	if (objects)
		return true;
	vk = gpu->device();
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.maxLod = 0;
	if (vkCreateSampler(vk, &si, nullptr, &linear) != VK_SUCCESS)
		return false;
	VkDescriptorSetLayoutBinding b[4] = {};
	for (int i = 0; i < 4; i++) {
		b[i].binding = uint32_t(i);
		b[i].descriptorCount = 1;
		b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = 4;
	li.pBindings = b;
	if (vkCreateDescriptorSetLayout(vk, &li, nullptr, &planes_layout) != VK_SUCCESS)
		return false;
	VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &planes_layout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pc;
	if (vkCreatePipelineLayout(vk, &pli, nullptr, &planes_pl) != VK_SUCCESS)
		return false;
	planes_pipe = compute_pipeline(vk, planes_pl, compose_comp, sizeof(compose_comp));
	if (!planes_pipe)
		return false;
	VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * job_count},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, job_count}};
	VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pi.maxSets = job_count;
	pi.poolSizeCount = 2;
	pi.pPoolSizes = ps;
	if (vkCreateDescriptorPool(vk, &pi, nullptr, &planes_pool) != VK_SUCCESS)
		return false;
	VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	ci.queueFamilyIndex = gpu->compute_family();
	if (vkCreateCommandPool(vk, &ci, nullptr, &cmd_pool) != VK_SUCCESS)
		return false;
	for (auto &j : jobs) {
		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = cmd_pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(vk, &ai, &j.cmd) != VK_SUCCESS)
			return false;
		VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vkCreateFence(vk, &fi, nullptr, &j.fence) != VK_SUCCESS)
			return false;
		VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		if (vkCreateSemaphore(vk, &sci, nullptr, &j.wait) != VK_SUCCESS)
			return false;
		VkExportSemaphoreCreateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
		ex.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		sci.pNext = &ex;
		if (vkCreateSemaphore(vk, &sci, nullptr, &j.done) != VK_SUCCESS)
			return false;
		VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = planes_pool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &planes_layout;
		if (vkAllocateDescriptorSets(vk, &dai, &j.planes_set) != VK_SUCCESS)
			return false;
	}
	objects = true;
	return true;
}

void Compositor::Impl::destroy_objects()
{
	if (!vk)
		return;
	reap(true);
	free_imported();
	free_ring();
	for (auto &j : jobs) {
		if (j.fence)
			vkDestroyFence(vk, j.fence, nullptr);
		if (j.wait)
			vkDestroySemaphore(vk, j.wait, nullptr);
		if (j.done)
			vkDestroySemaphore(vk, j.done, nullptr);
		j = Job{};
	}
	if (cmd_pool)
		vkDestroyCommandPool(vk, cmd_pool, nullptr);
	if (planes_pool)
		vkDestroyDescriptorPool(vk, planes_pool, nullptr);
	if (planes_pipe)
		vkDestroyPipeline(vk, planes_pipe, nullptr);
	if (planes_pl)
		vkDestroyPipelineLayout(vk, planes_pl, nullptr);
	if (planes_layout)
		vkDestroyDescriptorSetLayout(vk, planes_layout, nullptr);
	if (linear)
		vkDestroySampler(vk, linear, nullptr);
	cmd_pool = VK_NULL_HANDLE;
	planes_pool = VK_NULL_HANDLE;
	planes_pipe = VK_NULL_HANDLE;
	planes_pl = VK_NULL_HANDLE;
	planes_layout = VK_NULL_HANDLE;
	linear = VK_NULL_HANDLE;
	objects = false;
}

// The ring: RGBA buffers in the panel's orientation that the display engine
// scans out as they are (no rotation, no compression: front-buffer usage),
// written by the compute pass.
bool Compositor::Impl::make_ring(int w, int h)
{
	free_ring();
	bool quarter = !turned && (rotation == 1 || rotation == 3);
	int bw = quarter ? h : w, bh = quarter ? w : h;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	{
		VkPhysicalDeviceExternalImageFormatInfo ext = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
		ext.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
		VkPhysicalDeviceImageFormatInfo2 fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext};
		fi.format = VK_FORMAT_R8G8B8A8_UNORM;
		fi.type = VK_IMAGE_TYPE_2D;
		fi.tiling = VK_IMAGE_TILING_OPTIMAL;
		fi.usage = usage;
		VkExternalImageFormatProperties eprops = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
		VkImageFormatProperties2 props = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &eprops};
		VkResult r = vkGetPhysicalDeviceImageFormatProperties2(gpu->dev->get_physical_device(), &fi, &props);
		if (r != VK_SUCCESS) {
			LOGE("compositor: RGBA8 AHardwareBuffer images can't be written by a compute shader here (%d)", r);
			return false;
		}
	}
	for (auto &o : outs) {
		AHardwareBuffer_Desc d = {};
		d.width = uint32_t(bw);
		d.height = uint32_t(bh);
		d.layers = 1;
		d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
		d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
		          AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | AHARDWAREBUFFER_USAGE_FRONT_BUFFER;
		if (AHardwareBuffer_allocate(&d, &o.ahb) != 0) {
			LOGE("compositor: cannot allocate a %dx%d front buffer", bw, bh);
			return false;
		}
		VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
		VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fmt};
		if (gpu->get_ahb_props(vk, o.ahb, &props) != VK_SUCCESS)
			return false;
		VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
		VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = {uint32_t(bw), uint32_t(bh), 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(vk, &ici, nullptr, &o.image) != VK_SUCCESS)
			return false;
		VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
		imp.buffer = o.ahb;
		VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
		ded.image = o.image;
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
		mai.allocationSize = props.allocationSize;
		mai.memoryTypeIndex = props.memoryTypeBits ? uint32_t(__builtin_ctz(props.memoryTypeBits)) : 0;
		if (vkAllocateMemory(vk, &mai, nullptr, &o.memory) != VK_SUCCESS ||
		    vkBindImageMemory(vk, o.image, o.memory, 0) != VK_SUCCESS)
			return false;
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = o.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VK_FORMAT_R8G8B8A8_UNORM;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (vkCreateImageView(vk, &vi, nullptr, &o.view) != VK_SUCCESS)
			return false;
		o.content = 0;
	}
	out_w = bw;
	out_h = bh;
	return true;
}

void Compositor::Impl::free_ring()
{
	for (auto &o : outs) {
		if (o.view)
			vkDestroyImageView(vk, o.view, nullptr);
		if (o.image)
			vkDestroyImage(vk, o.image, nullptr);
		if (o.memory)
			vkFreeMemory(vk, o.memory, nullptr);
		if (o.ahb)
			AHardwareBuffer_release(o.ahb);
		o = Out{};
	}
	out_w = out_h = 0;
}

// The decoder's pictures are in its own (vendor) format: sampled through a
// YCbCr conversion made for that format, an immutable sampler of the set.
bool Compositor::Impl::ensure_image_pipeline(const VkAndroidHardwareBufferFormatPropertiesANDROID &fmt)
{
	uint64_t key = fmt.externalFormat ? fmt.externalFormat : uint64_t(fmt.format) << 32;
	if (key == ycbcr_format && image_pipe)
		return true;
	reap(true);
	for (auto &j : jobs)
		j.image_set = VK_NULL_HANDLE;
	free_imported();
	if (image_pool)
		vkDestroyDescriptorPool(vk, image_pool, nullptr);
	if (image_pipe)
		vkDestroyPipeline(vk, image_pipe, nullptr);
	if (image_pl)
		vkDestroyPipelineLayout(vk, image_pl, nullptr);
	if (image_layout)
		vkDestroyDescriptorSetLayout(vk, image_layout, nullptr);
	if (ycbcr_sampler)
		vkDestroySampler(vk, ycbcr_sampler, nullptr);
	if (conversion)
		vkDestroySamplerYcbcrConversion(vk, conversion, nullptr);
	image_pool = VK_NULL_HANDLE;
	image_pipe = VK_NULL_HANDLE;
	image_pl = VK_NULL_HANDLE;
	image_layout = VK_NULL_HANDLE;
	ycbcr_sampler = VK_NULL_HANDLE;
	conversion = VK_NULL_HANDLE;
	ycbcr_format = ~0ull;

	bool linear_ok = fmt.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
	VkFilter filter = linear_ok ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	VkExternalFormatANDROID ef = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
	ef.externalFormat = fmt.externalFormat;
	VkSamplerYcbcrConversionCreateInfo cci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, &ef};
	cci.format = fmt.externalFormat ? VK_FORMAT_UNDEFINED : fmt.format;
	cci.ycbcrModel = fmt.suggestedYcbcrModel;
	cci.ycbcrRange = fmt.suggestedYcbcrRange;
	cci.components = fmt.samplerYcbcrConversionComponents;
	cci.xChromaOffset = fmt.suggestedXChromaOffset;
	cci.yChromaOffset = fmt.suggestedYChromaOffset;
	cci.chromaFilter = filter;
	if (vkCreateSamplerYcbcrConversion(vk, &cci, nullptr, &conversion) != VK_SUCCESS) {
		LOGE("compositor: no YCbCr conversion for the decoder's format");
		return false;
	}
	VkSamplerYcbcrConversionInfo conv = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
	conv.conversion = conversion;
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &conv};
	si.magFilter = si.minFilter = filter;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(vk, &si, nullptr, &ycbcr_sampler) != VK_SUCCESS)
		return false;
	VkDescriptorSetLayoutBinding b[2] = {};
	b[0].binding = 0;
	b[0].descriptorCount = 1;
	b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	b[0].pImmutableSamplers = &ycbcr_sampler;
	b[1].binding = 1;
	b[1].descriptorCount = 1;
	b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = 2;
	li.pBindings = b;
	if (vkCreateDescriptorSetLayout(vk, &li, nullptr, &image_layout) != VK_SUCCESS)
		return false;
	VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &image_layout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pc;
	if (vkCreatePipelineLayout(vk, &pli, nullptr, &image_pl) != VK_SUCCESS)
		return false;
	image_pipe = compute_pipeline(vk, image_pl, compose_ycbcr_comp, sizeof(compose_ycbcr_comp));
	if (!image_pipe)
		return false;
	// A YCbCr descriptor may take several slots (one per plane): room for 3.
	VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * job_count},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, job_count}};
	VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pi.maxSets = job_count;
	pi.poolSizeCount = 2;
	pi.pPoolSizes = ps;
	if (vkCreateDescriptorPool(vk, &pi, nullptr, &image_pool) != VK_SUCCESS)
		return false;
	for (auto &j : jobs) {
		VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = image_pool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &image_layout;
		if (vkAllocateDescriptorSets(vk, &dai, &j.image_set) != VK_SUCCESS)
			return false;
	}
	ycbcr_format = key;
	LOGI("compositor: decoder format %#llx (model %d, range %d, %s filter)", (unsigned long long)fmt.externalFormat,
	     int(fmt.suggestedYcbcrModel), int(fmt.suggestedYcbcrRange), linear_ok ? "linear" : "nearest");
	return true;
}

Compositor::Impl::Imported *Compositor::Impl::import(AHardwareBuffer *ahb)
{
	auto it = imported.find(ahb);
	if (it != imported.end())
		return &it->second;
	VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
	VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fmt};
	if (gpu->get_ahb_props(vk, ahb, &props) != VK_SUCCESS)
		return nullptr;
	if (!ensure_image_pipeline(fmt))
		return nullptr;
	AHardwareBuffer_Desc d = {};
	AHardwareBuffer_describe(ahb, &d);
	Imported im;
	im.external_format = fmt.externalFormat;
	im.coded_w = d.width;
	im.coded_h = d.height;
	VkExternalFormatANDROID ef = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
	ef.externalFormat = fmt.externalFormat;
	VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ef};
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt.externalFormat ? VK_FORMAT_UNDEFINED : fmt.format;
	ici.extent = {d.width, d.height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(vk, &ici, nullptr, &im.image) != VK_SUCCESS)
		return nullptr;
	VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
	imp.buffer = ahb;
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
	ded.image = im.image;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
	mai.allocationSize = props.allocationSize;
	mai.memoryTypeIndex = props.memoryTypeBits ? uint32_t(__builtin_ctz(props.memoryTypeBits)) : 0;
	if (vkAllocateMemory(vk, &mai, nullptr, &im.memory) != VK_SUCCESS ||
	    vkBindImageMemory(vk, im.image, im.memory, 0) != VK_SUCCESS) {
		vkDestroyImage(vk, im.image, nullptr);
		if (im.memory)
			vkFreeMemory(vk, im.memory, nullptr);
		return nullptr;
	}
	VkSamplerYcbcrConversionInfo conv = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
	conv.conversion = conversion;
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &conv};
	vi.image = im.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = ici.format;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(vk, &vi, nullptr, &im.view) != VK_SUCCESS) {
		vkDestroyImage(vk, im.image, nullptr);
		vkFreeMemory(vk, im.memory, nullptr);
		return nullptr;
	}
	AHardwareBuffer_acquire(ahb);
	return &(imported[ahb] = im);
}

void Compositor::Impl::free_imported()
{
	for (auto &[ahb, im] : imported) {
		vkDestroyImageView(vk, im.view, nullptr);
		vkDestroyImage(vk, im.image, nullptr);
		vkFreeMemory(vk, im.memory, nullptr);
		AHardwareBuffer_release(ahb);
	}
	imported.clear();
}

// ---- Composing ----------------------------------------------------------------------

void Compositor::Impl::reap(bool wait)
{
	std::vector<std::shared_ptr<Source>> done;
	for (auto &j : jobs) {
		if (!j.busy)
			continue;
		if (wait)
			vkWaitForFences(vk, 1, &j.fence, VK_TRUE, 200000000ull);
		if (vkGetFenceStatus(vk, j.fence) != VK_SUCCESS)
			continue;
		j.busy = false;
		done.push_back(std::move(j.src));
		j.src.reset();
	}
	std::lock_guard<std::mutex> g(src_lock);
	for (auto &s : done)
		s->uses--;
}

void Compositor::Impl::emit_dropped(std::vector<std::shared_ptr<Source>> &v)
{
	for (auto &s : v)
		if (owner.on_dropped)
			owner.on_dropped(s->t);
	v.clear();
}

// Sources older than the newest one a sample point took: given back once no
// compose reads them. (Newer ones may still be decoding: they stay.)
void Compositor::Impl::retire()
{
	std::vector<std::shared_ptr<Source>> gone, dropped;
	{
		std::lock_guard<std::mutex> g(src_lock);
		size_t newest = sources.size();
		for (size_t i = sources.size(); i-- > 0;)
			if (sources[i]->sampled) {
				newest = i;
				break;
			}
		if (newest == sources.size())
			newest = 0;
		size_t i = 0;
		while (i < newest && i < sources.size()) {
			auto s = sources[i];
			if (s->uses) {
				i++;
				continue;
			}
			sources.erase(sources.begin() + long(i));
			newest--;
			if (!s->sampled && !s->emitted) {
				s->emitted = true;
				dropped.push_back(s);
			}
			gone.push_back(s);
		}
	}
	for (auto &s : gone) {
		if (s->fence >= 0) {
			close(s->fence);
			s->fence = -1;
		}
		if (s->release) {
			s->release();
			s->release = nullptr;
		}
	}
	emit_dropped(dropped);
}

// One compute pass: rows [y0, y0 + rows) of buffer `o` from picture `s`,
// after `wait_fence` (not owned; -1: none). The sync fd of its end, or -1.
int Compositor::Impl::pass(Job &j, const std::shared_ptr<Source> &s, Out &o, int y0, int rows, bool preserve,
                           int wait_fence)
{
	uint32_t cf = gpu->compute_family();
	Imported *im = nullptr;
	if (s->image) {
		im = import(s->ahb);
		if (!im)
			return -1;
	}
	vkResetFences(vk, 1, &j.fence);
	vkResetCommandBuffer(j.cmd, 0);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(j.cmd, &bi);

	// Taken from the display (its old content kept only when only some rows
	// are written: a stripe), and the decoder's picture from its producer.
	VkImageMemoryBarrier2 acq[2] = {};
	acq[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	// (After this queue's earlier writes to it, in submission order.)
	acq[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	acq[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	acq[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	acq[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	acq[0].oldLayout = preserve ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
	acq[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	acq[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	acq[0].dstQueueFamilyIndex = cf;
	acq[0].image = o.image;
	acq[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	int n = 1;
	if (im) {
		acq[1] = acq[0];
		acq[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
		acq[1].srcAccessMask = VK_ACCESS_2_NONE;
		acq[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
		acq[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		acq[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		acq[1].image = im->image;
		n = 2;
	}
	VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.imageMemoryBarrierCount = uint32_t(n);
	dep.pImageMemoryBarriers = acq;
	vkCmdPipelineBarrier2(j.cmd, &dep);

	PushConstants pc = {};
	pc.size[0] = out_w;
	pc.size[1] = out_h;
	pc.origin[1] = y0;
	pc.rotation = turned ? 0 : rotation; // a turned picture is already in the panel's orientation
	VkDescriptorImageInfo dst = {VK_NULL_HANDLE, o.view, VK_IMAGE_LAYOUT_GENERAL};
	if (im) {
		VkDescriptorImageInfo pic = {VK_NULL_HANDLE, im->view, VK_IMAGE_LAYOUT_GENERAL};
		VkWriteDescriptorSet w[2] = {};
		w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[0].dstSet = w[1].dstSet = j.image_set;
		w[0].dstBinding = 0;
		w[0].descriptorCount = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[0].pImageInfo = &pic;
		w[1].dstBinding = 1;
		w[1].descriptorCount = 1;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w[1].pImageInfo = &dst;
		vkUpdateDescriptorSets(vk, 2, w, 0, nullptr);
		pc.scale[0] = float(s->w) / float(im->coded_w);
		pc.scale[1] = float(s->h) / float(im->coded_h);
		vkCmdBindPipeline(j.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, image_pipe);
		vkCmdBindDescriptorSets(j.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, image_pl, 0, 1, &j.image_set, 0, nullptr);
		vkCmdPushConstants(j.cmd, image_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	} else {
		VkDescriptorImageInfo planes[3];
		for (int i = 0; i < 3; i++)
			planes[i] = {linear, s->planes[i], VK_IMAGE_LAYOUT_GENERAL};
		VkWriteDescriptorSet w[2] = {};
		w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[0].dstSet = w[1].dstSet = j.planes_set;
		w[0].dstBinding = 0;
		w[0].descriptorCount = 3;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[0].pImageInfo = planes;
		w[1].dstBinding = 3;
		w[1].descriptorCount = 1;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w[1].pImageInfo = &dst;
		vkUpdateDescriptorSets(vk, 2, w, 0, nullptr);
		pc.scale[0] = pc.scale[1] = 1;
		vkCmdBindPipeline(j.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, planes_pipe);
		vkCmdBindDescriptorSets(j.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, planes_pl, 0, 1, &j.planes_set, 0, nullptr);
		vkCmdPushConstants(j.cmd, planes_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	}
	vkCmdDispatch(j.cmd, uint32_t(out_w + 7) / 8, uint32_t(rows + 7) / 8, 1);

	// Back to the display, and the picture back to its producer.
	VkImageMemoryBarrier2 rel[2] = {};
	rel[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	rel[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	rel[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	rel[0].dstStageMask = VK_PIPELINE_STAGE_2_NONE;
	rel[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	rel[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	rel[0].srcQueueFamilyIndex = cf;
	rel[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	rel[0].image = o.image;
	rel[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (im) {
		rel[1] = rel[0];
		rel[1].srcAccessMask = VK_ACCESS_2_NONE;
		rel[1].image = im->image;
	}
	dep.pImageMemoryBarriers = rel;
	vkCmdPipelineBarrier2(j.cmd, &dep);
	vkEndCommandBuffer(j.cmd);

	// Wait for the picture (the wait also makes its writes visible to this
	// queue), signal a sync fd whose time is when the compose ended.
	bool waits = false;
	if (wait_fence >= 0) {
		int fd = dup(wait_fence);
		VkImportSemaphoreFdInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
		ii.semaphore = j.wait;
		ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
		ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		ii.fd = fd;
		if (fd >= 0 && gpu->import_semaphore_fd(vk, &ii) == VK_SUCCESS)
			waits = true;
		else if (fd >= 0)
			close(fd);
	}
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = waits ? 1 : 0;
	si.pWaitSemaphores = &j.wait;
	si.pWaitDstStageMask = &wait_stage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &j.cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &j.done;
	gpu->dev->external_queue_lock();
	VkResult r = vkQueueSubmit(gpu->compute_queue(), 1, &si, j.fence);
	gpu->dev->external_queue_unlock();
	if (r != VK_SUCCESS) {
		LOGE("compositor: submit failed (%d)", r);
		return -1;
	}
	int done_fd = -1;
	VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	gi.semaphore = j.done;
	gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	if (gpu->get_semaphore_fd(vk, &gi, &done_fd) != VK_SUCCESS)
		done_fd = -1;
	j.busy = true;
	j.src = s;
	{
		std::lock_guard<std::mutex> g(src_lock);
		s->uses++;
	}
	composes++;
	return done_fd;
}

// The newest picture into a refresh's buffer, all of it.
void Compositor::Impl::compose(Job &j, const std::shared_ptr<Source> &s, const Presenter::Commit &c, uint64_t sample,
                               uint64_t woke)
{
	int done_fd = pass(j, s, outs[c.buffer], 0, out_h, false, s->fence);
	if (done_fd < 0 && !j.busy)
		return;
	outs[c.buffer].content = s->id;
	uint64_t submitted = now_ns();
	std::lock_guard<std::mutex> g(rec_lock);
	records[c.seq] = {s, sample, woke, submitted, done_fd};
	while (records.size() > 16) {
		if (records.begin()->second.done_fd >= 0)
			close(records.begin()->second.done_fd);
		records.erase(records.begin());
	}
}

void Compositor::Impl::loop()
{
	// As urgent as the system lets an app's thread be (an app can't go real time).
	if (setpriority(PRIO_PROCESS, 0, -19) != 0)
		setpriority(PRIO_PROCESS, 0, -10);
	const PanelSchedule &ps = presenter.schedule();
	while (run) {
		judge_streams();
		uint64_t now = now_ns();
		// The next refresh whose buffer can still be written before its prefetch.
		Presenter::Commit next{};
		bool have = false;
		for (auto &c : presenter.upcoming()) {
			if (c.seq <= last_seq || c.predicted == 0)
				continue;
			if (double(c.predicted) - ps.prefill_ns <= double(now)) {
				skipped_late++;
				last_seq = c.seq; // too late for it: its buffer keeps what it has
				continue;
			}
			next = c;
			have = true;
			break;
		}
		if (!have) {
			usleep(500);
			continue;
		}
		uint64_t sample = uint64_t(std::max(double(now), double(next.predicted) - ps.prefill_ns - budget));
		sleep_until(sample);
		uint64_t woke = now_ns();
		last_seq = next.seq;
		std::lock_guard<std::mutex> gl(gpu_lock);
		if (!run)
			break;
		reap(false);
		retire();
		// The newest picture that is ready now; the ones before it that no
		// sample point took are overtaken.
		std::shared_ptr<Source> pick;
		std::vector<std::shared_ptr<Source>> dropped;
		{
			std::lock_guard<std::mutex> g(src_lock);
			for (auto it = sources.rbegin(); it != sources.rend(); ++it) {
				auto &s = *it;
				if (!s->ready) {
					uint64_t t = s->fence < 0 ? now_ns() : fence_time(s->fence, 0);
					if (!t)
						continue;
					s->ready = t;
				}
				pick = s;
				break;
			}
			if (pick) {
				for (auto &s : sources) {
					if (s == pick)
						break;
					if (!s->sampled && !s->emitted) {
						s->emitted = true;
						dropped.push_back(s);
					}
				}
				if (!pick->sampled) {
					pick->sampled = true;
					pick->t.latched = sample;
					if (pick->ready > pick->t.decoded)
						pick->t.decoded = pick->ready;
				}
			}
		}
		emit_dropped(dropped);
		{
			std::lock_guard<std::mutex> g(rec_lock);
			if (claimed.count(next.seq))
				continue; // a stream is writing this one
		}
		if (!pick || outs[next.buffer].content == pick->id)
			continue; // nothing new for this buffer
		Job &j = jobs[next_job];
		if (j.busy) {
			reap(true);
		}
		next_job = (next_job + 1) % job_count;
		compose(j, pick, next, sample, woke);
		wake.push_back(double(woke) - double(sample));
	}
}

// ---- Compositor ----------------------------------------------------------------------

// Streamed frames whose refresh was presented and whose stripes are all
// composed: each stripe against the row it starts at, fetched from P - prefill
// + max(0, row - prefill rows) * line. The least slack of them says how much
// later the frame could have come.
void Compositor::Impl::judge_streams()
{
	const PanelSchedule &sched = presenter.schedule();
	struct Out {
		FrameTiming t;
		int torn_row;
		double slack;
		bool shown, dropped;
	};
	std::vector<Out> out;
	{
		std::lock_guard<std::mutex> g(rec_lock);
		uint64_t newest = streamed.empty() ? 0 : streamed.rbegin()->first;
		for (auto it = streamed.begin(); it != streamed.end();) {
			auto &str = *it->second;
			if (!str.complete || !str.presented) {
				if (it->first + 16 < newest) { // never presented
					str.close_fds();
					it = streamed.erase(it);
				} else {
					++it;
				}
				continue;
			}
			std::vector<uint64_t> done(str.done_fds.size(), 0);
			bool all = true;
			for (size_t i = 0; i < str.done_fds.size(); i++) {
				done[i] = fence_time(str.done_fds[i], 0);
				if (!done[i] && str.done_fds[i] >= 0)
					all = false;
			}
			if (!all) {
				++it;
				continue;
			}
			uint64_t present = str.present;
			double worst = 1e18;
			int torn_row = -1;
			uint64_t last = 0;
			for (size_t i = 0; i < done.size(); i++) {
				if (!done[i])
					continue;
				last = std::max(last, done[i]);
				// The picture's rows to the panel's (the display scales a picture
				// taller than the panel, e.g. 2560 rows onto 1920).
				int row = out_h > 0 ? int(int64_t(str.ys[i]) * sched.rows / out_h) : str.ys[i];
				double slack = double(present) + sched.deadline_ns(row) - double(done[i]);
				if (slack < 0 && torn_row < 0)
					torn_row = row + std::max(0, int(-slack / sched.line_ns()));
				worst = std::min(worst, slack);
			}
			if (!done.empty() && done[0]) {
				first_lat.push_back(double(done[0]) - double(str.first));
				if (first_lat.size() >= 60) {
					std::vector<double> s = first_lat;
					std::sort(s.begin(), s.end());
					first_est = s[s.size() * 95 / 100] + 0.2e6;
					first_lat.clear();
				}
			}
			Out o{str.src->t, torn_row, 0, false, false};
			if (!present || worst > 1e17) {
				o.dropped = true;
			} else {
				slack.push_back(worst);
				o.t.presented = present;
				o.t.latched = uint64_t(double(present) - sched.prefill_ns);
				// Ready, as the phase lock sees it: when stripe 0 would have had
				// to be done for every stripe to have just made it.
				o.t.submitted = uint64_t(double(o.t.latched) - worst);
				o.t.decoded = last;
				// The claim: stripe 0 had to be handed in first_est before the
				// prefetch. How much earlier it was, modulo a refresh (a frame
				// that only just missed the refresh before shows here as a
				// little late, not as a whole refresh early).
				double period = sched.period_ns;
				double claim = std::fmod(double(present) - sched.prefill_ns - str.first_est - double(str.first), period);
				if (claim < 0)
					claim += period;
				if (claim > period / 2)
					claim -= period;
				o.slack = std::min(claim, worst);
				o.shown = torn_row < 0;
				if (!o.shown)
					stream_torn++;
				if (debug_stream())
					LOGI("stream: frame %u seq %llu claim %+.2f worst %+.2f first->done %.2f handin->P %.2f last->P %+.2f",
					     str.frame, (unsigned long long)it->first, claim / 1e6, worst / 1e6,
					     done[0] ? (double(done[0]) - double(str.first)) / 1e6 : -1.0,
					     (double(present) - double(str.first)) / 1e6, (double(last) - double(present)) / 1e6);
			}
			str.close_fds();
			out.push_back(o);
			it = streamed.erase(it);
		}
	}
	for (auto &o : out) {
		if (o.dropped) {
			if (owner.on_dropped)
				owner.on_dropped(o.t);
			continue;
		}
		if (owner.on_slack)
			owner.on_slack(o.slack);
		if (o.shown) {
			if (owner.on_presented)
				owner.on_presented(o.t);
		} else if (owner.on_torn) {
			owner.on_torn(o.t, std::min(o.torn_row, sched.rows - 1));
		}
	}
}

Compositor::Compositor(Presenter &p) : presenter_(p), impl_(std::make_unique<Impl>(*this, p)) {}
Compositor::~Compositor()
{
	stop();
	std::lock_guard<std::mutex> g(impl_->gpu_lock);
	impl_->destroy_objects();
}
Gpu *Compositor::gpu() { return impl_->gpu; }

bool Compositor::start(int w, int h, int rotation, bool turned)
{
	if (running_ && impl_->pic_w == w && impl_->pic_h == h && impl_->rotation == (rotation & 3) &&
	    impl_->turned == turned)
		return true;
	stop();
	Impl &m = *impl_;
	if (!m.gpu->init() || !m.gpu->ycbcr)
		return false;
	m.rotation = rotation & 3;
	m.turned = turned;
	turned_ = turned;
	{
		std::lock_guard<std::mutex> g(m.gpu_lock);
		if (!m.make_objects() || !m.make_ring(w, h)) {
			m.free_ring();
			return false;
		}
	}
	m.pic_w = w;
	m.pic_h = h;
	m.last_seq = 0;
	m.budget = 1.5e6;
	m.lat.clear();
	m.wake.clear();
	std::vector<AHardwareBuffer *> bufs;
	for (auto &o : m.outs)
		bufs.push_back(o.ahb);
	presenter_.on_ring_presented = [this](uint64_t seq, int b, uint64_t p) { ring_presented(seq, b, p); };
	int32_t transform = m.rotation == 1 ? ANATIVEWINDOW_TRANSFORM_ROTATE_270
	                    : m.rotation == 3 ? ANATIVEWINDOW_TRANSFORM_ROTATE_90 : 0;
	// The crop is in the picture's orientation (after the transform): a
	// turned picture's sides swapped back.
	presenter_.set_ring(bufs, turned ? h : w, turned ? w : h, transform);
	running_ = true;
	m.run = true;
	m.thread = std::thread([&m] { m.loop(); });
	LOGI("compositor: %dx%d pictures%s into %dx%d buffers (rotation %d)", w, h, turned ? " (turned: streamed)" : "",
	     m.out_w, m.out_h, m.rotation);
	return true;
}

void Compositor::stop()
{
	if (!impl_)
		return;
	Impl &m = *impl_;
	m.run = false;
	if (m.thread.joinable())
		m.thread.join();
	running_ = false;
	presenter_.set_ring({}, 0, 0, 0);
	std::lock_guard<std::mutex> g(m.gpu_lock);
	m.reap(true);
	{
		std::lock_guard<std::mutex> r(m.rec_lock);
		for (auto &[seq, rec] : m.records)
			if (rec.done_fd >= 0)
				close(rec.done_fd);
		m.records.clear();
		for (auto &[seq, s] : m.streamed)
			s->close_fds();
		m.streamed.clear();
		m.claimed.clear();
		if (m.streaming)
			m.streaming->close_fds();
		m.streaming.reset();
	}
	std::deque<std::shared_ptr<Impl::Source>> all;
	{
		std::lock_guard<std::mutex> s(m.src_lock);
		all.swap(m.sources);
	}
	for (auto &s : all) {
		if (s->fence >= 0)
			close(s->fence);
		if (s->release)
			s->release();
	}
	m.free_ring();
	m.pic_w = m.pic_h = 0;
}

static std::shared_ptr<Compositor::Impl::Source> new_source(Compositor::Impl &m)
{
	auto s = std::make_shared<Compositor::Impl::Source>();
	s->id = ++m.next_id;
	return s;
}

void Compositor::submit_planes(const VkImageView planes[3], int w, int h, int fence, const FrameTiming &t)
{
	if (!running_) {
		if (fence >= 0)
			close(fence);
		return;
	}
	Impl &m = *impl_;
	std::lock_guard<std::mutex> g(m.src_lock);
	auto s = new_source(m);
	for (int i = 0; i < 3; i++)
		s->planes[i] = planes[i];
	s->w = w;
	s->h = h;
	s->fence = fence;
	s->t = t;
	s->t.submitted = now_ns();
	m.sources.push_back(s);
}

void Compositor::submit_image(AHardwareBuffer *ahb, int fence, int crop_w, int crop_h, const FrameTiming &t,
                              std::function<void()> release)
{
	if (!running_) {
		if (fence >= 0)
			close(fence);
		if (release)
			release();
		return;
	}
	Impl &m = *impl_;
	std::lock_guard<std::mutex> g(m.src_lock);
	auto s = new_source(m);
	s->image = true;
	s->ahb = ahb;
	s->w = crop_w;
	s->h = crop_h;
	s->fence = fence;
	s->t = t;
	s->t.submitted = now_ns();
	s->release = std::move(release);
	m.sources.push_back(s);
}

void Compositor::stream_stripe(uint32_t frame, int k, int count, const VkImageView planes[3], int w, int h, int y,
                               int rows, int fence, const FrameTiming &t)
{
	if (!streaming()) {
		if (fence >= 0)
			close(fence);
		return;
	}
	Impl &m = *impl_;
	const PanelSchedule &ps = presenter_.schedule();
	std::vector<FrameTiming> overtaken;
	struct Tell {
		Compositor &c;
		std::vector<FrameTiming> &v;
		~Tell()
		{
			for (auto &t : v)
				if (c.on_dropped)
					c.on_dropped(t);
		}
	} tell{*this, overtaken}; // after the locks below are released
	std::lock_guard<std::mutex> gl(m.gpu_lock);
	uint64_t now = now_ns();
	if (k == 0 || !m.streaming || m.streaming->frame != frame) {
		// A new frame: the first refresh whose prefetch comes after stripe 0
		// can be composed (its usual time from being handed in, measured).
		auto st = std::make_shared<Impl::Streamed>();
		st->frame = frame;
		st->first = now;
		st->first_est = m.first_est;
		{
			std::lock_guard<std::mutex> s(m.src_lock);
			st->src = new_source(m);
		}
		std::lock_guard<std::mutex> r(m.rec_lock);
		if (m.streaming && !m.streaming->complete) {
			// Never finished (a reset): let its refresh go.
			m.claimed.erase(m.streaming->seq);
			m.streamed.erase(m.streaming->seq);
			m.streaming->close_fds();
		}
		for (auto &c : presenter_.upcoming()) {
			double cutoff = double(c.predicted) - ps.prefill_ns;
			if (c.predicted == 0 || cutoff < double(now) + m.first_est)
				continue;
			// An older frame waiting for this refresh (it missed the one
			// before): this one takes it over and the older one is dropped.
			// Queued behind it instead, every frame after would go out a
			// refresh late too.
			auto old = m.streamed.find(c.seq);
			if (old != m.streamed.end()) {
				old->second->close_fds();
				overtaken.push_back(old->second->src->t);
				m.streamed.erase(old);
			}
			st->seq = c.seq;
			st->buffer = c.buffer;
			st->cutoff = uint64_t(cutoff);
			m.claimed[c.seq] = frame;
			m.streamed[c.seq] = st;
			break;
		}
		if (!st->seq) {
			st->skip = true; // no refresh to claim: the frame goes whole, through a sample point
			m.stream_unclaimed++;
		}
		m.streaming = st;
	}
	Impl::Streamed &st = *m.streaming;
	auto &s = *st.src;
	for (int i = 0; i < 3; i++)
		s.planes[i] = planes[i];
	s.w = w;
	s.h = h;
	s.t = t;
	if (!s.t.submitted)
		s.t.submitted = st.first;
	if (!st.skip) {
		Impl::Job &j = m.jobs[m.next_job];
		if (j.busy)
			m.reap(true);
		m.next_job = (m.next_job + 1) % job_count;
		int done = m.pass(j, st.src, m.outs[st.buffer], y, rows, true, fence);
		std::lock_guard<std::mutex> r(m.rec_lock);
		st.ys.push_back(y);
		st.done_fds.push_back(done);
	}
	if (k + 1 < count) {
		if (fence >= 0)
			close(fence);
		return;
	}
	// The last stripe: the whole frame is a picture for the refreshes after.
	if (s.fence >= 0)
		close(s.fence);
	s.fence = fence;
	if (!st.skip) {
		m.outs[st.buffer].content = s.id;
		s.sampled = true;
		s.emitted = true; // told when it is judged (judge_streams)
		s.t.latched = st.cutoff;
	}
	{
		std::lock_guard<std::mutex> g(m.src_lock);
		m.sources.push_back(st.src);
	}
	std::lock_guard<std::mutex> r(m.rec_lock);
	st.complete = true;
	if (!st.skip)
		m.stream_frames++;
	m.streaming.reset();
}

void Compositor::forget_planes()
{
	if (!impl_)
		return;
	Impl &m = *impl_;
	std::lock_guard<std::mutex> g(m.gpu_lock);
	m.reap(true);
	{
		std::lock_guard<std::mutex> r(m.rec_lock);
		if (m.streaming && !m.streaming->complete) {
			m.claimed.erase(m.streaming->seq);
			m.streamed.erase(m.streaming->seq);
			m.streaming->close_fds();
		}
		m.streaming.reset();
	}
	std::lock_guard<std::mutex> s(m.src_lock);
	for (auto it = m.sources.begin(); it != m.sources.end();) {
		if ((*it)->image) {
			++it;
			continue;
		}
		if ((*it)->fence >= 0)
			close((*it)->fence);
		it = m.sources.erase(it);
	}
}

void Compositor::forget_images()
{
	if (!impl_)
		return;
	Impl &m = *impl_;
	std::lock_guard<std::mutex> g(m.gpu_lock);
	m.reap(true);
	std::deque<std::shared_ptr<Impl::Source>> gone;
	{
		std::lock_guard<std::mutex> s(m.src_lock);
		for (auto it = m.sources.begin(); it != m.sources.end();) {
			if (!(*it)->image) {
				++it;
				continue;
			}
			gone.push_back(*it);
			it = m.sources.erase(it);
		}
	}
	for (auto &s : gone) {
		if (s->fence >= 0)
			close(s->fence);
		if (s->release)
			s->release();
		s->release = nullptr;
	}
	if (m.vk)
		m.free_imported();
}

// A commit's present is known: the compose that was for it, against the
// transfer it was read in.
void Compositor::ring_presented(uint64_t seq, int, uint64_t present)
{
	if (!impl_)
		return;
	Impl &m = *impl_;
	const PanelSchedule &sched = presenter_.schedule();
	{
		// A streamed frame's refresh: judged when its last stripe is in too
		// (judge_streams), which can be after the transfer has started.
		std::lock_guard<std::mutex> g(m.rec_lock);
		m.claimed.erase(seq);
		auto it = m.streamed.find(seq);
		if (it != m.streamed.end()) {
			it->second->presented = true;
			it->second->present = present;
			return;
		}
	}
	Impl::Record rec;
	{
		std::lock_guard<std::mutex> g(m.rec_lock);
		auto it = m.records.find(seq);
		if (it == m.records.end())
			return; // nothing composed for it: the buffer showed what it held
		rec = it->second;
		m.records.erase(it);
	}
	uint64_t done = fence_time(rec.done_fd, 30);
	if (rec.done_fd >= 0)
		close(rec.done_fd);
	const PanelSchedule &ps = presenter_.schedule();
	if (done) {
		double l = double(done) - double(rec.sample);
		std::lock_guard<std::mutex> g(m.rec_lock);
		m.lat.push_back(l);
		m.window_lat.push_back(l);
		if (m.lat.size() >= 120) {
			// The budget: the slowest composes of the last second, and a margin.
			std::vector<double> s = m.lat;
			std::sort(s.begin(), s.end());
			m.budget = std::clamp(s[s.size() * 99 / 100] + 0.25e6, 0.4e6, 4e6);
			m.lat.clear();
		}
	}
	auto &src = rec.src;
	if (!present || src->emitted)
		return;
	src->emitted = true;
	FrameTiming t = src->t;
	t.presented = present;
	// Whole frames steer the phase lock the same way as streamed ones: how
	// long it waited, ready, for the sample point that took it, modulo a
	// refresh (one that only just missed the sample point before shows as a
	// little late, not as a whole refresh early).
	if (on_slack && src->ready && t.latched) {
		double period = ps.period_ns;
		double wait = std::fmod(double(t.latched) - double(src->ready), period);
		if (wait < 0)
			wait += period;
		if (wait > period / 2)
			wait -= period;
		on_slack(wait);
	}
	double fetch = double(present) - ps.prefill_ns;
	if (!done || double(done) <= fetch) {
		if (on_presented)
			on_presented(t);
		return;
	}
	// Done after the prefetch began: the rows fetched by then are the
	// buffer's old content (a frame from three refreshes before).
	m.torn++;
	double into = double(done) - fetch;
	int row = std::min(ps.rows, ps.prefill_lines + int(into / ps.line_ns()));
	if (on_torn)
		on_torn(t, row);
}

std::string Compositor::summary()
{
	if (!impl_ || !running_)
		return "";
	Impl &m = *impl_;
	std::vector<double> l, w;
	int composes, torn, skipped;
	double budget;
	{
		std::lock_guard<std::mutex> g(m.rec_lock);
		l.swap(m.window_lat);
		budget = m.budget;
	}
	{
		std::lock_guard<std::mutex> g(m.gpu_lock);
		w.swap(m.wake);
		composes = m.composes;
		skipped = m.skipped_late;
		m.composes = m.skipped_late = 0;
	}
	torn = m.torn.exchange(0);
	std::vector<double> sl;
	int sframes, storn, sun;
	double fest;
	{
		std::lock_guard<std::mutex> g(m.rec_lock);
		sl.swap(m.slack);
		sframes = m.stream_frames;
		storn = m.stream_torn;
		sun = m.stream_unclaimed;
		fest = m.first_est;
		m.stream_frames = m.stream_torn = m.stream_unclaimed = 0;
	}
	std::sort(sl.begin(), sl.end());
	std::sort(l.begin(), l.end());
	std::sort(w.begin(), w.end());
	auto p = [](const std::vector<double> &v, double q) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, size_t(double(v.size()) * q))] / 1e6; };
	char b[400];
	int n = snprintf(b, sizeof(b), "compose: %d/s, sample->done %.2f / %.2f ms, wake %.2f / %.2f, budget %.2f, torn %d, missed %d",
	                 composes, p(l, 0.5), p(l, 0.99), p(w, 0.5), p(w, 0.99), budget / 1e6, torn, skipped);
	if (m.turned && n > 0 && n < int(sizeof(b)))
		snprintf(b + n, sizeof(b) - size_t(n),
		         "\nstreamed: %d/s, slack p50 %.2f p1 %.2f ms, torn %d, unclaimed %d, stripe 0 in %.2f ms",
		         sframes, p(sl, 0.5), p(sl, 0.01), storn, sun, fest / 1e6);
	return b;
}

} // namespace gm
