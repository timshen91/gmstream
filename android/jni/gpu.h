// The one Vulkan device the client uses (Granite's, so Riptide's decoder runs
// on it): Riptide decodes on its graphics queue; the scanout compositor
// (compositor.h) submits on its compute queue, a queue family of its own
// asked for at high priority, so a compose is not stuck behind a decode.

#pragma once

#include <memory>
#include <mutex>

#include "context.hpp"
#include "device.hpp"

namespace gm {

struct Gpu {
	Vulkan::Context context;
	std::unique_ptr<Vulkan::Device> dev;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_props = nullptr;
	PFN_vkGetSemaphoreFdKHR get_semaphore_fd = nullptr;
	PFN_vkImportSemaphoreFdKHR import_semaphore_fd = nullptr;
	bool ycbcr = false; // sampler YCbCr conversion (the video decoder's pictures)

	// Made the first time; false if there is no usable device.
	bool init();
	bool ok() const { return ok_; }
	bool tried() const { return tried_; }
	VkDevice device() const { return dev->get_device(); }
	VkQueue compute_queue() const;
	uint32_t compute_family() const;
	uint32_t graphics_family() const;

private:
	std::mutex lock_;
	bool tried_ = false, ok_ = false;
};

} // namespace gm
