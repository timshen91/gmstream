// Keeps the GPU's clock up while Riptide frames are being encoded.
//
// The GPU's power management sets its clock by how busy it is. A game that
// leaves it lightly loaded leaves it slow, and the encoder, which does a frame in
// stripes as short bursts, then runs at about half speed (measured on a Radeon
// 8060S, 2560x1600 at 120 Hz with vkcube running: the first stripe out 1.04 ms
// after the frame, against 0.45 with the clock up; the whole frame 2.52 ms
// against 1.66). A tiny compute dispatch, submitted back to back on a queue of
// its own at low priority so it yields to the game and to the encoder, keeps
// the GPU busy in its eyes; it does almost no work. Only while frames are
// encoded: it stops a second after the last (gpuwarm_touch). Its own Vulkan
// device, so nothing is shared with the encoder's. GM_RIPTIDE_GPU_WARM=0 turns
// it off.

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>
#include <wlr/util/log.h>

#include "core.h"

// gpuwarm.comp: 64 threads a workgroup, 100 dependent FMAs each, a store that
// never happens (so the loop stays).
//   #version 450
//   layout(local_size_x = 64) in;
//   layout(set = 0, binding = 0) buffer B { float v[]; } b;
//   void main() { float x = float(gl_GlobalInvocationID.x);
//     for (int k = 0; k < 100; k++) x = fma(x, 1.0001, 0.5);
//     if (x == 12345.0) b.v[0] = x; }
static const uint32_t warm_spv[] = {
	0x07230203, 0x00010300, 0x0008000b, 0x00000035, 0x00000000, 0x00020011,
	0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
	0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x00060010, 0x00000004,
	0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00030003, 0x00000002,
	0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005,
	0x00000008, 0x00000078, 0x00080005, 0x0000000c, 0x475f6c67, 0x61626f6c,
	0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044, 0x00030005, 0x00000014,
	0x0000006b, 0x00030005, 0x0000002c, 0x00000042, 0x00040006, 0x0000002c,
	0x00000000, 0x00000076, 0x00030005, 0x0000002e, 0x00000062, 0x00040047,
	0x0000000c, 0x0000000b, 0x0000001c, 0x00040047, 0x0000002b, 0x00000006,
	0x00000004, 0x00030047, 0x0000002c, 0x00000002, 0x00050048, 0x0000002c,
	0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000002e, 0x00000021,
	0x00000000, 0x00040047, 0x0000002e, 0x00000022, 0x00000000, 0x00040047,
	0x00000034, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
	0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040020,
	0x00000007, 0x00000007, 0x00000006, 0x00040015, 0x00000009, 0x00000020,
	0x00000000, 0x00040017, 0x0000000a, 0x00000009, 0x00000003, 0x00040020,
	0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c,
	0x00000001, 0x0004002b, 0x00000009, 0x0000000d, 0x00000000, 0x00040020,
	0x0000000e, 0x00000001, 0x00000009, 0x00040015, 0x00000012, 0x00000020,
	0x00000001, 0x00040020, 0x00000013, 0x00000007, 0x00000012, 0x0004002b,
	0x00000012, 0x00000015, 0x00000000, 0x0004002b, 0x00000012, 0x0000001c,
	0x00000064, 0x00020014, 0x0000001d, 0x0004002b, 0x00000006, 0x00000020,
	0x3f800347, 0x0004002b, 0x00000006, 0x00000021, 0x3f000000, 0x0004002b,
	0x00000012, 0x00000024, 0x00000001, 0x0004002b, 0x00000006, 0x00000027,
	0x4640e400, 0x0003001d, 0x0000002b, 0x00000006, 0x0003001e, 0x0000002c,
	0x0000002b, 0x00040020, 0x0000002d, 0x0000000c, 0x0000002c, 0x0004003b,
	0x0000002d, 0x0000002e, 0x0000000c, 0x00040020, 0x00000030, 0x0000000c,
	0x00000006, 0x0004002b, 0x00000009, 0x00000032, 0x00000040, 0x0004002b,
	0x00000009, 0x00000033, 0x00000001, 0x0006002c, 0x0000000a, 0x00000034,
	0x00000032, 0x00000033, 0x00000033, 0x00050036, 0x00000002, 0x00000004,
	0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000007,
	0x00000008, 0x00000007, 0x0004003b, 0x00000013, 0x00000014, 0x00000007,
	0x00050041, 0x0000000e, 0x0000000f, 0x0000000c, 0x0000000d, 0x0004003d,
	0x00000009, 0x00000010, 0x0000000f, 0x00040070, 0x00000006, 0x00000011,
	0x00000010, 0x0003003e, 0x00000008, 0x00000011, 0x0003003e, 0x00000014,
	0x00000015, 0x000200f9, 0x00000016, 0x000200f8, 0x00000016, 0x000400f6,
	0x00000018, 0x00000019, 0x00000000, 0x000200f9, 0x0000001a, 0x000200f8,
	0x0000001a, 0x0004003d, 0x00000012, 0x0000001b, 0x00000014, 0x000500b1,
	0x0000001d, 0x0000001e, 0x0000001b, 0x0000001c, 0x000400fa, 0x0000001e,
	0x00000017, 0x00000018, 0x000200f8, 0x00000017, 0x0004003d, 0x00000006,
	0x0000001f, 0x00000008, 0x0008000c, 0x00000006, 0x00000022, 0x00000001,
	0x00000032, 0x0000001f, 0x00000020, 0x00000021, 0x0003003e, 0x00000008,
	0x00000022, 0x000200f9, 0x00000019, 0x000200f8, 0x00000019, 0x0004003d,
	0x00000012, 0x00000023, 0x00000014, 0x00050080, 0x00000012, 0x00000025,
	0x00000023, 0x00000024, 0x0003003e, 0x00000014, 0x00000025, 0x000200f9,
	0x00000016, 0x000200f8, 0x00000018, 0x0004003d, 0x00000006, 0x00000026,
	0x00000008, 0x000500b4, 0x0000001d, 0x00000028, 0x00000026, 0x00000027,
	0x000300f7, 0x0000002a, 0x00000000, 0x000400fa, 0x00000028, 0x00000029,
	0x0000002a, 0x000200f8, 0x00000029, 0x0004003d, 0x00000006, 0x0000002f,
	0x00000008, 0x00060041, 0x00000030, 0x00000031, 0x0000002e, 0x00000015,
	0x00000015, 0x0003003e, 0x00000031, 0x0000002f, 0x000200f9, 0x0000002a,
	0x000200f8, 0x0000002a, 0x000100fd, 0x00010038,
};

#define IDLE_NS 1000000000ull

struct gm_gpuwarm {
	void *lib;
	VkInstance instance;
	VkDevice device;
	VkQueue queue;
	VkCommandBuffer cmd;
	VkFence fence;
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkResetFences ResetFences;
	PFN_vkDeviceWaitIdle DeviceWaitIdle;

	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool quit;
	uint64_t last_touch;
};

static void *run(void *data) {
	struct gm_gpuwarm *w = data;
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &w->cmd };
	pthread_mutex_lock(&w->lock);
	while (!w->quit) {
		if (gm_now_ns() - w->last_touch > IDLE_NS) {
			pthread_cond_wait(&w->cond, &w->lock);
			continue;
		}
		pthread_mutex_unlock(&w->lock);
		if (w->QueueSubmit(w->queue, 1, &si, w->fence) == VK_SUCCESS) {
			w->WaitForFences(w->device, 1, &w->fence, VK_TRUE, UINT64_MAX);
			w->ResetFences(w->device, 1, &w->fence);
		}
		pthread_mutex_lock(&w->lock);
	}
	pthread_mutex_unlock(&w->lock);
	return NULL;
}

void gpuwarm_touch(struct gm_gpuwarm *w) {
	if (w == NULL) {
		return;
	}
	pthread_mutex_lock(&w->lock);
	bool was_idle = gm_now_ns() - w->last_touch > IDLE_NS;
	w->last_touch = gm_now_ns();
	if (was_idle) {
		pthread_cond_signal(&w->cond);
	}
	pthread_mutex_unlock(&w->lock);
}

#define LOAD(table, name) PFN_vk##name name = (PFN_vk##name)getproc(table, "vk" #name)

static bool init(struct gm_gpuwarm *w) {
	w->lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (w->lib == NULL) {
		return false;
	}
	PFN_vkGetInstanceProcAddr getproc = (PFN_vkGetInstanceProcAddr)dlsym(w->lib, "vkGetInstanceProcAddr");
	if (getproc == NULL) {
		return false;
	}
	LOAD(NULL, CreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "gmstream gpuwarm",
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	if (CreateInstance(&ici, NULL, &w->instance) != VK_SUCCESS) {
		return false;
	}
	VkInstance in = w->instance;
	w->DestroyInstance = (PFN_vkDestroyInstance)getproc(in, "vkDestroyInstance");
	LOAD(in, EnumeratePhysicalDevices);
	LOAD(in, GetPhysicalDeviceQueueFamilyProperties);
	LOAD(in, GetPhysicalDeviceMemoryProperties);
	LOAD(in, CreateDevice);
	LOAD(in, GetDeviceProcAddr);
	uint32_t n = 1;
	VkPhysicalDevice gpu;
	if (EnumeratePhysicalDevices(in, &n, &gpu) < 0 || n == 0) {
		return false;
	}
	uint32_t qn = 16;
	VkQueueFamilyProperties qp[16];
	GetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qp);
	uint32_t qf = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++) {
		if ((qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
			qf = i;
			break;
		}
	}
	if (qf == UINT32_MAX) {
		return false;
	}
	float one = 1.0f;
	VkDeviceQueueGlobalPriorityCreateInfoKHR prio = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR,
		.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR };
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .pNext = &prio,
		.queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &one };
	const char *exts[] = { "VK_KHR_global_priority" };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts };
	if (CreateDevice(gpu, &dci, NULL, &w->device) != VK_SUCCESS) {
		// Without the extension: the default priority, still below the encoder's.
		qci.pNext = NULL;
		dci.enabledExtensionCount = 0;
		if (CreateDevice(gpu, &dci, NULL, &w->device) != VK_SUCCESS) {
			return false;
		}
	}
	VkDevice d = w->device;
	PFN_vkGetDeviceProcAddr dgetproc = GetDeviceProcAddr;
#define DLOAD(name) PFN_vk##name name = (PFN_vk##name)dgetproc(d, "vk" #name)
	DLOAD(GetDeviceQueue);
	DLOAD(CreateBuffer);
	DLOAD(GetBufferMemoryRequirements);
	DLOAD(AllocateMemory);
	DLOAD(BindBufferMemory);
	DLOAD(CreateDescriptorSetLayout);
	DLOAD(CreatePipelineLayout);
	DLOAD(CreateShaderModule);
	DLOAD(CreateComputePipelines);
	DLOAD(CreateDescriptorPool);
	DLOAD(AllocateDescriptorSets);
	DLOAD(UpdateDescriptorSets);
	DLOAD(CreateCommandPool);
	DLOAD(AllocateCommandBuffers);
	DLOAD(BeginCommandBuffer);
	DLOAD(CmdBindPipeline);
	DLOAD(CmdBindDescriptorSets);
	DLOAD(CmdDispatch);
	DLOAD(EndCommandBuffer);
	DLOAD(CreateFence);
	w->DestroyDevice = (PFN_vkDestroyDevice)dgetproc(d, "vkDestroyDevice");
	w->QueueSubmit = (PFN_vkQueueSubmit)dgetproc(d, "vkQueueSubmit");
	w->WaitForFences = (PFN_vkWaitForFences)dgetproc(d, "vkWaitForFences");
	w->ResetFences = (PFN_vkResetFences)dgetproc(d, "vkResetFences");
	w->DeviceWaitIdle = (PFN_vkDeviceWaitIdle)dgetproc(d, "vkDeviceWaitIdle");
	GetDeviceQueue(d, qf, 0, &w->queue);

	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buffer;
	if (CreateBuffer(d, &bci, NULL, &buffer) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements mr;
	GetBufferMemoryRequirements(d, buffer, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = (uint32_t)__builtin_ctz(mr.memoryTypeBits) };
	VkDeviceMemory memory;
	if (AllocateMemory(d, &mai, NULL, &memory) != VK_SUCCESS || BindBufferMemory(d, buffer, memory, 0) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &binding };
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
		.pSetLayouts = &set_layout };
	if (CreateDescriptorSetLayout(d, &lci, NULL, &set_layout) != VK_SUCCESS ||
			CreatePipelineLayout(d, &plci, NULL, &layout) != VK_SUCCESS) {
		return false;
	}
	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(warm_spv),
		.pCode = warm_spv };
	VkShaderModule module;
	if (CreateShaderModule(d, &smci, NULL, &module) != VK_SUCCESS) {
		return false;
	}
	VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module, .pName = "main" },
		.layout = layout };
	VkPipeline pipeline;
	if (CreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorSetCount = 1, .pSetLayouts = &set_layout };
	if (CreateDescriptorPool(d, &dpci, NULL, &pool) != VK_SUCCESS) {
		return false;
	}
	dsai.descriptorPool = pool;
	if (AllocateDescriptorSets(d, &dsai, &set) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorBufferInfo dbi = { buffer, 0, 256 };
	VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
	UpdateDescriptorSets(d, 1, &write, 0, NULL);
	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
	VkCommandPool cpool;
	if (CreateCommandPool(d, &cpi, NULL, &cpool) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	if (AllocateCommandBuffers(d, &cai, &w->cmd) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	BeginCommandBuffer(w->cmd, &begin);
	CmdBindPipeline(w->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	CmdBindDescriptorSets(w->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	CmdDispatch(w->cmd, 4, 1, 1);
	if (EndCommandBuffer(w->cmd) != VK_SUCCESS) {
		return false;
	}
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	return CreateFence(d, &fci, NULL, &w->fence) == VK_SUCCESS;
}

struct gm_gpuwarm *gpuwarm_create(void) {
	const char *env = getenv("GM_RIPTIDE_GPU_WARM");
	if (env != NULL && strcmp(env, "0") == 0) {
		return NULL;
	}
	struct gm_gpuwarm *w = calloc(1, sizeof(*w));
	if (!init(w)) {
		wlr_log(WLR_INFO, "GPU warmer unavailable");
		gpuwarm_destroy(w);
		return NULL;
	}
	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->cond, NULL);
	pthread_create(&w->thread, NULL, run, w);
	return w;
}

void gpuwarm_destroy(struct gm_gpuwarm *w) {
	if (w == NULL) {
		return;
	}
	if (w->thread) {
		pthread_mutex_lock(&w->lock);
		w->quit = true;
		pthread_cond_signal(&w->cond);
		pthread_mutex_unlock(&w->lock);
		pthread_join(w->thread, NULL);
	}
	if (w->device != VK_NULL_HANDLE) {
		w->DeviceWaitIdle(w->device);
		w->DestroyDevice(w->device, NULL); // its objects go with it
	}
	if (w->instance != VK_NULL_HANDLE) {
		w->DestroyInstance(w->instance, NULL);
	}
	if (w->lib != NULL) {
		dlclose(w->lib);
	}
	free(w);
}
