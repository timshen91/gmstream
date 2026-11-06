// Keeps the GPU's clock up while Riptide frames arrive (apple/GPUWarm.swift,
// core/gpuwarm.c): decoding a stripe at a time is short bursts, which the
// GPU's power management reads as a light load. A Vulkan device of its own,
// on a compute queue at low priority where the driver allows it, runs a tiny
// kernel back to back, two in flight, and stops a second after the last frame.
// Plain Vulkan through the loader (no Granite), so it shares nothing with the
// decoder's device.

#include "gpuwarm.h"

#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "conn.h"
#include "log.h"

namespace gm {

static const uint32_t warm_comp[] = {
#include "shaders/warm_comp.inc"
};

struct GpuWarm::Vk {
	void *lib = nullptr;
	PFN_vkGetInstanceProcAddr gipa = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkShaderModule module = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkCommandPool cmd_pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd[2] = {};
	VkFence fence[2] = {};
#define F(name) PFN_##name name = nullptr
	F(vkDestroyInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceQueueFamilyProperties);
	F(vkGetPhysicalDeviceMemoryProperties); F(vkCreateDevice); F(vkGetDeviceProcAddr); F(vkDestroyDevice);
	F(vkGetDeviceQueue); F(vkCreateBuffer); F(vkGetBufferMemoryRequirements); F(vkAllocateMemory);
	F(vkBindBufferMemory); F(vkCreateDescriptorSetLayout); F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets);
	F(vkUpdateDescriptorSets); F(vkCreatePipelineLayout); F(vkCreateShaderModule); F(vkCreateComputePipelines);
	F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkCmdBindPipeline);
	F(vkCmdBindDescriptorSets); F(vkCmdPushConstants); F(vkCmdDispatch); F(vkEndCommandBuffer); F(vkCreateFence);
	F(vkQueueSubmit); F(vkWaitForFences); F(vkResetFences); F(vkDeviceWaitIdle);
#undef F

	bool init();
	void destroy();
};

bool GpuWarm::Vk::init()
{
	lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!lib)
		return false;
	gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
	auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	app.pApplicationName = "gmstream gpu warm";
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	if (!create_instance || create_instance(&ici, nullptr, &instance) != VK_SUCCESS)
		return false;
#define L(name) name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
	L(vkDestroyInstance); L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceQueueFamilyProperties);
	L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
#undef L
	uint32_t n = 1;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	if (vkEnumeratePhysicalDevices(instance, &n, &gpu) < 0 || !gpu)
		return false;
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qf.data());
	// A compute-only family (Adreno's low-priority async compute) if there is one.
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++)
		if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			family = i;
	if (family == UINT32_MAX)
		for (uint32_t i = 0; i < qn; i++)
			if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
				family = i;
				break;
			}
	if (family == UINT32_MAX)
		return false;
	float prio = 0.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	// Low global priority where the driver offers it (it is an extension).
	VkDeviceQueueGlobalPriorityCreateInfoKHR gp = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
	gp.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
	const char *ext = VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
	qci.pNext = &gp;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = &ext;
	if (vkCreateDevice(gpu, &dci, nullptr, &device) != VK_SUCCESS) {
		qci.pNext = nullptr;
		dci.enabledExtensionCount = 0;
		if (vkCreateDevice(gpu, &dci, nullptr, &device) != VK_SUCCESS)
			return false;
	}
#define D(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
	D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory);
	D(vkBindBufferMemory); D(vkCreateDescriptorSetLayout); D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets);
	D(vkUpdateDescriptorSets); D(vkCreatePipelineLayout); D(vkCreateShaderModule); D(vkCreateComputePipelines);
	D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkCmdBindPipeline);
	D(vkCmdBindDescriptorSets); D(vkCmdPushConstants); D(vkCmdDispatch); D(vkEndCommandBuffer); D(vkCreateFence);
	D(vkQueueSubmit); D(vkWaitForFences); D(vkResetFences); D(vkDeviceWaitIdle);
#undef D
	vkGetDeviceQueue(device, family, 0, &queue);

	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = 4 * 64 * 4;
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, buffer, &req);
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = uint32_t(__builtin_ctz(req.memoryTypeBits));
	if (vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS || vkBindBufferMemory(device, buffer, memory, 0))
		return false;
	VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dl.bindingCount = 1;
	dl.pBindings = &b;
	vkCreateDescriptorSetLayout(device, &dl, nullptr, &dsl);
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
	VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dp.maxSets = 1;
	dp.poolSizeCount = 1;
	dp.pPoolSizes = &ps;
	vkCreateDescriptorPool(device, &dp, nullptr, &pool);
	VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	da.descriptorPool = pool;
	da.descriptorSetCount = 1;
	da.pSetLayouts = &dsl;
	if (vkAllocateDescriptorSets(device, &da, &set) != VK_SUCCESS)
		return false;
	VkDescriptorBufferInfo dbi = {buffer, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = set;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	w.pBufferInfo = &dbi;
	vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
	VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &dsl;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &pcr;
	vkCreatePipelineLayout(device, &pl, nullptr, &layout);
	VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	sm.codeSize = sizeof(warm_comp);
	sm.pCode = warm_comp;
	vkCreateShaderModule(device, &sm, nullptr, &module);
	VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
	cp.layout = layout;
	if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline) != VK_SUCCESS)
		return false;
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.queueFamilyIndex = family;
	vkCreateCommandPool(device, &cpi, nullptr, &cmd_pool);
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = cmd_pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 2;
	vkAllocateCommandBuffers(device, &cai, cmd);
	for (int i = 0; i < 2; i++) {
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		vkBeginCommandBuffer(cmd[i], &bi);
		vkCmdBindPipeline(cmd[i], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(cmd[i], VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
		float seed = 1.0f + float(i);
		vkCmdPushConstants(cmd[i], layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &seed);
		vkCmdDispatch(cmd[i], 4, 1, 1);
		vkEndCommandBuffer(cmd[i]);
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		vkCreateFence(device, &fci, nullptr, &fence[i]);
	}
	LOGI("gpu warm: queue family %u%s", family, dci.enabledExtensionCount ? ", low global priority" : "");
	return true;
}

void GpuWarm::Vk::destroy()
{
	// Everything goes with the device and the instance.
	if (device) {
		vkDeviceWaitIdle(device);
		vkDestroyDevice(device, nullptr);
	}
	if (instance)
		vkDestroyInstance(instance, nullptr);
	if (lib)
		dlclose(lib);
}

GpuWarm::~GpuWarm()
{
	{
		std::lock_guard<std::mutex> g(lock_);
		quit_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void GpuWarm::touch()
{
	last_touch_ = now_ns();
	if (!started_.exchange(true))
		thread_ = std::thread([this] { run(); });
	else
		cv_.notify_one();
}

void GpuWarm::run()
{
	Vk vk;
	bool ok = vk.init();
	if (!ok)
		LOGW("gpu warm: no Vulkan device for it");
	std::unique_lock<std::mutex> lk(lock_);
	int i = 0;
	while (!quit_) {
		if (!ok || now_ns() - last_touch_ > 1000000000ull) {
			cv_.wait_for(lk, std::chrono::milliseconds(100));
			continue;
		}
		lk.unlock();
		// Two in flight: the next waits for the one before the last.
		vk.vkWaitForFences(vk.device, 1, &vk.fence[i], VK_TRUE, 50000000ull);
		vk.vkResetFences(vk.device, 1, &vk.fence[i]);
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &vk.cmd[i];
		vk.vkQueueSubmit(vk.queue, 1, &si, vk.fence[i]);
		i ^= 1;
		lk.lock();
	}
	lk.unlock();
	if (ok)
		vk.vkWaitForFences(vk.device, 2, vk.fence, VK_TRUE, 100000000ull);
	vk.destroy();
}

} // namespace gm
