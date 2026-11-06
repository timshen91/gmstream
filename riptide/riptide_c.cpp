// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT

#include "context.hpp"
#include "device.hpp"
#include "image.hpp"
#include "buffer.hpp"
#include "riptide.h"
#include "riptide_decoder.hpp"
#include "riptide_encoder.hpp"
#include "logging.hpp"
#include "slangmosh_scaler.hpp"
#include "scaler.hpp"
#include "thread_id.hpp"
#include <vector>
#include <algorithm>
#include <time.h>

using namespace Granite;
using namespace Vulkan;
using namespace Riptide;

struct NullLogger : Util::LoggingInterface
{
	bool log(const char *, const char *, va_list) override
	{
#ifdef VULKAN_DEBUG
		return false;
#else
		return true;
#endif
	}
};

static NullLogger null_logger;

extern "C" {
void riptide_get_api_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
	*major = RIPTIDE_API_VERSION_MAJOR;
	*minor = RIPTIDE_API_VERSION_MINOR;
	*patch = RIPTIDE_API_VERSION_PATCH;
}

struct riptide_device_opaque
{
	Context context;
	Device device;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	CommandBuffer::Type queue_type = CommandBuffer::Type::Generic;
};

void riptide_device_set_command_buffer(riptide_device device, VkCommandBuffer cmd)
{
	device->cmd = cmd;
}

VkQueueGlobalPriority riptide_device_get_global_priority(riptide_device device)
{
	return device->device.get_device_features().global_compute_priority;
}

riptide_result riptide_create_device_by_compat(
		// If non-zero, needs to match VkPhysicalDeviceProperties::vendorID/deviceID.
		// Risks picking the wrong device if there are multiple ICDs for the same GPU.
		uint32_t vid, uint32_t pid,
		const riptide_uuid *device_uuid, // If non-NULL, needs to match VkPhysicalDeviceIDProperties::deviceUUID
		const riptide_uuid *driver_uuid, // If non-NULL, needs to match VkPhysicalDeviceIDProperties::driverUUID
		const riptide_luid *device_luid, // If non-NULL, needs to match VkPhysicalDeviceIDProperties::deviceLUID
		VkQueueGlobalPriority global_priority,
		riptide_device *device)
{
	// TODO: Find a better way to do this.
	Util::set_thread_logging_interface(&null_logger);

	if (!Context::init_loader(nullptr))
		return RIPTIDE_ERROR_NO_VULKAN;

	auto *dev = new riptide_device_opaque();
	dev->context.set_num_thread_indices(1);
	dev->context.set_system_handles({});

	VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.apiVersion = VK_API_VERSION_1_1;
	app_info.pApplicationName = "riptide-c";
	app_info.pEngineName = "Granite";
	dev->context.set_application_info(&app_info);

	// Just enable video extensions so that we can use video image usage, but don't bother creating queues for it, etc.
	if (!dev->context.init_instance(nullptr, 0, CONTEXT_CREATION_ENABLE_VIDEO_FEATURE_ONLY_BIT))
	{
		delete dev;
		return RIPTIDE_ERROR_NO_VULKAN;
	}

	uint32_t count;
	if (vkEnumeratePhysicalDevices(dev->context.get_instance(), &count, nullptr) != VK_SUCCESS)
	{
		delete dev;
		return RIPTIDE_ERROR_NO_VULKAN;
	}

	std::vector<VkPhysicalDevice> gpus(count);

	if (vkEnumeratePhysicalDevices(dev->context.get_instance(), &count, gpus.data()) < 0)
	{
		delete dev;
		return RIPTIDE_ERROR_NO_VULKAN;
	}

	VkPhysicalDevice selected_gpu = VK_NULL_HANDLE;

	for (auto &gpu : gpus)
	{
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(gpu, &props);
		// Is this even possible these days?
		if (props.apiVersion < VK_API_VERSION_1_1)
			continue;

		VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids };
		vkGetPhysicalDeviceProperties2(gpu, &props2);

		if (vid && props2.properties.vendorID != vid)
			continue;
		if (pid && props2.properties.deviceID != pid)
			continue;

		if (device_uuid && memcmp(device_uuid, ids.deviceUUID, VK_UUID_SIZE) != 0)
			continue;
		if (driver_uuid && memcmp(driver_uuid, ids.driverUUID, VK_UUID_SIZE) != 0)
			continue;
		if (device_luid && !ids.deviceLUIDValid)
			continue;
		if (device_luid && memcmp(device_luid, ids.deviceLUID, VK_LUID_SIZE) != 0)
			continue;

		ContextCreationFlags flags = CONTEXT_CREATION_ENABLE_VIDEO_FEATURE_ONLY_BIT;
		if (global_priority == VK_QUEUE_GLOBAL_PRIORITY_REALTIME)
			flags |= CONTEXT_CREATION_ENABLE_COMPUTE_REALTIME_GLOBAL_PRIORITY_BIT;
		else if (global_priority == VK_QUEUE_GLOBAL_PRIORITY_HIGH)
			flags |= CONTEXT_CREATION_ENABLE_COMPUTE_HIGH_GLOBAL_PRIORITY_BIT;

		if (dev->context.init_device(gpu, VK_NULL_HANDLE, nullptr, 0, flags))
		{
			selected_gpu = gpu;
			break;
		}
	}

	if (!selected_gpu)
	{
		delete dev;
		return RIPTIDE_ERROR_NO_VULKAN;
	}

	ContextOptions context_opts = {};
	context_opts.memory_priorities = false;
	context_opts.lean_memory_mode = true;

	dev->device.set_context(dev->context, context_opts);
	*device = dev;

	if (global_priority > VK_QUEUE_GLOBAL_PRIORITY_MEDIUM)
		riptide_device_set_queue_type(dev, VK_QUEUE_COMPUTE_BIT);

	return RIPTIDE_SUCCESS;
}

riptide_result riptide_create_default_device(riptide_device *device)
{
	return riptide_create_device_by_compat(0, 0, nullptr, nullptr, nullptr, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM, device);
}

static std::mutex global_device_lock;

riptide_result riptide_create_device(const riptide_device_create_info *info, riptide_device *out_device)
{
	// TODO: Find a better way to do this.
	Util::set_thread_logging_interface(&null_logger);

	// Safety against concurrent device creations since we're setting global function pointer state here.
	std::lock_guard<std::mutex> holder{global_device_lock};

	if (!Context::init_loader(info->GetInstanceProcAddr))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	struct MyInstanceFactory : InstanceFactory
	{
		const riptide_device_create_info *info = nullptr;

		VkInstance create_instance(const VkInstanceCreateInfo *) override
		{
			return info->instance;
		}

		// Lifetime of any data in create info must remain as long as Context is alive.
		const VkInstanceCreateInfo *get_existing_create_info() override
		{
			return info->instance_create_info;
		}

		bool factory_owns_created_instance() override
		{
			return true;
		}
	} instance;

	struct MyDeviceFactory : DeviceFactory
	{
		const riptide_device_create_info *info = nullptr;

		VkDevice create_device(VkPhysicalDevice, const VkDeviceCreateInfo *) override
		{
			return info->device;
		}

		const VkDeviceCreateInfo *get_existing_create_info() override
		{
			return info->device_create_info;
		}

		bool factory_owns_created_device() override
		{
			return true;
		}

		VkQueue get_queue(uint32_t family_index, uint32_t index) override
		{
			for (uint32_t i = 0; i < info->queue_info_count; i++)
				if (info->queue_info[i].familyIndex == family_index && info->queue_info[i].index == index)
					return info->queue_info[i].queue;
			return VK_NULL_HANDLE;
		}
	} device;

	instance.info = info;
	device.info = info;

	auto dev = new riptide_device_opaque;
	dev->context.set_instance_factory(&instance);
	dev->context.set_device_factory(&device);

	if (!dev->context.init_instance(nullptr, 0))
		return RIPTIDE_ERROR_NO_VULKAN;

	if (!dev->context.init_device(info->physical_device, VK_NULL_HANDLE, nullptr, 0))
		return RIPTIDE_ERROR_NO_VULKAN;

	ContextOptions context_opts = {};
	context_opts.memory_priorities = false;
	context_opts.lean_memory_mode = true;
	dev->device.set_context(dev->context, context_opts);

	dev->device.set_queue_lock(
		[cb = info->queue_lock_callback, userdata = info->userdata]() {
			if (cb)
				cb(userdata);
		},
		[cb = info->queue_unlock_callback, userdata = info->userdata]() {
			if (cb)
				cb(userdata);
		});

	*out_device = dev;
	return RIPTIDE_SUCCESS;
}

void riptide_device_report_performance_stats(riptide_device device, riptide_message_cb cb, void *userdata, bool reset)
{
	Util::set_thread_logging_interface(&null_logger);

	device->device.timestamp_log([=](const std::string &tag, const TimestampIntervalReport &report)
	{
		char msg[256];
		snprintf(msg, sizeof(msg), "%s: %.3f ms per frame", tag.c_str(), report.time_per_frame_context * 1e3);
		cb(userdata, msg);
	});

	if (device->device.get_device_features().supports_memory_budget)
	{
		HeapBudget budgets[VK_MAX_MEMORY_HEAPS];
		device->device.get_memory_budget(budgets);
		for (uint32_t i = 0; i < device->device.get_memory_properties().memoryHeapCount; i++)
		{
			char msg[256];
			snprintf(msg, sizeof(msg), "Memory Heap %u (%s): "
			         "MaxSize %.3f MiB, BudgetSize %.3f MiB, DeviceUsage %.3f MiB, TrackedUsage %.3f MiB", i,
			         (device->device.get_memory_properties().memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
				         ? "DEVICE"
				         : "HOST",
			         float(budgets[i].max_size) / (1024.0f * 1024.0f),
			         float(budgets[i].budget_size) / (1024.0f * 1024.0f),
			         float(budgets[i].device_usage) / (1024.0f * 1024.0f),
			         float(budgets[i].tracked_usage) / (1024.0f * 1024.0f));
			cb(userdata, msg);
		}
	}

	if (reset)
		device->device.timestamp_log_reset();
}

void riptide_device_get_vk_device_handles(
	riptide_device device,
	VkInstance *vk_instance, VkPhysicalDevice *vk_physical_device,
	VkDevice *vk_device)
{
	Util::set_thread_logging_interface(&null_logger);

	if (vk_instance)
		*vk_instance = device->device.get_instance();
	if (vk_physical_device)
		*vk_physical_device = device->device.get_physical_device();
	if (vk_device)
		*vk_device = device->device.get_device();
}

static bool riptide_device_confirm_external_semaphore_support(riptide_device device)
{
	VkSemaphoreTypeCreateInfo type_info = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
	VkPhysicalDeviceExternalSemaphoreInfo sem_info =
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &type_info };
	VkExternalSemaphoreProperties sem_props = { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };

#ifdef _WIN32
	sem_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
	sem_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

	type_info.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
	vkGetPhysicalDeviceExternalSemaphoreProperties(device->device.get_physical_device(), &sem_info, &sem_props);
	if (!(sem_props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
		return false;

	type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	vkGetPhysicalDeviceExternalSemaphoreProperties(device->device.get_physical_device(), &sem_info, &sem_props);
	if (!(sem_props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
		return false;

#ifdef _WIN32
	// Despite being a timeline, D3D12_FENCE was added before TIMELINE was added, and AMD drivers have
	// at least gotten confused when trying to use TIMELINE type here in the past.
	sem_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
	sem_info.pNext = nullptr;
	vkGetPhysicalDeviceExternalSemaphoreProperties(device->device.get_physical_device(), &sem_info, &sem_props);
	if (!(sem_props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
		return false;
#endif

	return true;
}

static bool riptide_device_confirm_external_memory_support(riptide_device device)
{
	VkExternalImageFormatProperties external_props = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
	VkImageFormatProperties2 props2 = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &external_props };
	VkPhysicalDeviceExternalImageFormatInfo external_format_info =
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };

	// TODO: Android?
#ifndef _WIN32
	if (!device->device.get_device_features().supports_drm_modifiers)
		return false;
#endif

	static const VkExternalMemoryHandleTypeFlagBits required_external_types[] = {
#ifdef _WIN32
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
#else
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
#endif
	};

	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info =
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT };

	for (auto type : required_external_types)
	{
		external_format_info.handleType = type;
		external_format_info.pNext = nullptr;

		if (type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
		{
			VkDrmFormatModifierPropertiesEXT mod;
			VkDrmFormatModifierPropertiesListEXT modifier_list =
				{ VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
			modifier_list.drmFormatModifierCount = 1;
			modifier_list.pDrmFormatModifierProperties = &mod;
			VkFormatProperties3 props3 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3, &modifier_list };
			device->device.get_format_properties(VK_FORMAT_R8_UNORM, &props3);

			if (modifier_list.drmFormatModifierCount != 1)
				return false;

			modifier_info.drmFormatModifier = mod.drmFormatModifier;
			modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			external_format_info.pNext = &modifier_info;
		}

		if (!device->device.get_image_format_properties(
			VK_FORMAT_R8_UNORM, VK_IMAGE_TYPE_2D, type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ?
			VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			0, &external_format_info, &props2))
			return false;

		if (!(external_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
			return false;
	}

	return true;
}

bool riptide_device_confirm_interop_support(riptide_device device)
{
	Util::set_thread_logging_interface(&null_logger);
	if (!device->device.get_device_features().supports_external)
		return false;
	if (!riptide_device_confirm_external_semaphore_support(device))
		return false;
	if (!riptide_device_confirm_external_memory_support(device))
		return false;

	return true;
}

riptide_result
riptide_device_set_queue_type(riptide_device device, VkQueueFlagBits queue_flags)
{
	if (!device)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (queue_flags != VK_QUEUE_GRAPHICS_BIT && queue_flags != VK_QUEUE_COMPUTE_BIT)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	device->queue_type = queue_flags == VK_QUEUE_GRAPHICS_BIT ? CommandBuffer::Type::Generic : CommandBuffer::Type::AsyncCompute;
	return RIPTIDE_SUCCESS;
}

void riptide_device_destroy(riptide_device device)
{
	Util::set_thread_logging_interface(&null_logger);

	delete device;
}

struct riptide_sync_object_opaque
{
	Device *device = nullptr;
	Semaphore semaphore;
};

riptide_result
riptide_sync_object_create(const riptide_sync_object_create_info *info, riptide_sync_object *out_sync)
{
	Util::set_thread_logging_interface(&null_logger);

	if (!info->device)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if ((info->handle_type & (
		VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
		VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
		VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT |
		VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT |
		VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)) == 0)
	{
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	auto &device = info->device->device;

	if (!device.get_device_features().supports_external)
		return RIPTIDE_ERROR_NOT_IMPLEMENTED;

	auto sem = device.request_semaphore_external(info->semaphore_type, info->handle_type);
	if (!sem)
		return RIPTIDE_ERROR_UNSUPPORTED_EXTERNAL_HANDLE;

	ExternalHandle ext = {};
	ext.handle = (decltype(ext.handle))info->external_handle;
	ext.semaphore_handle_type = info->handle_type;
	if (ext && !sem->import_from_handle(ext))
		return RIPTIDE_ERROR_FAILED_EXTERNAL_HANDLE;

	if (!ext && !(info->import_flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto *sync = new riptide_sync_object_opaque();
	sync->device = &device;
	sync->semaphore = std::move(sem);

	*out_sync = sync;
	return RIPTIDE_SUCCESS;
}

VkSemaphore
riptide_sync_object_get_semaphore(riptide_sync_object sync)
{
	if (!sync)
		return VK_NULL_HANDLE;

	Util::set_thread_logging_interface(&null_logger);
	return sync->semaphore->get_semaphore();
}

riptide_result
riptide_sync_object_export_handle(riptide_sync_object sync, riptide_os_handle *handle)
{
	if (!sync)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	Util::set_thread_logging_interface(&null_logger);
	if (auto native_handle = sync->semaphore->export_to_handle())
	{
		*handle = (riptide_os_handle)native_handle.handle;
		return RIPTIDE_SUCCESS;
	}
	else
	{
		return RIPTIDE_ERROR_FAILED_EXTERNAL_HANDLE;
	}
}

riptide_result
riptide_sync_object_cpu_wait(riptide_sync_object sync, uint64_t value, uint64_t timeout)
{
	if (!sync || sync->semaphore->get_semaphore_type() != VK_SEMAPHORE_TYPE_TIMELINE)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	Util::set_thread_logging_interface(&null_logger);
	return sync->semaphore->wait_timeline_timeout(value, timeout) ? RIPTIDE_SUCCESS : RIPTIDE_TIMEOUT;
}

riptide_result
riptide_sync_object_cpu_signal(riptide_sync_object sync, uint64_t value)
{
	if (!sync || sync->semaphore->get_semaphore_type() != VK_SEMAPHORE_TYPE_TIMELINE)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	Util::set_thread_logging_interface(&null_logger);
	auto &table = sync->device->get_device_table();

	VkSemaphoreSignalInfo signal_info = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
	signal_info.semaphore = sync->semaphore->get_semaphore();
	signal_info.value = value;
	VkResult vr;

	if (table.vkSignalSemaphore)
		vr = table.vkSignalSemaphore(sync->device->get_device(), &signal_info);
	else if (table.vkSignalSemaphoreKHR)
		vr = table.vkSignalSemaphoreKHR(sync->device->get_device(), &signal_info);
	else
		return RIPTIDE_ERROR_GENERIC;

	return vr == VK_SUCCESS ? RIPTIDE_SUCCESS : RIPTIDE_ERROR_GENERIC;
}

void riptide_sync_object_destroy(riptide_sync_object sync)
{
	auto *device = sync->device;
	Util::set_thread_logging_interface(&null_logger);
	delete sync;
	device->next_frame_context();
}

struct riptide_image_opaque
{
	Device *device = nullptr;
	ImageHandle img;
};

riptide_result riptide_image_create(const riptide_image_create_info *info, riptide_image *out_image)
{
	Util::set_thread_logging_interface(&null_logger);
	if (!info->device || !info->image_create_info)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto &device = info->device->device;

	if (info->image_create_info->sharingMode != VK_SHARING_MODE_EXCLUSIVE)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (info->handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
		info->image_create_info->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (info->handle_type != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
		info->image_create_info->tiling != VK_IMAGE_TILING_OPTIMAL)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (info->image_create_info->imageType != VK_IMAGE_TYPE_2D)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	ImageCreateInfo image_create_info = {};
	image_create_info.domain = ImageDomain::Physical;
	image_create_info.misc = IMAGE_MISC_EXTERNAL_MEMORY_BIT | IMAGE_MISC_NO_DEFAULT_VIEWS_BIT;
	image_create_info.external.handle = (decltype(image_create_info.external.handle))info->external_handle;
	image_create_info.external.memory_handle_type = info->handle_type;
	image_create_info.pnext = const_cast<void *>(info->image_create_info->pNext);
	image_create_info.layout = ImageLayout::General;
	image_create_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;

	image_create_info.type = info->image_create_info->imageType;
	image_create_info.format = info->image_create_info->format;
	image_create_info.flags = info->image_create_info->flags;
	image_create_info.width = info->image_create_info->extent.width;
	image_create_info.height = info->image_create_info->extent.height;
	image_create_info.depth = info->image_create_info->extent.depth;
	image_create_info.layers = info->image_create_info->arrayLayers;
	image_create_info.levels = info->image_create_info->mipLevels;
	image_create_info.samples = info->image_create_info->samples;
	image_create_info.usage = info->image_create_info->usage;

	if (device.get_device_features().driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY &&
	    (info->handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT ||
	     info->handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT))
	{
		VkFormatProperties3 format_properties = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
		device.get_format_properties(image_create_info.format, &format_properties);

		if (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR)
		{
			// NVIDIA workaround. For planar formats, the D3D side assumes video compatible layouts.
			image_create_info.usage |= VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;

			// If we're on an older driver, just pass it through as-is.
			// Normally we have to pass down a codec profile, but this is mostly noise.
			if (device.get_device_features().video_maintenance1_features.videoMaintenance1)
				image_create_info.flags |= VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
		}
	}

	auto img = device.create_image(image_create_info);
	if (!img)
		return RIPTIDE_ERROR_FAILED_EXTERNAL_HANDLE;

	auto *image = new riptide_image_opaque();
	image->device = &device;
	image->img = std::move(img);

	*out_image = image;
	return RIPTIDE_SUCCESS;
}

VkImage riptide_image_get_handle(riptide_image image)
{
	Util::set_thread_logging_interface(&null_logger);
	return image->img->get_image();
}

riptide_result
riptide_image_get_image_view(riptide_image image, VkImageAspectFlagBits aspect,
							  VkImageUsageFlagBits usage, riptide_image_view *view)
{
	Util::set_thread_logging_interface(&null_logger);

	if ((aspect & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT |
	               VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)) == 0)
	{
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	if (usage != VK_IMAGE_USAGE_SAMPLED_BIT && usage != VK_IMAGE_USAGE_STORAGE_BIT)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto &img = *image->img;

	*view = {};
	view->image = img.get_image();
	view->image_format = img.get_format();
	view->width = img.get_width();
	view->height = img.get_height();

	// Imported images just use GENERAL.
	view->layout = VK_IMAGE_LAYOUT_GENERAL;

	if (aspect == VK_IMAGE_ASPECT_COLOR_BIT)
	{
		view->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		view->swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
		view->view_format = img.get_format();

		if (view->view_format == VK_FORMAT_R8G8B8A8_SRGB)
			view->view_format = VK_FORMAT_R8G8B8A8_UNORM;
		else if (view->view_format == VK_FORMAT_B8G8R8A8_SRGB)
			view->view_format = VK_FORMAT_B8G8R8A8_UNORM;
		else if (view->view_format == VK_FORMAT_A8B8G8R8_SRGB_PACK32)
			view->view_format = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

		return RIPTIDE_SUCCESS;
	}

	// Handle the usual suspects.
	switch (img.get_format())
	{
	// Normal explicit planar formats.
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R16_UNORM:
		view->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		view->swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
		view->view_format = img.get_format();
		break;

	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R16G16_UNORM:
		if (usage == VK_IMAGE_USAGE_STORAGE_BIT)
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		if (aspect == VK_IMAGE_ASPECT_PLANE_0_BIT)
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		view->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		view->swizzle = aspect == VK_IMAGE_ASPECT_PLANE_2_BIT ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_R;
		view->view_format = img.get_format();
		break;

	// Special 4:4:4 HDR10 format
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		if (usage == VK_IMAGE_USAGE_STORAGE_BIT)
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		view->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		view->view_format = img.get_format();

		switch (aspect)
		{
		case VK_IMAGE_ASPECT_PLANE_0_BIT: view->swizzle = VK_COMPONENT_SWIZZLE_R; break;
		case VK_IMAGE_ASPECT_PLANE_1_BIT: view->swizzle = VK_COMPONENT_SWIZZLE_G; break;
		case VK_IMAGE_ASPECT_PLANE_2_BIT: view->swizzle = VK_COMPONENT_SWIZZLE_B; break;
		default: return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}
		break;

	// 3-plane YCbCr
	case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
	case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
		view->view_format = VK_FORMAT_R8_UNORM;
		view->aspect = aspect;
		break;

	case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
	case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
		view->view_format = VK_FORMAT_R10X6_UNORM_PACK16;
		view->aspect = aspect;
		break;

	case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
	case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
		view->view_format = VK_FORMAT_R16_UNORM;
		view->aspect = aspect;
		break;

	// 2-plane YCbCr
	case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
	case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:
		switch (aspect)
		{
		case VK_IMAGE_ASPECT_PLANE_0_BIT:
			view->view_format = VK_FORMAT_R8_UNORM;
			view->aspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
			break;

		case VK_IMAGE_ASPECT_PLANE_1_BIT:
			if (usage == VK_IMAGE_USAGE_STORAGE_BIT)
				return RIPTIDE_ERROR_INVALID_ARGUMENT;
			view->swizzle = VK_COMPONENT_SWIZZLE_R;
			view->view_format = VK_FORMAT_R8G8_UNORM;
			view->aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
			break;

		case VK_IMAGE_ASPECT_PLANE_2_BIT:
			if (usage == VK_IMAGE_USAGE_STORAGE_BIT)
				return RIPTIDE_ERROR_INVALID_ARGUMENT;
			view->swizzle = VK_COMPONENT_SWIZZLE_G;
			view->view_format = VK_FORMAT_R8G8_UNORM;
			view->aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
			break;

		default:
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}
		break;

	case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
	case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:
		switch (aspect)
		{
		case VK_IMAGE_ASPECT_PLANE_0_BIT:
			view->view_format = VK_FORMAT_R10X6_UNORM_PACK16;
			view->aspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
			break;

		case VK_IMAGE_ASPECT_PLANE_1_BIT:
			if (usage == VK_IMAGE_USAGE_STORAGE_BIT)
				return RIPTIDE_ERROR_INVALID_ARGUMENT;
			view->swizzle = VK_COMPONENT_SWIZZLE_R;
			view->view_format = VK_FORMAT_R10X6G10X6_UNORM_2PACK16;
			view->aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
			break;

		case VK_IMAGE_ASPECT_PLANE_2_BIT:
			if (usage == VK_IMAGE_USAGE_STORAGE_BIT)
				return RIPTIDE_ERROR_INVALID_ARGUMENT;
			view->swizzle = VK_COMPONENT_SWIZZLE_G;
			view->view_format = VK_FORMAT_R10X6G10X6_UNORM_2PACK16;
			view->aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
			break;

		default:
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}
		break;

	case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
	case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM:
		switch (aspect)
		{
		case VK_IMAGE_ASPECT_PLANE_0_BIT:
			view->view_format = VK_FORMAT_R16_UNORM;
			view->aspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
			break;

		case VK_IMAGE_ASPECT_PLANE_1_BIT:
			if (usage == VK_IMAGE_USAGE_STORAGE_BIT)
				return RIPTIDE_ERROR_INVALID_ARGUMENT;
			view->swizzle = VK_COMPONENT_SWIZZLE_R;
			view->view_format = VK_FORMAT_R16G16_UNORM;
			view->aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
			break;

		case VK_IMAGE_ASPECT_PLANE_2_BIT:
			if (usage == VK_IMAGE_USAGE_STORAGE_BIT)
				return RIPTIDE_ERROR_INVALID_ARGUMENT;
			view->swizzle = VK_COMPONENT_SWIZZLE_G;
			view->view_format = VK_FORMAT_R16G16_UNORM;
			view->aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
			break;

		default:
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}
		break;

	default:
		return RIPTIDE_ERROR_NOT_IMPLEMENTED;
	}

	return RIPTIDE_SUCCESS;
}

void riptide_image_destroy(riptide_image image)
{
	auto *device = image->device;
	Util::set_thread_logging_interface(&null_logger);
	delete image;

	// Pump frame contexts through to make sure memory gets freed eventually.
	device->next_frame_context();
}

struct riptide_encoder_opaque
{
	Device *device = nullptr;
	riptide_device rt_device = nullptr;
	Encoder encoder;

	ChromaSubsampling chroma = {};
	int width = 0;
	int height = 0;
	float ycbcr_scaling_factor = 1.0f;

	// For scaling path.
	ImageHandle scaler_planes[3];
	ImageHandle turn_planes[3]; // the input scaled before it is turned (rotation)
	Program *turn_program = nullptr;
	VideoScaler scaler;
	QueryPoolHandle frame_start; // measuring: the scaled encode's first GPU command
	QueryPoolHandle host_start, stripe0_end; // measuring: the encode call (host), stripe 0's last GPU command

	int context = 0;

	struct
	{
		Fence queued_fence;
		BufferHandle queued_meta;
		BufferHandle queued_meta_gpu;
		BufferHandle queued_bitstream;
		BufferHandle queued_bitstream_gpu;
		ImageHandle upload_images[3];
		Semaphore upload_semaphore;
	} contexts[RIPTIDE_MAX_FRAME_CONTEXTS];

	// Inter frames (riptide_encoder_enable_inter, bitstream/inter.md). Each frame
	// is predicted from the encoder's own decode of the previous one; the last
	// frames are kept (their packets, what a decoder received of them, their
	// decode and the vectors after it) to replay a decoder's losses.
	//
	// The frame is coded in stripes (riptide_inter_info::stripe_rows):
	// horizontal bands, each coded as an image of its own (its own encoder,
	// packets and share of the rate) that predicts from the whole previous
	// frame. They are submitted to the GPU one after another, so each can be sent
	// as soon as the GPU has finished it, and decoded as soon as it has arrived.
	struct InterStripeRecord
	{
		std::vector<uint8_t> bytes;
		std::vector<riptide_packet> packets;
		std::vector<int16_t> mv_state; // the decoder's vectors after this stripe
	};
	struct InterFrame
	{
		uint32_t number = 0;
		bool valid = false, predicted = false;
		std::vector<InterStripeRecord> stripes;
		std::vector<uint8_t> received; // per packet, the stripes' in order
		ImageHandle planes[3];
	};
	struct InterStripe
	{
		int y = 0, height = 0;
		std::unique_ptr<Encoder> encoder;
		std::unique_ptr<Decoder> decoder; // the encoder's own decode, and replays
		ImageHandle source[3], decoded[3];
		BufferHandle meta_gpu, meta, bitstream_gpu, bitstream, stats;
		Fence fence;
		size_t budget = 0;
		bool stats_valid = false;
		bool decode_pending = false;
		std::vector<uint8_t> inter_bytes;
		std::vector<size_t> inter_sizes;
	};
	struct
	{
		bool enabled = false;
		int stripe_rows = 0, first_stripe_rows = 0;
		std::vector<InterStripe> stripes;
		std::vector<InterFrame> ring;
		uint32_t next_number = 0; // the next encode's
		bool intra_requested = true;
		bool predicted = false; // the last encode
		int packetized = 0; // stripes of the last encode handed out
	} inter;
};

riptide_result
riptide_encoder_create(const riptide_encoder_create_info *info, riptide_encoder *encoder)
{
	Util::set_thread_logging_interface(&null_logger);

	if (!info->device)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (info->width <= 0 || info->height <= 0)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (info->chroma == RIPTIDE_CHROMA_SUBSAMPLING_420 && (info->width % 2 || info->height % 2))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto *enc = new riptide_encoder_opaque();
	enc->rt_device = info->device;
	enc->device = &info->device->device;
	enc->chroma = ChromaSubsampling(info->chroma);
	enc->width = info->width;
	enc->height = info->height;

	if (!enc->encoder.init(&info->device->device, info->width, info->height, enc->chroma))
	{
		delete enc;
		return RIPTIDE_ERROR_GENERIC;
	}

	ResourceLayout layout;
	Shaders<> shaders(info->device->device, layout, 0);
	enc->scaler.set_program(shaders.scaler);

	static const uint32_t rotate_plane[] = {
#include "shaders/rotate_plane_comp.inc"
	};
	ResourceLayout turn = {};
	turn.sets[0].sampled_image_mask = 1;
	turn.sets[0].fp_mask = 1;
	turn.sets[0].storage_image_mask = 2;
	turn.sets[0].meta[0].array_size = 1;
	turn.sets[0].meta[1].array_size = 1;
	turn.push_constant_size = 12;
	enc->turn_program = info->device->device.request_program(rotate_plane, sizeof(rotate_plane), &turn);

	*encoder = enc;
	return RIPTIDE_SUCCESS;
}

struct WrappedViewBuffers : ViewBuffers
{
	ImageHandle wrapped_images[3];
	ImageViewHandle image_views[3];
	bool wrap(Device *device, const riptide_gpu_buffers *buffers, VkImageUsageFlags usage);
};

static bool wrap_view(Device *device, const riptide_image_view &view, ImageHandle &wrapped_image,
                      ImageViewHandle &wrapped_view,
                      VkImageUsageFlags usage)
{
	ImageCreateInfo image_info = {};
	image_info.usage = usage;
	image_info.type = VK_IMAGE_TYPE_2D;
	image_info.domain = ImageDomain::Physical;
	image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	image_info.width = view.width;
	image_info.height = view.height;
	image_info.format = view.image_format;

	// The exact numbers aren't important.
	image_info.layers = view.layer + 1;
	image_info.levels = view.mip_level + 1;

	image_info.layout = view.layout == VK_IMAGE_LAYOUT_GENERAL ? ImageLayout::General : ImageLayout::Optimal;
	wrapped_image = device->wrap_image(image_info, view.image);
	if (!wrapped_image)
		return false;

	ImageViewCreateInfo view_info = {};
	view_info.image = wrapped_image.get();
	view_info.format = view.view_format;
	view_info.view_type = VK_IMAGE_VIEW_TYPE_2D;
	view_info.layers = 1;
	view_info.levels = 1;
	view_info.base_level = view.mip_level;
	view_info.base_layer = view.layer;
	view_info.swizzle.r = view.swizzle;
	view_info.swizzle.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.swizzle.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.swizzle.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.aspect = view.aspect;
	wrapped_view = device->create_image_view(view_info);
	if (!wrapped_view)
		return false;

	return true;
}

bool WrappedViewBuffers::wrap(Device *device, const riptide_gpu_buffers *buffers, VkImageUsageFlags usage)
{
	for (int i = 0; i < 3; i++)
	{
		if (!wrap_view(device, buffers->planes[i], wrapped_images[i], image_views[i], usage))
			return false;
		planes[i] = image_views[i].get();
	}

	return true;
}

static void riptide_device_wait_semaphore(Device *device, CommandBuffer::Type queue_type, const riptide_gpu_sync_operation *acquire, VkPipelineStageFlags2 stages)
{
	if (acquire && acquire->sync.semaphore != VK_NULL_HANDLE)
	{
		auto sem = device->request_semaphore(
			acquire->sync.value != 0 ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY, acquire->sync.semaphore);
		sem->signal_external();

		if (acquire->sync.value)
		{
			sem = device->request_timeline_semaphore_as_binary(*sem, acquire->sync.value);
			sem->signal_external();
		}

		device->add_wait_semaphore(queue_type, std::move(sem), stages, false);
	}
}

static void riptide_device_signal_semaphore(Device *device, CommandBuffer::Type queue_type, const riptide_gpu_sync_operation *release)
{
	if (release && release->sync.semaphore != VK_NULL_HANDLE)
	{
		auto signal = device->request_semaphore(
			release->sync.value != 0 ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY, release->sync.semaphore);

		if (release->sync.value)
			signal = device->request_timeline_semaphore_as_binary(*signal, release->sync.value);

		if (signal)
			device->submit_empty(queue_type, nullptr, signal.get());
	}
}

static riptide_result
riptide_inter_encode(riptide_encoder encoder,
                      const riptide_gpu_sync_operation *acquire,
                      const riptide_gpu_sync_operation *release,
                      const ViewBuffers &views,
                      const riptide_rate_control *rate_control);

// Measuring (RT_HOSTTRACE): GPU times of an encode against the host's clock.
struct PendingGpuTrace
{
	QueryPoolHandle gpu_start, stripe0_end;
	int64_t host_submit_ns, host_seen_ns;
};
static std::vector<PendingGpuTrace> pending_gpu_traces;
static std::vector<double> gpu_trace_start, gpu_trace_seen;

static int64_t riptide_monotonic_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

static void riptide_gpu_trace_resolve(Device *device)
{
	VkCalibratedTimestampInfoKHR info[2] = {
		{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_DEVICE_KHR },
		{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR },
	};
	uint64_t ts[2], dev;
	if (!device->get_device_table().vkGetCalibratedTimestampsKHR ||
	    device->get_device_table().vkGetCalibratedTimestampsKHR(device->get_device(), 2, info, ts, &dev) != VK_SUCCESS)
		return;
	double period = device->get_gpu_properties().limits.timestampPeriod;
	auto to_host = [&](uint64_t ticks) { return double(ts[1]) + double(int64_t(ticks - ts[0])) * period; };
	for (auto it = pending_gpu_traces.begin(); it != pending_gpu_traces.end();)
	{
		if (it->gpu_start->is_signalled() && it->stripe0_end->is_signalled())
		{
			gpu_trace_start.push_back((to_host(it->gpu_start->get_timestamp_ticks()) - double(it->host_submit_ns)) * 1e-6);
			gpu_trace_seen.push_back((double(it->host_seen_ns) - to_host(it->stripe0_end->get_timestamp_ticks())) * 1e-6);
			it = pending_gpu_traces.erase(it);
		}
		else
			++it;
	}
	if (pending_gpu_traces.size() > 64)
		pending_gpu_traces.erase(pending_gpu_traces.begin());
	if (gpu_trace_start.size() >= 240)
	{
		std::sort(gpu_trace_start.begin(), gpu_trace_start.end());
		std::sort(gpu_trace_seen.begin(), gpu_trace_seen.end());
		fprintf(stderr, "PW submit->GPU start p50 %.3f p90 %.3f ms; GPU stripe 0 done->host p50 %.3f p90 %.3f ms\n",
		        gpu_trace_start[gpu_trace_start.size() / 2], gpu_trace_start[gpu_trace_start.size() * 9 / 10],
		        gpu_trace_seen[gpu_trace_seen.size() / 2], gpu_trace_seen[gpu_trace_seen.size() * 9 / 10]);
		gpu_trace_start.clear();
		gpu_trace_seen.clear();
	}
}

// Measuring (RT_HOSTTRACE): host times of points in an encode call, printed
// every 240 calls as the medians since the start (mark 0) of each.
static void riptide_host_mark(int i)
{
	static const bool on = getenv("RT_HOSTTRACE") != nullptr;
	if (!on)
		return;
	static int64_t t0;
	static std::vector<std::vector<double>> marks(16);
	static int calls;
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t t = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
	if (i == 0)
	{
		t0 = t;
		if (++calls % 240 == 0)
		{
			for (size_t k = 1; k < marks.size(); k++)
				if (!marks[k].empty())
				{
					std::sort(marks[k].begin(), marks[k].end());
					fprintf(stderr, "PW host mark %zu: %.3f ms\n", k, marks[k][marks[k].size() / 2]);
					marks[k].clear();
				}
		}
		return;
	}
	marks[i].push_back((t - t0) * 1e-6);
}

static riptide_result
riptide_encoder_encode_gpu_inner(riptide_encoder encoder,
                                  const riptide_gpu_sync_operation *acquire,
                                  const riptide_gpu_sync_operation *release,
                                  const ViewBuffers &views,
                                  const riptide_rate_control *rate_control)
{
	auto *device = encoder->device;

	if (encoder->rt_device->cmd && (acquire || release))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (encoder->inter.enabled)
		return riptide_inter_encode(encoder, acquire, release, views, rate_control);

	auto &context = encoder->contexts[encoder->context];

	Util::register_thread_index(0);

	Util::set_thread_logging_interface(&null_logger);

	BufferCreateInfo bufinfo = {};
	bufinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
	                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
	                VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	bufinfo.size = encoder->encoder.get_meta_required_size();
	bufinfo.domain = BufferDomain::CachedHost;
	if (!context.queued_meta || context.queued_meta->get_create_info().size < bufinfo.size)
		context.queued_meta = device->create_buffer(bufinfo);

	if (!context.queued_meta)
		return RIPTIDE_ERROR_OUT_OF_HOST_MEMORY;

	bufinfo.domain = BufferDomain::Device;
	if (!context.queued_meta_gpu || context.queued_meta_gpu->get_create_info().size < bufinfo.size)
		context.queued_meta_gpu = device->create_buffer(bufinfo);

	if (!context.queued_meta_gpu)
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	auto target_bitstream_size = rate_control->maximum_bitstream_size & ~VkDeviceSize(3u);

	// Check for bogus sizes.
	if (target_bitstream_size > UINT32_MAX || target_bitstream_size == 0)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	bufinfo.size = target_bitstream_size + encoder->encoder.get_meta_required_size();
	bufinfo.domain = BufferDomain::CachedHost;
	if (!context.queued_bitstream || context.queued_bitstream->get_create_info().size < bufinfo.size)
		context.queued_bitstream = device->create_buffer(bufinfo);

	if (!context.queued_bitstream)
		return RIPTIDE_ERROR_OUT_OF_HOST_MEMORY;

	bufinfo.domain = BufferDomain::Device;
	if (!context.queued_bitstream_gpu || context.queued_bitstream_gpu->get_create_info().size < bufinfo.size)
		context.queued_bitstream_gpu = device->create_buffer(bufinfo);

	if (!context.queued_bitstream_gpu)
		return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	Encoder::BitstreamBuffers bitstream_buffers = {};

	bitstream_buffers.meta.buffer = context.queued_meta_gpu.get();
	bitstream_buffers.meta.size = context.queued_meta_gpu->get_create_info().size;
	bitstream_buffers.bitstream.buffer = context.queued_bitstream_gpu.get();
	bitstream_buffers.bitstream.size = context.queued_bitstream_gpu->get_create_info().size;
	bitstream_buffers.target_size = target_bitstream_size;

	auto cmd =
			encoder->rt_device->cmd
				? device->request_borrowed_command_buffer(encoder->rt_device->cmd)
				: device->request_command_buffer(encoder->rt_device->queue_type);

	if (acquire)
	{
		for (size_t i = 0; i < acquire->num_images; i++)
		{
			cmd->acquire_image_barrier(*acquire->images[i].image->img,
			                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			                           acquire->images[i].queue_family_index);
		}
	}

	bool ret = encoder->encoder.encode(*cmd, views, bitstream_buffers);
	if (!ret)
	{
		device->submit_discard(cmd);
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	if (release)
	{
		for (size_t i = 0; i < release->num_images; i++)
		{
			cmd->release_image_barrier(*release->images[i].image->img,
									   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
									   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
									   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
									   release->images[i].queue_family_index);
		}
	}

	// NVIDIA really doesn't like it if we write bitstream to cached host.
	// Performance issue since these memory types are mapped coherent on the GPU.
	// A staging copy is just better. Could avoid it on iGPU, but iGPU isn't really supposed to be
	// used as the encoder when streaming.
	cmd->copy_buffer(*context.queued_meta, *context.queued_meta_gpu);
	cmd->copy_buffer(*context.queued_bitstream, *context.queued_bitstream_gpu);

	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				 VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);

	context.queued_fence.reset();

	if (encoder->rt_device->cmd)
	{
		device->submit_discard(cmd);

		// Technicality, image views we created have not been submitted yet to Vulkan.
		// Need to signal the GPU queues before we can move the context along.
		device->submit_external(encoder->rt_device->queue_type);
	}
	else if (context.upload_images[0])
	{
		context.upload_semaphore.reset();
		device->submit(cmd, &context.queued_fence, 1, &context.upload_semaphore);
	}
	else
	{
		device->submit(cmd, &context.queued_fence);
	}

	riptide_device_signal_semaphore(device, encoder->rt_device->queue_type, release);

	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_encode_gpu(riptide_encoder encoder,
                            const riptide_gpu_sync_operation *acquire,
                            const riptide_gpu_sync_operation *release,
                            const riptide_gpu_buffers *buffers,
                            const riptide_rate_control *rate_control)
{
	auto *device = encoder->device;
	WrappedViewBuffers views = {};
	if (!views.wrap(device, buffers, VK_IMAGE_USAGE_SAMPLED_BIT))
		return RIPTIDE_ERROR_OUT_OF_HOST_MEMORY;
	views.range_scale = encoder->ycbcr_scaling_factor;
	device->next_frame_context();
	riptide_device_wait_semaphore(device, encoder->rt_device->queue_type, acquire, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	return riptide_encoder_encode_gpu_inner(encoder, acquire, release, views, rate_control);
}

riptide_result
riptide_encoder_encode_gpu_scaled(riptide_encoder encoder,
                                   const riptide_gpu_sync_operation *acquire,
                                   const riptide_gpu_sync_operation *release,
                                   const riptide_scaled_encode_info *scaling_info,
                                   const riptide_rate_control *rate_control)
{
	Util::set_thread_logging_interface(&null_logger);

	if (scaling_info->intermediate_plane_format != VK_FORMAT_R8_UNORM &&
		scaling_info->intermediate_plane_format != VK_FORMAT_R16_UNORM)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (scaling_info->ycbcr_range != VK_SAMPLER_YCBCR_RANGE_ITU_FULL &&
		scaling_info->ycbcr_range != VK_SAMPLER_YCBCR_RANGE_ITU_NARROW)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	uint32_t ycbcr_range_bit_depth = scaling_info->ycbcr_range_bit_depth ? scaling_info->ycbcr_range_bit_depth : 8;
	if (scaling_info->ycbcr_range == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW &&
		(ycbcr_range_bit_depth < 8 || ycbcr_range_bit_depth > 16))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	uint32_t rotation = scaling_info->rotation;
	if (rotation != 0 && rotation != 1 && rotation != 3)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (rotation && (scaling_info->intermediate_plane_format != VK_FORMAT_R8_UNORM || !encoder->turn_program))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto *device = encoder->device;
	riptide_host_mark(0);
	device->next_frame_context();
	riptide_host_mark(1);
	encoder->host_start = device->write_calibrated_timestamp();

	if (!encoder->scaler_planes[0])
	{
		auto info = ImageCreateInfo::immutable_2d_image(
			encoder->width, encoder->height, scaling_info->intermediate_plane_format);
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

		encoder->scaler_planes[0] = encoder->device->create_image(info);

		if (encoder->chroma == ChromaSubsampling::Chroma420)
		{
			info.width /= 2;
			info.height /= 2;
		}

		encoder->scaler_planes[1] = encoder->device->create_image(info);
		encoder->scaler_planes[2] = encoder->device->create_image(info);
	}

	for (auto &plane : encoder->scaler_planes)
		if (!plane)
			return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;

	// Turned: the scaler writes the picture as it comes (the encoder's size
	// with its sides swapped), and a pass turns each plane into scaler_planes.
	if (rotation && !encoder->turn_planes[0])
	{
		auto info = ImageCreateInfo::immutable_2d_image(
			encoder->height, encoder->width, scaling_info->intermediate_plane_format);
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		encoder->turn_planes[0] = encoder->device->create_image(info);
		if (encoder->chroma == ChromaSubsampling::Chroma420)
		{
			info.width /= 2;
			info.height /= 2;
		}
		encoder->turn_planes[1] = encoder->device->create_image(info);
		encoder->turn_planes[2] = encoder->device->create_image(info);
		for (auto &plane : encoder->turn_planes)
			if (!plane)
				return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	ImageHandle *scaled = rotation ? encoder->turn_planes : encoder->scaler_planes;

	riptide_device_wait_semaphore(device, encoder->rt_device->queue_type, acquire, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	auto cmd =
			encoder->rt_device->cmd
				? device->request_borrowed_command_buffer(encoder->rt_device->cmd)
				: device->request_command_buffer(encoder->rt_device->queue_type);

	if (acquire)
	{
		for (size_t i = 0; i < acquire->num_images; i++)
		{
			cmd->acquire_image_barrier(*acquire->images[i].image->img,
									   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
									   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
									   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
									   acquire->images[i].queue_family_index);
		}
	}

	encoder->frame_start = cmd->write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
	if (encoder->host_start)
	{
		auto a = encoder->host_start, b = encoder->frame_start;
		device->register_time_interval("GPU", std::move(a), std::move(b), "host encode call to GPU start");
	}
	for (int i = 0; i < 3; i++)
	{
		cmd->image_barrier(*scaled[i], VK_IMAGE_LAYOUT_UNDEFINED,
						   VK_IMAGE_LAYOUT_GENERAL,
						   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
						   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	}

	VideoScaler::RescaleInfo info = {};
	info.input_color_space = scaling_info->input_color_space;
	info.output_color_space = scaling_info->output_color_space;
	info.crop_rect = scaling_info->crop_rect;
	info.skip_dither = scaling_info->skip_dither;
	info.force_linear_filtering = scaling_info->force_linear_filtering;
	encoder->scaler.set_ycbcr_chroma_midpoint(scaling_info->ycbcr_chroma_midpoint);
	encoder->scaler.set_ycbcr_range(scaling_info->ycbcr_range, ycbcr_range_bit_depth);

	if (scaling_info->view.view_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
	    scaling_info->view.view_format == VK_FORMAT_G16_B16R16_2PLANE_420_UNORM)
	{
		// Sam wanted this,
		WrappedViewBuffers wrapped;
		riptide_gpu_buffers buffers = {};

		for (auto &plane : buffers.planes)
			plane = scaling_info->view;

		// P010 is scaled through 16-bit plane views
		bool is_16bit = scaling_info->view.view_format == VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;
		buffers.planes[0].view_format = is_16bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
		buffers.planes[1].view_format = is_16bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;
		buffers.planes[2].view_format = is_16bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;

		buffers.planes[0].aspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
		buffers.planes[1].aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
		buffers.planes[2].aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
		buffers.planes[2].swizzle = VK_COMPONENT_SWIZZLE_G;

		if (!wrapped.wrap(device, &buffers, VK_IMAGE_USAGE_SAMPLED_BIT))
		{
			device->submit_discard(cmd);
			return RIPTIDE_ERROR_OUT_OF_HOST_MEMORY;
		}

		auto crop_rect = scaling_info->crop_rect ? *scaling_info->crop_rect : VkRect2D{};
		info.crop_rect = scaling_info->crop_rect ? &crop_rect : nullptr;

		if (crop_rect.offset.x % 2 || crop_rect.offset.y % 2 || crop_rect.extent.width % 2 || crop_rect.extent.height % 2)
		{
			device->submit_discard(cmd);
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}

		// Scale one plane at a time. Easier with how implementation works.
		info.num_output_planes = 1;
		for (int i = 0; i < 3; i++)
		{
			info.input = wrapped.planes[i];
			info.output_planes[0] = &scaled[i]->get_view();
			encoder->scaler.rescale(*cmd, info);

			if (i == 0)
			{
				crop_rect.offset.x >>= 1;
				crop_rect.offset.y >>= 1;
				crop_rect.extent.width >>= 1;
				crop_rect.extent.height >>= 1;
			}
		}
	}
	else if (format_ycbcr_num_planes(scaling_info->view.view_format) == 1)
	{
		ImageHandle wrapped_image;
		ImageViewHandle wrapped_view;
		if (!wrap_view(device, scaling_info->view, wrapped_image, wrapped_view, VK_IMAGE_USAGE_SAMPLED_BIT))
		{
			device->submit_discard(cmd);
			return RIPTIDE_ERROR_OUT_OF_HOST_MEMORY;
		}

		info.input = wrapped_view.get();
		for (int i = 0; i < 3; i++)
			info.output_planes[i] = &scaled[i]->get_view();
		info.num_output_planes = 3;
		encoder->scaler.rescale(*cmd, info);
	}
	else
	{
		// Could be supported if need be, but let's not unless we can prove we need it ...
		device->submit_discard(cmd);
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	if (rotation)
	{
		for (int i = 0; i < 3; i++)
		{
			cmd->image_barrier(*encoder->turn_planes[i],
			                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
			                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
			cmd->image_barrier(*encoder->scaler_planes[i], VK_IMAGE_LAYOUT_UNDEFINED,
			                   VK_IMAGE_LAYOUT_GENERAL,
			                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
			                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		}
		cmd->set_program(encoder->turn_program);
		for (int i = 0; i < 3; i++)
		{
			auto &out = *encoder->scaler_planes[i];
			int32_t push[3] = { int32_t(out.get_width()), int32_t(out.get_height()), int32_t(rotation) };
			cmd->set_texture(0, 0, encoder->turn_planes[i]->get_view(), StockSampler::NearestClamp);
			cmd->set_storage_texture(0, 1, out.get_view());
			cmd->push_constants(push, 0, sizeof(push));
			cmd->dispatch((out.get_width() + 7) / 8, (out.get_height() + 7) / 8, 1);
		}
	}

	for (int i = 0; i < 3; i++)
	{
		cmd->image_barrier(*encoder->scaler_planes[i],
						   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
						   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
						   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}

	if (encoder->rt_device->cmd)
	{
		device->submit_discard(cmd);
		device->submit_external(encoder->rt_device->queue_type);
	}
	else
	{
		device->submit(cmd);
	}

	ViewBuffers buffers = {};
	for (int i = 0; i < 3; i++)
		buffers.planes[i] = &encoder->scaler_planes[i]->get_view();
	buffers.range_scale = encoder->ycbcr_scaling_factor;
	riptide_host_mark(2);
	return riptide_encoder_encode_gpu_inner(encoder, nullptr, release, buffers, rate_control);
}

static bool riptide_cpu_buffer_format_subsampled(riptide_cpu_buffer_format format)
{
	switch (format)
	{
	case RIPTIDE_CPU_BUFFER_FORMAT_YUV420P10:
	case RIPTIDE_CPU_BUFFER_FORMAT_YUV420P16:
	case RIPTIDE_CPU_BUFFER_FORMAT_YUV420P:
	case RIPTIDE_CPU_BUFFER_FORMAT_NV12:
		return true;

	default:
		return false;
	}
}

static bool riptide_cpu_buffer_format_16bit(riptide_cpu_buffer_format format)
{
	return format > RIPTIDE_CPU_BUFFER_FORMAT_YUV444P;
}

riptide_result
riptide_encoder_set_frame_context(riptide_encoder encoder, int context)
{
	if (context >= RIPTIDE_MAX_FRAME_CONTEXTS)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	encoder->context = context;

	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_encode_cpu(riptide_encoder encoder, const riptide_cpu_buffer *buffers,
                            const riptide_rate_control *rate_control)
{
	Util::set_thread_logging_interface(&null_logger);
	int num_planes = buffers->format == RIPTIDE_CPU_BUFFER_FORMAT_NV12 ? 2 : 3;
	auto *device = encoder->device;

	auto &context = encoder->contexts[encoder->context];

	// Validate some assumptions.
	if (buffers->width != encoder->width || buffers->height != encoder->height)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (encoder->rt_device->cmd)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (encoder->chroma == ChromaSubsampling::Chroma420 && !riptide_cpu_buffer_format_subsampled(buffers->format))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (encoder->chroma == ChromaSubsampling::Chroma444 && riptide_cpu_buffer_format_subsampled(buffers->format))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (buffers->format == RIPTIDE_CPU_BUFFER_FORMAT_YUV420P10 ||
		buffers->format == RIPTIDE_CPU_BUFFER_FORMAT_YUV444P10)
	{
		riptide_encoder_set_ycbcr_scaling_factor(encoder, float(0xffff) / float(0x3ff));
	}
	else
	{
		riptide_encoder_set_ycbcr_scaling_factor(encoder, 1.0f);
	}

	Util::register_thread_index(0);

	for (int plane = 0; plane < num_planes; plane++)
	{
		int plane_width = encoder->width;
		int plane_height = encoder->height;

		if (plane != 0 && encoder->chroma == ChromaSubsampling::Chroma420)
		{
			plane_width /= 2;
			plane_height /= 2;
		}

		size_t plane_bpp = num_planes == 2 && plane == 1 ? 2 : 1;
		if (riptide_cpu_buffer_format_16bit(buffers->format))
			plane_bpp *= 2;

		if (buffers->row_stride_in_bytes[plane] < plane_width * plane_bpp)
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		if (buffers->row_stride_in_bytes[plane] * plane_height > buffers->plane_size_in_bytes[plane])
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	auto &images = context.upload_images;

	for (int plane = 0; plane < num_planes; plane++)
	{
		if (images[plane])
			continue;

		unsigned plane_bpp = num_planes == 2 && plane == 1 ? 2 : 1;
		if (riptide_cpu_buffer_format_16bit(buffers->format))
			plane_bpp *= 2;

		auto info = ImageCreateInfo::immutable_2d_image(
			buffers->width, buffers->height,
			plane_bpp == 2 && num_planes == 2
				? VK_FORMAT_R8G8_UNORM
				: (plane_bpp == 2 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM));

		info.misc = IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_TRANSFER_BIT |
		            IMAGE_MISC_CONCURRENT_QUEUE_GRAPHICS_BIT |
		            IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_COMPUTE_BIT;

		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

		if (plane != 0 && encoder->chroma == ChromaSubsampling::Chroma420)
		{
			info.width /= 2;
			info.height /= 2;
		}

		images[plane] = device->create_image(info);
		if (!images[plane])
			return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	if (context.upload_semaphore)
	{
		device->add_wait_semaphore(CommandBuffer::Type::AsyncTransfer,
			std::move(context.upload_semaphore),
			VK_PIPELINE_STAGE_2_COPY_BIT, true);
		context.upload_semaphore = {};
	}

	auto cmd = device->request_command_buffer(CommandBuffer::Type::AsyncTransfer);
	cmd->begin_barrier_batch();
	for (auto &img : images)
	{
		if (!img)
			continue;

		cmd->image_barrier(*img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, 0,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	}
	cmd->end_barrier_batch();

	riptide_gpu_buffers gpu_buffers = {};

	for (int plane = 0; plane < 3; plane++)
	{
		auto &p = gpu_buffers.planes[plane];
		p.width = images[plane] ? images[plane]->get_width() : images[1]->get_width();
		p.height = images[plane] ? images[plane]->get_height() : images[1]->get_height();

		p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		p.swizzle = num_planes == 2 && plane == 2 ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_R;
		p.image_format = images[plane] ? images[plane]->get_format() : VK_FORMAT_R8G8_UNORM;
		p.view_format = p.image_format;
		p.layout = images[plane]
			           ? images[plane]->get_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL)
			           : images[1]->get_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
		p.image = images[plane] ? images[plane]->get_image() : images[1]->get_image();

		unsigned plane_bpp = num_planes == 2 && plane == 1 ? 2 : 1;
		if (riptide_cpu_buffer_format_16bit(buffers->format))
			plane_bpp *= 2;

		if (plane < num_planes)
		{
			void *ptr = cmd->update_image(*images[plane], {}, { p.width, p.height, 1 },
			                              buffers->row_stride_in_bytes[plane] / plane_bpp, 0,
			                              { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
			memcpy(ptr, buffers->data[plane], buffers->row_stride_in_bytes[plane] * p.height);
		}
	}

	cmd->begin_barrier_batch();
	for (auto &img : images)
	{
		if (!img)
			continue;

		cmd->image_barrier(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		                   0, 0);
	}
	cmd->end_barrier_batch();

	Semaphore sem;
	device->submit(cmd, nullptr, 1, &sem);
	device->add_wait_semaphore(encoder->rt_device->queue_type, std::move(sem), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);

	auto ret = riptide_encoder_encode_gpu(encoder, nullptr, nullptr, &gpu_buffers, rate_control);
	return ret;
}

// Inter frames in stripes (see riptide_encoder_opaque::inter).

static ImageHandle riptide_inter_plane(Device *device, int width, int height, VkFormat format = VK_FORMAT_R8_UNORM)
{
	auto info = ImageCreateInfo::immutable_2d_image(width, height, format);
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.layout = ImageLayout::General;
	return device->create_image(info);
}

static void riptide_inter_plane_size(riptide_encoder encoder, int c, int height, int *w, int *h)
{
	bool sub = c != 0 && encoder->chroma == ChromaSubsampling::Chroma420;
	*w = sub ? encoder->width / 2 : encoder->width;
	*h = sub ? height / 2 : height;
}

// Rows of plane c (copy_rows.comp, through the stripe's encoder or decoder):
// from row src_y of src into dst from row dst_y, `rows` luma rows of the
// frame's width.
static void riptide_inter_copy_rows(riptide_encoder encoder, Encoder *enc, Decoder *dec, CommandBuffer &cmd,
                                     const ImageView &src, const ImageView &dst, int c, int src_y, int dst_y, int rows)
{
	int w, h;
	riptide_inter_plane_size(encoder, c, rows, &w, &h);
	bool sub = c != 0 && encoder->chroma == ChromaSubsampling::Chroma420;
	if (sub)
		src_y /= 2, dst_y /= 2;
	if (enc)
		enc->copy_rows(cmd, src, dst, w, h, src_y, dst_y);
	else
		dec->copy_rows(cmd, src, dst, w, h, src_y, dst_y);
}

// Decodes stripe k of frame f from what the decoder received of it, from the
// previous frame's decode and the stripe's vectors after it (the encoder's own
// decode, or a replay of a lossy one), into f's planes.
static bool riptide_inter_decode_stripe(riptide_encoder encoder, riptide_encoder_opaque::InterFrame &f, int k)
{
	auto &inter = encoder->inter;
	auto *device = encoder->device;
	auto &s = inter.stripes[k];
	auto &rec = f.stripes[k];
	if (!f.planes[0])
		for (int c = 0; c < 3; c++)
		{
			int w, h;
			riptide_inter_plane_size(encoder, c, encoder->height, &w, &h);
			if (!(f.planes[c] = riptide_inter_plane(device, w, h)))
				return false;
		}
	if (!s.decoded[0])
		for (int c = 0; c < 3; c++)
		{
			int w, h;
			riptide_inter_plane_size(encoder, c, s.height, &w, &h);
			if (!(s.decoded[c] = riptide_inter_plane(device, w, h)))
				return false;
		}
	auto &decoder = *s.decoder;
	ViewBuffers reference = {};
	if (f.predicted)
	{
		auto &prev = inter.ring[(f.number - 1) % inter.ring.size()];
		if (!prev.valid || prev.number != f.number - 1 || prev.stripes.size() != inter.stripes.size())
			return false;
		for (int i = 0; i < 3; i++)
			reference.planes[i] = &prev.planes[i]->get_view();
		decoder.set_motion_state(prev.stripes[k].mv_state.data(), prev.stripes[k].mv_state.size() / 2);
	}
	decoder.set_reference(f.predicted ? &reference : nullptr, s.y);
	decoder.clear();
	size_t first = 0;
	for (int j = 0; j < k; j++)
		first += f.stripes[j].packets.size();
	for (size_t i = 0; i < rec.packets.size(); i++)
		if (first + i >= f.received.size() || f.received[first + i])
			decoder.push_packet(rec.bytes.data() + rec.packets[i].offset, rec.packets[i].size);
	ViewBuffers out = {};
	for (int i = 0; i < 3; i++)
		out.planes[i] = &s.decoded[i]->get_view();
	auto cmd = device->request_command_buffer(encoder->rt_device->queue_type);
	for (auto &plane : s.decoded)
		cmd->image_barrier(*plane, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	if (!decoder.decode(*cmd, out))
	{
		device->submit_discard(cmd);
		return false;
	}
	cmd->barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	// The stripe into the frame: the next frame's reference.
	for (int c = 0; c < 3; c++)
		riptide_inter_copy_rows(encoder, nullptr, &decoder, *cmd, s.decoded[c]->get_view(), f.planes[c]->get_view(), c, 0, s.y, s.height);
	cmd->barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
	             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
	device->submit(cmd);
	int bw = (encoder->width + 15) / 16, bh = (s.height + 15) / 16;
	rec.mv_state.resize(2 * size_t(bw) * bh);
	decoder.get_motion_state(rec.mv_state.data(), rec.mv_state.size() / 2);
	return true;
}

static riptide_encoder_opaque::InterFrame &riptide_inter_last(riptide_encoder encoder)
{
	auto &inter = encoder->inter;
	return inter.ring[(inter.next_number - 1) % inter.ring.size()];
}

// The last encode's own decodes not done yet (riptide_encoder_decode_stripe):
// all of them, and the frame is then a reference.
static bool riptide_inter_finish(riptide_encoder encoder)
{
	auto &inter = encoder->inter;
	if (inter.next_number == 0)
		return true;
	auto &f = riptide_inter_last(encoder);
	bool pending = false;
	for (auto &s : inter.stripes)
		pending = pending || s.decode_pending;
	if (!pending)
		return true;
	Util::set_thread_logging_interface(&null_logger);
	// Stripes never handed out are handed out here (their packets kept, as sent).
	while (inter.packetized < int(inter.stripes.size()))
	{
		size_t n = 0;
		std::vector<riptide_packet> packets(65536);
		std::vector<uint8_t> bytes(inter.stripes[inter.packetized].budget + 65536);
		if (riptide_encoder_packetize_stripe(encoder, inter.packetized, 1100, packets.data(), packets.size(), &n,
		                                      nullptr, bytes.data(), bytes.size()) != RIPTIDE_SUCCESS)
			return false;
	}
	for (size_t k = 0; k < inter.stripes.size(); k++)
		if (inter.stripes[k].decode_pending)
		{
			inter.stripes[k].decode_pending = false;
			if (!riptide_inter_decode_stripe(encoder, f, int(k)))
				return false;
		}
	f.valid = true;
	return true;
}

// Each stripe's share of a frame's bytes. From the last frame's rate
// statistics: the cut the rate control would have made across the whole frame
// (the operations that add the least distortion per byte saved, in every
// stripe alike), so the stripes are coded at about the same quality as one
// image would be; every stripe keeps at least a quarter of its share by area,
// for content that changes. The first frame: by area.
static void riptide_inter_budgets(riptide_encoder encoder, size_t total)
{
	auto &inter = encoder->inter;
	auto &stripes = inter.stripes;
	const size_t S = stripes.size();
	const size_t header = 8; // each stripe's start of frame header
	double T = double(total / 4) - double(S * header / 4); // words
	std::vector<double> area(S), alloc(S);
	for (size_t k = 0; k < S; k++)
		area[k] = T * stripes[k].height / encoder->height;
	bool stats = true;
	for (auto &s : stripes)
		stats = stats && s.stats_valid;
	if (stats && S > 1)
	{
		std::vector<const uint32_t *> st(S);
		for (size_t k = 0; k < S; k++)
		{
			if (stripes[k].fence)
				stripes[k].fence->wait();
			st[k] = static_cast<const uint32_t *>(encoder->device->map_host_buffer(*stripes[k].stats, MEMORY_ACCESS_READ_BIT));
		}
		std::vector<double> cum(S, 0.0);
		double sum = 0.0;
		for (size_t k = 0; k < S; k++)
			sum += st[k][1];
		if (sum <= T)
		{
			// All of it fits: the rest by area.
			for (size_t k = 0; k < S; k++)
				alloc[k] = st[k][1] + (T - sum) * stripes[k].height / encoder->height;
		}
		else
		{
			size_t i = 0;
			for (; i < Encoder::RateStatBuckets; i++)
			{
				double step = 0.0;
				for (size_t k = 0; k < S; k++)
					step += st[k][16 + i];
				if (sum - step <= T)
				{
					double f = step > 0.0 ? (sum - T) / step : 0.0;
					for (size_t k = 0; k < S; k++)
						alloc[k] = st[k][1] - cum[k] - f * st[k][16 + i];
					break;
				}
				sum -= step;
				for (size_t k = 0; k < S; k++)
					cum[k] += st[k][16 + i];
			}
			if (i == Encoder::RateStatBuckets)
				for (size_t k = 0; k < S; k++)
					alloc[k] = st[k][1] - cum[k];
		}
		for (size_t k = 0; k < S; k++)
			encoder->device->unmap_host_buffer(*stripes[k].stats, MEMORY_ACCESS_READ_BIT);
		// The floor, taken from the stripes above it.
		double floor_sum = 0.0, above = 0.0;
		for (size_t k = 0; k < S; k++)
		{
			alloc[k] = std::max(alloc[k], 0.0);
			if (alloc[k] < area[k] * 0.25)
				floor_sum += area[k] * 0.25;
			else
				above += alloc[k];
		}
		double scale = above > 0.0 ? std::max(T - floor_sum, 0.0) / above : 0.0;
		for (size_t k = 0; k < S; k++)
			alloc[k] = alloc[k] < area[k] * 0.25 ? area[k] * 0.25 : alloc[k] * scale;
	}
	else
		alloc = area;
	size_t used = 0;
	for (size_t k = 0; k < S; k++)
	{
		size_t b = (size_t(std::max(alloc[k], 64.0)) * 4 + header) & ~size_t(3);
		if (k + 1 == S)
			b = std::max(total > used ? (total - used) & ~size_t(3) : 0, size_t(256) + header);
		stripes[k].budget = b;
		used += b;
	}
}

static riptide_result
riptide_inter_encode(riptide_encoder encoder,
                      const riptide_gpu_sync_operation *acquire,
                      const riptide_gpu_sync_operation *release,
                      const ViewBuffers &views,
                      const riptide_rate_control *rate_control)
{
	auto &inter = encoder->inter;
	auto *device = encoder->device;
	size_t total = rate_control->maximum_bitstream_size & ~size_t(3);
	if (total < inter.stripes.size() * 512)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (!riptide_inter_finish(encoder))
		inter.intra_requested = true;
	riptide_host_mark(5);

	uint32_t number = inter.next_number++;
	auto &prev = inter.ring[(number - 1) % inter.ring.size()];
	bool predict = !inter.intra_requested && number > 0 && prev.valid && prev.number == number - 1;
	inter.intra_requested = false;
	inter.predicted = predict;
	inter.packetized = 0;
	auto &f = inter.ring[number % inter.ring.size()];
	f.number = number;
	f.valid = false;
	f.predicted = predict;
	f.stripes.assign(inter.stripes.size(), {});
	f.received.clear();
	ViewBuffers reference = {};
	if (predict)
		for (int i = 0; i < 3; i++)
			reference.planes[i] = &prev.planes[i]->get_view();

	riptide_inter_budgets(encoder, total);
	riptide_host_mark(6);

	for (size_t k = 0; k < inter.stripes.size(); k++)
	{
		auto &s = inter.stripes[k];
		BufferCreateInfo bufinfo = {};
		bufinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		                VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufinfo.size = s.encoder->get_meta_required_size();
		if (!s.meta)
		{
			bufinfo.domain = BufferDomain::CachedHost;
			s.meta = device->create_buffer(bufinfo);
			bufinfo.domain = BufferDomain::Device;
			s.meta_gpu = device->create_buffer(bufinfo);
			bufinfo.domain = BufferDomain::CachedHost;
			bufinfo.size = Encoder::RateStatsSize;
			s.stats = device->create_buffer(bufinfo);
			if (!s.meta || !s.meta_gpu || !s.stats)
				return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
			s.encoder->set_rate_stats_readback(s.stats.get());
		}
		bufinfo.size = s.budget + s.encoder->get_meta_required_size();
		if (!s.bitstream || s.bitstream->get_create_info().size < bufinfo.size)
		{
			bufinfo.size += bufinfo.size / 2; // room to grow
			bufinfo.domain = BufferDomain::CachedHost;
			s.bitstream = device->create_buffer(bufinfo);
			bufinfo.domain = BufferDomain::Device;
			s.bitstream_gpu = device->create_buffer(bufinfo);
			if (!s.bitstream || !s.bitstream_gpu)
				return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
		}
		if (!s.source[0])
			for (int c = 0; c < 3; c++)
			{
				int w, h;
				riptide_inter_plane_size(encoder, c, s.height, &w, &h);
				if (!(s.source[c] = riptide_inter_plane(device, w, h, views.planes[0]->get_format())))
					return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
			}

		auto cmd = device->request_command_buffer(encoder->rt_device->queue_type);
		if (k == 0 && acquire)
			for (size_t i = 0; i < acquire->num_images; i++)
				cmd->acquire_image_barrier(*acquire->images[i].image->img,
				                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
				                           acquire->images[i].queue_family_index);
		auto t0 = cmd->write_timestamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
		// The stripe's rows of the frame, as an image of its own.
		for (auto &plane : s.source)
			cmd->image_barrier(*plane, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		for (int c = 0; c < 3; c++)
			riptide_inter_copy_rows(encoder, s.encoder.get(), nullptr, *cmd, *views.planes[c], s.source[c]->get_view(), c, s.y, 0, s.height);
		cmd->barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		if (k == 0 && release)
			for (size_t i = 0; i < release->num_images; i++)
				cmd->release_image_barrier(*release->images[i].image->img,
				                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				                           release->images[i].queue_family_index);

		ViewBuffers sv = {};
		for (int c = 0; c < 3; c++)
			sv.planes[c] = &s.source[c]->get_view();
		sv.range_scale = views.range_scale;
		Encoder::BitstreamBuffers bb = {};
		bb.meta.buffer = s.meta_gpu.get();
		bb.meta.size = s.meta_gpu->get_create_info().size;
		bb.bitstream.buffer = s.bitstream_gpu.get();
		bb.bitstream.size = s.bitstream_gpu->get_create_info().size;
		bb.target_size = s.budget;
		s.encoder->set_reference(predict ? &reference : nullptr, s.y);
		auto t1 = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		if (!s.encoder->encode(*cmd, sv, bb))
		{
			device->submit_discard(cmd);
			inter.intra_requested = true;
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		}
		auto t2 = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		cmd->copy_buffer(*s.meta, *s.meta_gpu);
		cmd->copy_buffer(*s.bitstream, 0, *s.bitstream_gpu, 0, s.budget + s.encoder->get_meta_required_size());
		cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
		auto t3 = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		{
			auto a = t1, b = t2;
			device->register_time_interval("GPU", std::move(t0), std::move(t1), "stripe cut");
			device->register_time_interval("GPU", std::move(a), std::move(b), "stripe encode (all)");
			device->register_time_interval("GPU", std::move(t2), std::move(t3), "stripe copy out");
		}
		if (encoder->frame_start && (k == 0 || k + 1 == inter.stripes.size()))
		{
			// Measuring: from the frame's first GPU command to this stripe's last.
			auto end = cmd->write_timestamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
			if (k == 0)
				encoder->stripe0_end = end;
			auto start = encoder->frame_start;
			device->register_time_interval("GPU", std::move(start), std::move(end),
			                               k == 0 ? "frame to stripe 0 done" : "frame to last stripe done");
		}
		s.fence.reset();
		if (k == 0)
			riptide_host_mark(3);
		int64_t submit_ns = riptide_monotonic_ns();
		device->submit(cmd, &s.fence);
		static const bool host_trace = getenv("RT_HOSTTRACE") != nullptr;
		if (host_trace && k == 0 && encoder->frame_start && encoder->stripe0_end)
			pending_gpu_traces.push_back({ encoder->frame_start, encoder->stripe0_end, submit_ns, 0 });
		if (k == 0)
			riptide_host_mark(4);
		s.stats_valid = true;
		s.decode_pending = true;
		if (k == 0)
			riptide_device_signal_semaphore(device, encoder->rt_device->queue_type, release);
	}
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_enable_inter(riptide_encoder encoder, const riptide_inter_info *info)
{
	Util::set_thread_logging_interface(&null_logger);
	auto &inter = encoder->inter;
	if (!info->enable)
	{
		inter.enabled = false;
		inter.stripes.clear();
		inter.ring.clear();
		return RIPTIDE_SUCCESS;
	}
	if (info->history < 1 || info->history > 64 || info->packet_boundary < 128)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	int rows = info->stripe_rows;
	int first = info->first_stripe_rows ? info->first_stripe_rows : rows;
	if (rows < 0 || (rows && (rows % 64 || rows < 128 || first % 64 || first < 128)))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (!inter.enabled || rows != inter.stripe_rows || first != inter.first_stripe_rows)
	{
		// The first stripe of `first` rows, then stripes of `rows`, the last
		// taking the rest (128 rows or more, less than 128 + rows).
		int count = 1;
		if (rows && encoder->height >= first + 128)
			count = 2 + (encoder->height - first - 128) / rows;
		inter.stripes.clear();
		inter.stripes.resize(size_t(count));
		for (int k = 0; k < count; k++)
		{
			auto &s = inter.stripes[k];
			s.y = k == 0 ? 0 : first + (k - 1) * rows;
			s.height = k + 1 == count ? encoder->height - s.y : k == 0 ? first : rows;
			s.encoder.reset(new Encoder);
			s.decoder.reset(new Decoder);
			if (!s.encoder->init(encoder->device, encoder->width, s.height, encoder->chroma) ||
			    !s.decoder->init(encoder->device, encoder->width, s.height, encoder->chroma, false))
			{
				inter.stripes.clear();
				inter.enabled = false;
				return RIPTIDE_ERROR_GENERIC;
			}
		}
		inter.stripe_rows = rows;
		inter.first_stripe_rows = first;
		inter.ring.clear();
		inter.ring.resize(size_t(info->history) + 1);
		inter.next_number = 0;
		inter.intra_requested = true;
	}
	for (auto &s : inter.stripes)
		s.encoder->set_inter_packet_boundary(info->packet_boundary);
	inter.enabled = true;
	return RIPTIDE_SUCCESS;
}

void riptide_encoder_request_intra(riptide_encoder encoder)
{
	encoder->inter.intra_requested = true;
}

riptide_result
riptide_encoder_get_frame_info(riptide_encoder encoder, uint32_t *frame_number, bool *predicted)
{
	auto &inter = encoder->inter;
	if (!inter.enabled || inter.next_number == 0)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	*frame_number = inter.next_number - 1;
	*predicted = inter.predicted;
	return RIPTIDE_SUCCESS;
}

int riptide_encoder_get_stripe_count(riptide_encoder encoder)
{
	return encoder->inter.enabled ? int(encoder->inter.stripes.size()) : 1;
}

riptide_result
riptide_encoder_get_stripe(riptide_encoder encoder, int stripe, int *y, int *height)
{
	auto &inter = encoder->inter;
	if (!inter.enabled || stripe < 0 || stripe >= int(inter.stripes.size()))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	*y = inter.stripes[stripe].y;
	*height = inter.stripes[stripe].height;
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_packetize_stripe(riptide_encoder encoder, int stripe, size_t packet_boundary,
                                  riptide_packet *packets, size_t capacity, size_t *out_packets,
                                  size_t *inter_packets, void *bitstream, size_t size)
{
	Util::set_thread_logging_interface(&null_logger);
	auto &inter = encoder->inter;
	*out_packets = 0;
	if (!inter.enabled || inter.next_number == 0 || stripe != inter.packetized ||
	    stripe >= int(inter.stripes.size()))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	auto &s = inter.stripes[stripe];
	auto &f = riptide_inter_last(encoder);
	auto &rec = f.stripes[stripe];
	s.fence->wait();
	static const bool host_trace = getenv("RT_HOSTTRACE") != nullptr;
	if (host_trace && stripe == 0 && !pending_gpu_traces.empty())
	{
		pending_gpu_traces.back().host_seen_ns = riptide_monotonic_ns();
		riptide_gpu_trace_resolve(encoder->device);
	}
	if (stripe == 0 && encoder->stripe0_end)
	{
		auto now = encoder->device->write_calibrated_timestamp();
		encoder->device->register_time_interval("GPU", std::move(encoder->stripe0_end), std::move(now),
		                                        "GPU stripe 0 done to host woken");
	}

	auto *device = encoder->device;
	auto *mapped_meta = device->map_host_buffer(*s.meta, MEMORY_ACCESS_READ_BIT);
	auto *mapped_bitstream = device->map_host_buffer(*s.bitstream, MEMORY_ACCESS_READ_BIT);
	s.encoder->build_inter_packets(s.inter_bytes, s.inter_sizes);
	size_t n_inter = s.inter_sizes.size();
	if (inter_packets)
		*inter_packets = n_inter;
	size_t n = s.encoder->compute_num_packets(mapped_meta, packet_boundary);
	size_t inter_size = s.inter_bytes.size();
	if (n_inter + n > capacity || inter_size > size)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	memcpy(bitstream, s.inter_bytes.data(), inter_size);
	size_t offset = 0;
	for (size_t sz : s.inter_sizes)
	{
		packets[(*out_packets)++] = {offset, sz};
		offset += sz;
	}
	n = s.encoder->packetize(reinterpret_cast<Encoder::Packet *>(packets + n_inter), packet_boundary,
	                         static_cast<uint8_t *>(bitstream) + inter_size, size - inter_size,
	                         mapped_meta, mapped_bitstream);
	for (size_t i = 0; i < n; i++)
		packets[n_inter + i].offset += inter_size;
	*out_packets = n_inter + n;
	size_t end = 0;
	for (size_t i = 0; i < *out_packets; i++)
		end = std::max(end, packets[i].offset + packets[i].size);
	// Kept as sent, for the own decode and replays.
	rec.bytes.assign(static_cast<const uint8_t *>(bitstream), static_cast<const uint8_t *>(bitstream) + end);
	rec.packets.assign(packets, packets + *out_packets);
	f.received.insert(f.received.end(), *out_packets, 1);
	inter.packetized++;
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_decode_stripe(riptide_encoder encoder, int stripe)
{
	Util::set_thread_logging_interface(&null_logger);
	auto &inter = encoder->inter;
	if (!inter.enabled || inter.next_number == 0 || stripe < 0 || stripe >= inter.packetized)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	auto &s = inter.stripes[stripe];
	if (!s.decode_pending)
		return RIPTIDE_SUCCESS;
	s.decode_pending = false;
	auto &f = riptide_inter_last(encoder);
	if (!riptide_inter_decode_stripe(encoder, f, stripe))
	{
		inter.intra_requested = true;
		return RIPTIDE_ERROR_GENERIC;
	}
	bool done = true;
	for (auto &t : inter.stripes)
		done = done && !t.decode_pending;
	if (done)
		f.valid = true;
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_read_last_decode(riptide_encoder encoder, const riptide_cpu_buffer *out)
{
	Util::set_thread_logging_interface(&null_logger);
	auto &inter = encoder->inter;
	if (!inter.enabled || inter.next_number == 0 || !riptide_inter_finish(encoder))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	auto &f = riptide_inter_last(encoder);
	if (!f.valid)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	auto *device = encoder->device;
	BufferCreateInfo info = {};
	info.domain = BufferDomain::CachedHost;
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	size_t offsets[3], total = 0;
	for (int i = 0; i < 3; i++)
	{
		offsets[i] = total;
		total += (size_t(f.planes[i]->get_width()) * f.planes[i]->get_height() + 3) & ~size_t(3);
	}
	info.size = total;
	auto buffer = device->create_buffer(info);
	auto cmd = device->request_command_buffer(encoder->rt_device->queue_type);
	for (int i = 0; i < 3; i++)
		cmd->copy_image_to_buffer(*buffer, *f.planes[i], offsets[i], {0, 0, 0},
		                          {f.planes[i]->get_width(), f.planes[i]->get_height(), 1}, 0, 0,
		                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
	Fence fence;
	device->submit(cmd, &fence);
	fence->wait();
	auto *mapped = static_cast<const uint8_t *>(device->map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
	for (int i = 0; i < 3; i++)
	{
		size_t w = f.planes[i]->get_width(), h = f.planes[i]->get_height();
		for (size_t y = 0; y < h; y++)
			memcpy(static_cast<uint8_t *>(out->data[i]) + y * out->row_stride_in_bytes[i], mapped + offsets[i] + y * w, w);
	}
	device->unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_report_loss(riptide_encoder encoder, uint32_t frame_number, const uint8_t *received, size_t count)
{
	Util::set_thread_logging_interface(&null_logger);
	auto &inter = encoder->inter;
	if (!inter.enabled)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (!riptide_inter_finish(encoder))
	{
		inter.intra_requested = true;
		return RIPTIDE_ERROR_GENERIC;
	}
	uint32_t last = inter.next_number - 1;
	auto &f = inter.ring[frame_number % inter.ring.size()];
	bool kept = inter.next_number > 0 && uint32_t(last - frame_number) < inter.ring.size() &&
	            f.valid && f.number == frame_number && count == f.received.size();
	if (kept && f.predicted)
	{
		auto &prev = inter.ring[(frame_number - 1) % inter.ring.size()];
		kept = prev.valid && prev.number == frame_number - 1;
	}
	if (!kept)
	{
		// Too old to replay: start over.
		inter.intra_requested = true;
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}
	memcpy(f.received.data(), received, count);
	// Replay that decoder from the frame on: what it has now is what the next frame
	// predicts from.
	for (uint32_t n = frame_number; n != last + 1; n++)
		for (size_t k = 0; k < inter.stripes.size(); k++)
			if (!riptide_inter_decode_stripe(encoder, inter.ring[n % inter.ring.size()], int(k)))
			{
				inter.intra_requested = true;
				return RIPTIDE_ERROR_GENERIC;
			}
	auto &newest = inter.ring[last % inter.ring.size()];
	for (size_t k = 0; k < inter.stripes.size(); k++)
		inter.stripes[k].encoder->set_motion_state(newest.stripes[k].mv_state.data(),
		                                           newest.stripes[k].mv_state.size() / 2);
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_compute_num_packets_with_padding(
		riptide_encoder encoder, size_t packet_boundary, size_t padding_size, size_t *num_packets)
{
	Util::set_thread_logging_interface(&null_logger);

	auto &inter = encoder->inter;
	if (inter.enabled)
	{
		// Every stripe's (the ones not handed out yet).
		*num_packets = 0;
		for (size_t k = size_t(inter.packetized); k < inter.stripes.size(); k++)
		{
			auto &s = inter.stripes[k];
			s.fence->wait();
			auto *meta = encoder->device->map_host_buffer(*s.meta, MEMORY_ACCESS_READ_BIT);
			s.encoder->build_inter_packets(s.inter_bytes, s.inter_sizes);
			*num_packets += s.inter_sizes.size() + s.encoder->compute_num_packets(meta, packet_boundary);
		}
		return RIPTIDE_SUCCESS;
	}

	auto &context = encoder->contexts[encoder->context];
	if (context.queued_fence)
		context.queued_fence->wait();

	if (!context.queued_meta)
		return RIPTIDE_ERROR_GENERIC;

	// This isn't really a "map". It just returns the persistently mapped pointer.
	auto *mapped_meta = encoder->device->map_host_buffer(*context.queued_meta, MEMORY_ACCESS_READ_BIT);
	*num_packets = encoder->encoder.compute_num_packets(mapped_meta, packet_boundary, padding_size);
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_compute_num_packets(riptide_encoder encoder, size_t packet_boundary, size_t *num_packets)
{
	return riptide_encoder_compute_num_packets_with_padding(encoder, packet_boundary, 0, num_packets);
}

riptide_result
riptide_encoder_compute_num_critical_packets(
		riptide_encoder encoder, int bands, size_t packet_boundary, size_t padding_size, size_t *num_packets)
{
	Util::set_thread_logging_interface(&null_logger);

	auto &context = encoder->contexts[encoder->context];

	// Internal assertion.
	if (bands < 0 || bands > 4)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (context.queued_fence)
		context.queued_fence->wait();

	if (!context.queued_meta)
		return RIPTIDE_ERROR_GENERIC;

	// This isn't really a "map". It just returns the persistently mapped pointer.
	auto *mapped_meta = encoder->device->map_host_buffer(*context.queued_meta, MEMORY_ACCESS_READ_BIT);
	*num_packets = encoder->encoder.compute_num_critical_packets(bands, mapped_meta, packet_boundary, padding_size);
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_packetize_with_padding(
		riptide_encoder encoder, riptide_packet *packets, size_t packet_boundary, size_t padding_size,
		size_t *out_packets, void *bitstream, size_t size)
{
	Util::set_thread_logging_interface(&null_logger);

	auto &inter = encoder->inter;
	if (inter.enabled)
	{
		// The stripes not handed out yet, in order, then the encoder's own decode.
		*out_packets = 0;
		size_t offset = 0;
		while (inter.packetized < int(inter.stripes.size()))
		{
			size_t n = 0;
			if (riptide_encoder_packetize_stripe(encoder, inter.packetized, packet_boundary,
			                                      packets + *out_packets, SIZE_MAX / sizeof(*packets) - *out_packets,
			                                      &n, nullptr, static_cast<uint8_t *>(bitstream) + offset,
			                                      size - offset) != RIPTIDE_SUCCESS)
				return RIPTIDE_ERROR_INVALID_ARGUMENT;
			size_t end = 0;
			for (size_t i = 0; i < n; i++)
			{
				packets[*out_packets + i].offset += offset;
				end = std::max(end, packets[*out_packets + i].offset + packets[*out_packets + i].size);
			}
			*out_packets += n;
			offset = end;
		}
		return riptide_inter_finish(encoder) ? RIPTIDE_SUCCESS : RIPTIDE_ERROR_GENERIC;
	}

	auto &context = encoder->contexts[encoder->context];

	if (context.queued_fence)
		context.queued_fence->wait();

	if (!context.queued_meta || !context.queued_bitstream)
		return RIPTIDE_ERROR_GENERIC;

	auto *mapped_meta = encoder->device->map_host_buffer(*context.queued_meta, MEMORY_ACCESS_READ_BIT);
	auto *mapped_bitstream = encoder->device->map_host_buffer(*context.queued_bitstream, MEMORY_ACCESS_READ_BIT);

	*out_packets = encoder->encoder.packetize(
		reinterpret_cast<Encoder::Packet *>(packets), packet_boundary,
		bitstream, size, mapped_meta, mapped_bitstream, padding_size);
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_get_mapped_raw_bitstream(
		riptide_encoder encoder, const void **mapped_bitstream, size_t *mapped_bitstream_size,
		const void **mapped_metadata, size_t *mapped_metadata_size)
{
	Util::set_thread_logging_interface(&null_logger);

	auto &context = encoder->contexts[encoder->context];

	if (context.queued_fence)
		context.queued_fence->wait();

	if (!context.queued_meta || !context.queued_bitstream)
		return RIPTIDE_ERROR_GENERIC;

	*mapped_bitstream = encoder->device->map_host_buffer(*context.queued_bitstream, MEMORY_ACCESS_READ_BIT);
	*mapped_metadata = encoder->device->map_host_buffer(*context.queued_meta, MEMORY_ACCESS_READ_BIT);
	*mapped_bitstream_size = context.queued_bitstream->get_create_info().size;
	*mapped_metadata_size = context.queued_meta->get_create_info().size;

	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_get_num_active_blocks(riptide_encoder encoder, int bands, size_t *num_active_blocks)
{
	Util::set_thread_logging_interface(&null_logger);
	if (bands < 0 || bands > 4)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	*num_active_blocks = encoder->encoder.get_num_active_blocks(bands);
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_compute_block_active_words(riptide_encoder encoder,
		int bands, uint32_t *words, size_t word_count)
{
	Util::set_thread_logging_interface(&null_logger);
	if (bands < 0 || bands > 4)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto &context = encoder->contexts[encoder->context];

	if (context.queued_fence)
		context.queued_fence->wait();

	if (!context.queued_meta)
		return RIPTIDE_ERROR_GENERIC;

	const void *mapped_metadata = encoder->device->map_host_buffer(*context.queued_meta, MEMORY_ACCESS_READ_BIT);
	encoder->encoder.compute_block_active_words(bands, words, word_count, mapped_metadata);
	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_encoder_packetize(riptide_encoder encoder, riptide_packet *packets, size_t packet_boundary,
						   size_t *out_packets, void *bitstream, size_t size)
{
	return riptide_encoder_packetize_with_padding(
			encoder, packets, packet_boundary, 0, out_packets, bitstream, size);
}

void riptide_encoder_set_ycbcr_scaling_factor(riptide_encoder encoder, float factor)
{
	encoder->ycbcr_scaling_factor = factor;
}

void riptide_encoder_destroy(riptide_encoder encoder)
{
	auto *device = encoder->device;
	Util::set_thread_logging_interface(&null_logger);
	delete encoder;
	device->next_frame_context();
}

struct riptide_decoder_opaque
{
	Device *device = nullptr;
	riptide_device rt_device = nullptr;
	Decoder decoder;
	bool fragment_path = false;
	ChromaSubsampling chroma = {};
	int width = 0;
	int height = 0;
	float ycbcr_scaling_factor = 1.0f;

	struct
	{
		BufferHandle readback_buffers[3];
		ImageHandle planes[3];
		Semaphore semaphore;
		Fence fence;
	} contexts[RIPTIDE_MAX_FRAME_CONTEXTS];
};

bool riptide_decoder_device_prefers_fragment_path(riptide_device device)
{
	Util::set_thread_logging_interface(&null_logger);
	return Decoder::device_prefers_fragment_path(device->device);
}

riptide_result
riptide_decoder_create(const riptide_decoder_create_info *info, riptide_decoder *decoder)
{
	Util::set_thread_logging_interface(&null_logger);
	if (!info->device)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (info->width <= 0 || info->height <= 0)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (info->chroma == RIPTIDE_CHROMA_SUBSAMPLING_420 && (info->width % 2 || info->height % 2))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto *dec = new riptide_decoder_opaque();
	dec->rt_device = info->device;
	dec->device = &info->device->device;
	dec->chroma = ChromaSubsampling(info->chroma);
	dec->fragment_path = info->fragment_path;
	dec->width = info->width;
	dec->height = info->height;

	if (!dec->decoder.init(dec->device, info->width, info->height, dec->chroma, info->fragment_path))
	{
		delete dec;
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	*decoder = dec;
	return RIPTIDE_SUCCESS;
}

void riptide_decoder_clear(riptide_decoder decoder)
{
	Util::set_thread_logging_interface(&null_logger);
	decoder->decoder.clear();
}

// A frame is potentially split into multiple packets.
riptide_result
riptide_decoder_push_packet(riptide_decoder decoder, const void *data, size_t size)
{
	Util::set_thread_logging_interface(&null_logger);
	bool ret = decoder->decoder.push_packet(data, size);
	return ret ? RIPTIDE_SUCCESS : RIPTIDE_ERROR_INVALID_ARGUMENT;
}

// For error correction purposes, it may be okay to decode a frame which dropped some packets.
bool riptide_decoder_decode_is_ready(riptide_decoder decoder, bool allow_partial_frame)
{
	Util::set_thread_logging_interface(&null_logger);
	return decoder->decoder.decode_is_ready(allow_partial_frame);
}

bool riptide_decoder_decode_is_ready_with_sideband(riptide_decoder decoder, bool allow_partial_frame,
		int num_pristine_bands, float minimum_packet_ratio,
		const uint32_t *active_block_mask, size_t word_count)
{
	Util::set_thread_logging_interface(&null_logger);
	return decoder->decoder.decode_is_ready(allow_partial_frame, num_pristine_bands, minimum_packet_ratio,
	                                        active_block_mask, word_count);
}

riptide_result
riptide_decoder_decode_gpu_buffer(riptide_decoder decoder,
                                   const riptide_gpu_sync_operation *acquire,
                                   const riptide_gpu_sync_operation *release,
                                   const riptide_gpu_buffers *buffers)
{
	if (decoder->rt_device->cmd && (acquire || release))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (decoder->fragment_path && decoder->ycbcr_scaling_factor != 1.0f)
		return RIPTIDE_ERROR_NOT_IMPLEMENTED;

	Util::register_thread_index(0);

	Util::set_thread_logging_interface(&null_logger);
	auto *device = decoder->device;
	device->next_frame_context();

	WrappedViewBuffers views = {};
	if (!views.wrap(device, buffers, decoder->fragment_path ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_STORAGE_BIT))
		return RIPTIDE_ERROR_OUT_OF_HOST_MEMORY;
	views.range_scale = decoder->ycbcr_scaling_factor;

	// Just use normal graphics queue here since the result will likely be consumed there.
	auto cmd = decoder->rt_device->cmd
		           ? device->request_borrowed_command_buffer(decoder->rt_device->cmd)
		           : device->request_command_buffer(decoder->rt_device->queue_type);

	VkPipelineStageFlags2 stages = decoder->fragment_path
		                               ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		                               : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

	VkAccessFlags2 access = decoder->fragment_path
		                        ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
		                        : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

	if (acquire)
	{
		for (size_t i = 0; i < acquire->num_images; i++)
		{
			if (acquire->images[i].queue_family_index != VK_QUEUE_FAMILY_IGNORED)
			{
				cmd->acquire_image_barrier(*acquire->images[i].image->img,
										   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
										   stages, access, acquire->images[i].queue_family_index);
			}
			else
			{
				cmd->image_barrier(*acquire->images[i].image->img,
								   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
								   stages, 0, stages, access);
			}
		}
	}

	auto ret = decoder->decoder.decode(*cmd, views);
	if (!ret)
	{
		device->submit_discard(cmd);
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	if (release)
	{
		for (size_t i = 0; i < release->num_images; i++)
		{
			cmd->release_image_barrier(*release->images[i].image->img,
									   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
									   stages, access, release->images[i].queue_family_index);
		}
	}

	riptide_device_wait_semaphore(device, decoder->rt_device->queue_type, acquire, stages);

	// This just queues up a command buffer, flush only happens when sync objects are signaled.
	if (decoder->rt_device->cmd)
	{
		device->submit_discard(cmd);

		// Technicality, image views we created have not been submitted yet to Vulkan.
		// Need to signal the GPU queues before we can move the context along.
		device->submit_external(decoder->rt_device->queue_type);
	}
	else
		device->submit(cmd);

	riptide_device_signal_semaphore(device, decoder->rt_device->queue_type, release);

	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_decoder_decode_cpu_buffer_async(riptide_decoder decoder, const riptide_cpu_buffer *buffers, int context_index)
{
	if (decoder->rt_device->cmd)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (context_index >= RIPTIDE_MAX_FRAME_CONTEXTS)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	Util::register_thread_index(0);

	Util::set_thread_logging_interface(&null_logger);
	auto *device = decoder->device;

	if (buffers->width != decoder->width || buffers->height != decoder->height)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (buffers->format == RIPTIDE_CPU_BUFFER_FORMAT_NV12)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (decoder->chroma == ChromaSubsampling::Chroma420 && !riptide_cpu_buffer_format_subsampled(buffers->format))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;
	if (decoder->chroma == ChromaSubsampling::Chroma444 && riptide_cpu_buffer_format_subsampled(buffers->format))
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	if (buffers->format == RIPTIDE_CPU_BUFFER_FORMAT_YUV420P10 ||
	    buffers->format == RIPTIDE_CPU_BUFFER_FORMAT_YUV444P10)
	{
		riptide_decoder_set_ycbcr_scaling_factor(decoder, float(0x3ff) / float(0xffff));
	}
	else
	{
		riptide_decoder_set_ycbcr_scaling_factor(decoder, 1.0f);
	}

	const size_t plane_bpp = riptide_cpu_buffer_format_16bit(buffers->format) ? 2 : 1;

	auto &context = decoder->contexts[context_index];

	for (int plane = 0; plane < 3; plane++)
	{
		int plane_width = decoder->width;
		int plane_height = decoder->height;

		if (plane != 0 && decoder->chroma == ChromaSubsampling::Chroma420)
		{
			plane_width /= 2;
			plane_height /= 2;
		}

		if (buffers->row_stride_in_bytes[plane] < plane_width * plane_bpp)
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
		if (buffers->row_stride_in_bytes[plane] * plane_height > buffers->plane_size_in_bytes[plane])
			return RIPTIDE_ERROR_INVALID_ARGUMENT;
	}

	for (int plane = 0; plane < 3; plane++)
	{
		auto &img = context.planes[plane];

		if (!img)
		{
			auto info = ImageCreateInfo::immutable_2d_image(buffers->width, buffers->height,
					plane_bpp == 2 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM);
			info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			if (decoder->fragment_path)
			{
				info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
				info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			}
			else
			{
				info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
				info.layout = ImageLayout::General;
				info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
			}

			info.misc |= IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_TRANSFER_BIT;
			if (decoder->rt_device->queue_type == CommandBuffer::Type::Generic)
				info.misc |= IMAGE_MISC_CONCURRENT_QUEUE_GRAPHICS_BIT;
			else
				info.misc |= IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_COMPUTE_BIT;

			if (plane != 0 && decoder->chroma == ChromaSubsampling::Chroma420)
			{
				info.width /= 2;
				info.height /= 2;
			}

			img = device->create_image(info);
			if (!img)
				return RIPTIDE_ERROR_OUT_OF_DEVICE_MEMORY;
		}
	}

	if (context.semaphore)
	{
		device->add_wait_semaphore(decoder->rt_device->queue_type, std::move(context.semaphore),
		                           decoder->fragment_path
			                           ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			                           : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, true);
		context.semaphore = {};
	}

	if (decoder->fragment_path)
	{
		auto cmd = device->request_command_buffer(decoder->rt_device->queue_type);
		cmd->begin_barrier_batch();

		for (auto &img : context.planes)
		{
			cmd->image_barrier(*img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
			                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
			                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,
			                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
		}

		cmd->end_barrier_batch();

		// This just queues up a command buffer, flush only happens when sync objects are signaled.
		device->submit(cmd);
	}

	riptide_gpu_buffers gpu_buffers = {};
	for (int plane = 0; plane < 3; plane++)
	{
		auto &p = gpu_buffers.planes[plane];
		p.image = context.planes[plane]->get_image();
		p.width = context.planes[plane]->get_width();
		p.height = context.planes[plane]->get_height();
		p.image_format = context.planes[plane]->get_format();
		p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		p.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
		p.view_format = p.image_format;
		p.layout = decoder->fragment_path ? VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
	}

	BufferCreateInfo bufinfo = {};

	auto res = riptide_decoder_decode_gpu_buffer(decoder, nullptr, nullptr, &gpu_buffers);
	if (res != RIPTIDE_SUCCESS)
		return res;

	auto cmd = device->request_command_buffer(decoder->rt_device->queue_type);

	if (decoder->fragment_path)
	{
		cmd->begin_barrier_batch();
		for (auto &img : context.planes)
		{
			cmd->image_barrier(*img, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		}
		cmd->end_barrier_batch();
	}
	else
	{
		cmd->barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	}

	Semaphore sem;
	device->submit(cmd, nullptr, 1, &sem);
	device->add_wait_semaphore(CommandBuffer::Type::AsyncTransfer, std::move(sem), VK_PIPELINE_STAGE_2_COPY_BIT, true);
	cmd = device->request_command_buffer(CommandBuffer::Type::AsyncTransfer);

	for (int plane = 0; plane < 3; plane++)
	{
		bufinfo.size = buffers->plane_size_in_bytes[plane];
		bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bufinfo.domain = BufferDomain::CachedHost;

		if (!context.readback_buffers[plane] || context.readback_buffers[plane]->get_create_info().size < bufinfo.size)
			context.readback_buffers[plane] = device->create_buffer(bufinfo);

		cmd->copy_image_to_buffer(*context.readback_buffers[plane], *context.planes[plane], 0, {},
		                          { context.planes[plane]->get_width(), context.planes[plane]->get_height(), 1 },
		                          buffers->row_stride_in_bytes[plane] / plane_bpp, 0,
		                          { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
	}

	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
	             VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);

	context.fence.reset();
	context.semaphore.reset();
	device->submit(cmd, &context.fence, 1, &context.semaphore);

	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_decoder_decode_cpu_buffer_complete(riptide_decoder decoder, const riptide_cpu_buffer *buffers, int context_index)
{
	if (context_index >= RIPTIDE_MAX_FRAME_CONTEXTS)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto &context = decoder->contexts[context_index];

	if (!context.fence)
		return RIPTIDE_ERROR_INVALID_ARGUMENT;

	auto *device = decoder->device;
	context.fence->wait();

	for (int plane = 0; plane < 3; plane++)
	{
		void *mapped = device->map_host_buffer(*context.readback_buffers[plane], MEMORY_ACCESS_READ_BIT);
		memcpy(buffers->data[plane], mapped, buffers->plane_size_in_bytes[plane]);
	}

	return RIPTIDE_SUCCESS;
}

riptide_result
riptide_decoder_decode_cpu_buffer_synchronous(riptide_decoder decoder, const riptide_cpu_buffer *buffers)
{
	auto result = riptide_decoder_decode_cpu_buffer_async(decoder, buffers, 0);
	if (result != RIPTIDE_SUCCESS)
		return result;
	return riptide_decoder_decode_cpu_buffer_complete(decoder, buffers, 0);
}

void riptide_decoder_set_ycbcr_scaling_factor(riptide_decoder decoder, float factor)
{
	decoder->ycbcr_scaling_factor = factor;
}

void riptide_decoder_destroy(riptide_decoder decoder)
{
	auto *device = decoder->device;
	Util::set_thread_logging_interface(&null_logger);
	delete decoder;
	device->next_frame_context();
}
}
