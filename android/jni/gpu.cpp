#include "gpu.h"

#include "log.h"

using namespace Vulkan;

namespace gm {

bool Gpu::init()
{
	std::lock_guard<std::mutex> g(lock_);
	if (tried_)
		return ok_;
	tried_ = true;
	if (!Context::init_loader(nullptr)) {
		LOGW("gpu: no Vulkan loader");
		return false;
	}
	context.set_num_thread_indices(1);
	context.set_system_handles({});
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	app.pApplicationName = "gmstream";
	app.pEngineName = "Granite";
	context.set_application_info(&app);
	static const char *dev_ext[] = {
		VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
		VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
	};
	// The compute queue at high priority (Granite falls back to medium where
	// the system refuses it): the compositor's queue.
	if (!context.init_instance_and_device(nullptr, 0, dev_ext, 3, CONTEXT_CREATION_ENABLE_COMPUTE_HIGH_GLOBAL_PRIORITY_BIT)) {
		LOGW("gpu: cannot create a Vulkan device");
		return false;
	}
	ContextOptions opts = {};
	opts.memory_priorities = false;
	opts.lean_memory_mode = true;
	dev = std::make_unique<Device>();
	dev->set_context(context, opts);
	VkDevice d = dev->get_device();
	get_ahb_props = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
		vkGetDeviceProcAddr(d, "vkGetAndroidHardwareBufferPropertiesANDROID"));
	get_semaphore_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(d, "vkGetSemaphoreFdKHR"));
	import_semaphore_fd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(d, "vkImportSemaphoreFdKHR"));
	ycbcr = dev->get_device_features().vk11_features.samplerYcbcrConversion;
	ok_ = get_ahb_props && get_semaphore_fd && import_semaphore_fd;
	auto &props = dev->get_gpu_properties();
	auto &q = context.get_queue_info();
	LOGI("gpu: %s, Vulkan %u.%u, graphics family %u, compute family %u%s%s", props.deviceName,
	     VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), q.family_indices[QUEUE_INDEX_GRAPHICS],
	     q.family_indices[QUEUE_INDEX_COMPUTE], ycbcr ? "" : ", no YCbCr sampling",
	     ok_ ? "" : " (no AHardwareBuffer/sync fd interop)");
	return ok_;
}

VkQueue Gpu::compute_queue() const { return context.get_queue_info().queues[QUEUE_INDEX_COMPUTE]; }
uint32_t Gpu::compute_family() const { return context.get_queue_info().family_indices[QUEUE_INDEX_COMPUTE]; }
uint32_t Gpu::graphics_family() const { return context.get_queue_info().family_indices[QUEUE_INDEX_GRAPHICS]; }

} // namespace gm
