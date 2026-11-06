// Riptide on Android: the host's own Vulkan decoder (riptide/), driven as the
// Apple client drives its Metal port (apple/Riptide.swift) and as the host
// replays a client's decode (riptide_c.cpp, riptide_inter_decode_stripe):
// every frame decoded in order with whatever arrived of it, stripe by stripe
// as each lands, each stripe into its rows of the frame; what never arrived
// is reported. A finished frame is converted to RGB into an AHardwareBuffer
// and handed to the Presenter (a SurfaceControl of our own) with a sync fence.
//
// Everything Vulkan happens on the network thread; the Presenter's release
// callbacks only hand back fences.

#include "riptide_client.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include <android/data_space.h>
#include <android/hardware_buffer.h>
#include <poll.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "context.hpp"
#include "device.hpp"
#include "riptide_decoder.hpp"

#include "gpuwarm.h"
#include "log.h"
#include "presenter.h"

using namespace Vulkan;

namespace gm {

// Bring-up switches: adb shell setprop debug.gmstream.<name> 1
static bool debug_prop(const char *name)
{
	char v[PROP_VALUE_MAX] = {};
	return __system_property_get(name, v) > 0 && v[0] == '1';
}

static const uint32_t yuv_vert[] = {
#include "shaders/yuv_vert.inc"
};
static const uint32_t yuv_frag[] = {
#include "shaders/yuv_frag.inc"
};
static const uint32_t yuv_r90_vert[] = {
#include "shaders/yuv_r90_vert.inc"
};
static const uint32_t yuv_r270_vert[] = {
#include "shaders/yuv_r270_vert.inc"
};
static const uint32_t debug_frag[] = {
#include "shaders/debug_frag.inc"
};

static constexpr int max_bridge = 6; // frames decoded from nothing to bridge a gap (the host keeps ~8)
static constexpr int ring_size = 4;  // frames' planes: the one decoding, its reference, slack
static constexpr int out_count = 4;  // RGB buffers out to SurfaceFlinger

struct RiptideClient::Impl {
	RiptideClient &owner;
	Presenter *presenter = nullptr;
	bool tried = false, ok = false;
	Context context;
	std::unique_ptr<Device> dev;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_props = nullptr;
	Program *program = nullptr, *program_r90 = nullptr, *program_r270 = nullptr;
	int rotation = 0, out_rotation = 0; // Surface.ROTATION_*: asked for; what the buffers were made for

	int width = 0, height = 0, first_rows = 0, stripe_rows = 0;
	struct Stripe {
		std::unique_ptr<Riptide::Decoder> dec;
		int y = 0, h = 0;
		ImageHandle planes[3]; // the stripe's own decode, then copied into the frame
	};
	std::vector<Stripe> stripes;
	ImageHandle ring[ring_size][3];
	int ring_index = 0;
	int current = -1, reference = -1;
	bool has_last = false;
	uint32_t last_decoded = 0;
	bool waiting_intra = false;
	std::map<uint32_t, std::pair<int, std::vector<std::pair<int, int>>>> pending; // reports not taken in

	struct InterStripe {
		int first = -1, count = 0;
		std::vector<bool> received;
		int received_count = 0;
	};
	struct InterFrame {
		uint32_t id = 0;
		bool predicted = false;
		std::vector<InterStripe> stripes;
		int next = 0, max_seen = -1;
		bool lost = false;
		bool has_timing = false;
		FrameTiming timing;
		uint64_t first_received = 0, first_encoded = 0, decode_start = 0;
		bool ready(int k) const
		{
			auto &s = stripes[size_t(k)];
			return (s.first >= 0 && (s.received_count == s.count || s.received[size_t(s.count - 1)])) || max_seen > k;
		}
	};
	bool in_frame = false;
	InterFrame frame;

	// RGB buffers out.
	struct Out {
		AHardwareBuffer *ahb = nullptr;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		ImageHandle wrapped;
		ImageViewHandle view;
		bool busy = false;
		int release_fence = -1;
	};
	Out outs[out_count];
	std::mutex out_lock;
	int out_w = 0, out_h = 0;
	bool skip = false;
	bool keep_awake = true;
	bool scanout = false, out_scanout = false; // asked for; what the buffers were made for
	GpuWarm warm;

	explicit Impl(RiptideClient &o) : owner(o) {}
	// The buffer is the picture turned to the panel; this undoes the turn.
	int32_t buffer_transform() const
	{
		return out_rotation == 1 ? ANATIVEWINDOW_TRANSFORM_ROTATE_270
		       : out_rotation == 3 ? ANATIVEWINDOW_TRANSFORM_ROTATE_90 : 0;
	}
	~Impl() { teardown(); }

	bool init();
	void teardown();
	bool ensure(const RiptidePacket &p);
	bool ensure_out(int w, int h);
	void free_out();
	void packet(const RiptidePacket &p, bool known, int64_t off);
	bool start(const RiptidePacket &p);
	void begin_frame(bool predicted);
	void decode_stripe(InterFrame &f, int k, bool last_of_frame);
	void end_frame(InterFrame *f, CommandBufferHandle &cmd);
	void finish(InterFrame &f);
	void done(InterFrame &f);
	void report(uint32_t id, int count, const std::vector<std::pair<int, int>> &ranges);
	void need_intra();
	static std::pair<int, std::vector<std::pair<int, int>>> missing(const InterFrame &f);
};

bool RiptideClient::Impl::init()
{
	if (tried)
		return ok;
	tried = true;
	if (!Context::init_loader(nullptr)) {
		LOGW("riptide: no Vulkan loader");
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
	if (!context.init_instance_and_device(nullptr, 0, dev_ext, 3, 0)) {
		LOGW("riptide: cannot create a Vulkan device");
		return false;
	}
	ContextOptions opts = {};
	opts.memory_priorities = false;
	opts.lean_memory_mode = true;
	dev = std::make_unique<Device>();
	dev->set_context(context, opts);
	get_ahb_props = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
		vkGetDeviceProcAddr(dev->get_device(), "vkGetAndroidHardwareBufferPropertiesANDROID"));
	ResourceLayout vl = {}, fl = {};
	fl.sets[0].sampled_image_mask = 7;
	fl.sets[0].fp_mask = 7;
	for (int i = 0; i < 3; i++)
		fl.sets[0].meta[i].array_size = 1;
	fl.output_mask = 1;
	if (debug_prop("debug.gmstream.planes"))
		program = dev->request_program(yuv_vert, sizeof(yuv_vert), debug_frag, sizeof(debug_frag), &vl, &fl);
	else
		program = dev->request_program(yuv_vert, sizeof(yuv_vert), yuv_frag, sizeof(yuv_frag), &vl, &fl);
	program_r90 = dev->request_program(yuv_r90_vert, sizeof(yuv_r90_vert), yuv_frag, sizeof(yuv_frag), &vl, &fl);
	program_r270 = dev->request_program(yuv_r270_vert, sizeof(yuv_r270_vert), yuv_frag, sizeof(yuv_frag), &vl, &fl);
	ok = get_ahb_props && program && program_r90 && program_r270;
	auto &props = dev->get_gpu_properties();
	LOGI("riptide: %s, Vulkan %u.%u%s", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
	     VK_VERSION_MINOR(props.apiVersion), ok ? "" : " (no AHardwareBuffer interop)");
	return ok;
}

void RiptideClient::Impl::free_out()
{
	if (!dev)
		return;
	dev->wait_idle();
	std::lock_guard<std::mutex> g(out_lock);
	for (auto &o : outs) {
		o.view.reset();
		o.wrapped.reset();
		if (o.image)
			vkDestroyImage(dev->get_device(), o.image, nullptr);
		if (o.memory)
			vkFreeMemory(dev->get_device(), o.memory, nullptr);
		if (o.ahb)
			AHardwareBuffer_release(o.ahb);
		if (o.release_fence >= 0)
			close(o.release_fence);
		o = Out{};
	}
	out_w = out_h = 0;
}

void RiptideClient::Impl::teardown()
{
	if (!dev)
		return;
	dev->wait_idle();
	stripes.clear();
	for (auto &r : ring)
		for (auto &p : r)
			p.reset();
	free_out();
	program = nullptr;
	dev.reset();
}

// An RGBA buffer SurfaceFlinger can scan out, as a Vulkan image we render into.
bool RiptideClient::Impl::ensure_out(int w, int h)
{
	if (w == out_w && h == out_h && scanout == out_scanout && rotation == out_rotation)
		return true;
	if (presenter)
		presenter->set_front(nullptr, 0, 0);
	free_out();
	VkDevice vk = dev->get_device();
	// Drawn in the panel's orientation: a quarter turn swaps the sides.
	bool quarter = rotation == 1 || rotation == 3;
	int bw = quarter ? h : w, bh = quarter ? w : h;
	for (auto &o : outs) {
		AHardwareBuffer_Desc d = {};
		d.width = uint32_t(bw);
		d.height = uint32_t(bh);
		d.layers = 1;
		d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
		d.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
		          AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | (scanout ? AHARDWAREBUFFER_USAGE_FRONT_BUFFER : 0);
		if (AHardwareBuffer_allocate(&d, &o.ahb) != 0)
			return false;
		VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
		VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fmt};
		if (get_ahb_props(vk, o.ahb, &props) != VK_SUCCESS)
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
		ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
		uint32_t bits = props.memoryTypeBits;
		mai.memoryTypeIndex = bits ? uint32_t(__builtin_ctz(bits)) : 0;
		if (vkAllocateMemory(vk, &mai, nullptr, &o.memory) != VK_SUCCESS ||
		    vkBindImageMemory(vk, o.image, o.memory, 0) != VK_SUCCESS)
			return false;
		auto info = ImageCreateInfo::render_target(uint32_t(bw), uint32_t(bh), VK_FORMAT_R8G8B8A8_UNORM);
		info.usage = ici.usage;
		info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		o.wrapped = dev->wrap_image(info, o.image);
		ImageViewCreateInfo vi = {};
		vi.image = o.wrapped.get();
		vi.format = VK_FORMAT_R8G8B8A8_UNORM;
		vi.view_type = VK_IMAGE_VIEW_TYPE_2D;
		o.view = dev->create_image_view(vi);
		if (!o.view)
			return false;
	}
	out_w = w;
	out_h = h;
	out_scanout = scanout;
	out_rotation = rotation;
	return true;
}

static ImageHandle make_plane(Device &dev, int w, int h)
{
	auto info = ImageCreateInfo::immutable_2d_image(uint32_t(w), uint32_t(h), VK_FORMAT_R8_UNORM);
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.layout = ImageLayout::General;
	return dev.create_image(info);
}

// The stripes' decoders and the frames' planes for frames of this layout.
bool RiptideClient::Impl::ensure(const RiptidePacket &p)
{
	int rows = p.stripes > 1 ? p.stripe_rows : 0, first = p.stripes > 1 ? p.first_rows : 0;
	if (p.width == width && p.height == height && int(stripes.size()) == p.stripes && rows == stripe_rows &&
	    first == first_rows)
		return true;
	dev->wait_idle();
	stripes.clear();
	bool resized = p.width != width || p.height != height;
	width = p.width;
	height = p.height;
	stripe_rows = rows;
	first_rows = first;
	reference = current = -1;
	has_last = false;
	in_frame = false;
	if (resized) {
		for (auto &r : ring)
			for (int c = 0; c < 3; c++)
				r[c] = make_plane(*dev, c ? width / 2 : width, c ? height / 2 : height);
		if (owner.on_size)
			owner.on_size(width, height);
	}
	for (int k = 0; k < p.stripes; k++) {
		Stripe s;
		s.y = k == 0 ? 0 : first + (k - 1) * rows;
		s.h = k + 1 == p.stripes ? height - s.y : k == 0 ? first : rows;
		if (s.h < 2 || s.h % 2) {
			stripes.clear();
			return false;
		}
		s.dec = std::make_unique<Riptide::Decoder>();
		if (!s.dec->init(dev.get(), width, s.h, Riptide::ChromaSubsampling::Chroma420, false)) {
			LOGE("riptide: decoder init failed (%dx%d)", width, s.h);
			stripes.clear();
			return false;
		}
		for (int c = 0; c < 3; c++)
			s.planes[c] = make_plane(*dev, c ? width / 2 : width, c ? s.h / 2 : s.h);
		stripes.push_back(std::move(s));
	}
	if (!ensure_out(width, height)) {
		LOGE("riptide: no output buffers");
		stripes.clear();
		return false;
	}
	LOGI("riptide: %dx%d in %d stripes", width, height, p.stripes);
	return true;
}

std::pair<int, std::vector<std::pair<int, int>>> RiptideClient::Impl::missing(const InterFrame &f)
{
	auto &last = f.stripes.back();
	int total = last.first >= 0 ? last.first + last.count : 0, known = 0;
	for (auto &s : f.stripes)
		if (s.first >= 0)
			known = std::max(known, s.first + s.count);
	std::vector<bool> got(size_t(std::max(total, known)), false);
	for (auto &s : f.stripes)
		if (s.first >= 0)
			for (int i = 0; i < s.count; i++)
				if (s.received[size_t(i)])
					got[size_t(s.first + i)] = true;
	std::vector<std::pair<int, int>> ranges;
	for (size_t i = 0; i < got.size();) {
		if (got[i]) {
			i++;
			continue;
		}
		size_t start = i;
		while (i < got.size() && !got[i])
			i++;
		ranges.emplace_back(int(start), int(i - start));
	}
	if (total == 0)
		ranges.emplace_back(int(got.size()), 0xffff); // the last stripe never came: the rest too
	return {total, ranges};
}

void RiptideClient::Impl::report(uint32_t id, int count, const std::vector<std::pair<int, int>> &ranges)
{
	pending[id] = {count, ranges};
	if (owner.on_loss)
		owner.on_loss(id, count, ranges);
}

void RiptideClient::Impl::need_intra()
{
	waiting_intra = true;
	in_frame = false;
	pending.clear();
	if (owner.on_need_intra)
		owner.on_need_intra();
}

void RiptideClient::Impl::begin_frame(bool predicted)
{
	current = ring_index;
	ring_index = (ring_index + 1) % ring_size;
	Riptide::ViewBuffers ref = {};
	if (predicted && reference >= 0)
		for (int c = 0; c < 3; c++)
			ref.planes[c] = &ring[reference][c]->get_view();
	for (auto &s : stripes) {
		s.dec->clear();
		s.dec->set_reference(predicted && reference >= 0 ? &ref : nullptr, s.y);
	}
}

// Stripe k from what arrived of it (nothing: from last frame's motion), into
// its rows of the frame; the last one also converts the frame for the screen.
// Bring-up: the first steps logged with debug.gmstream.trace=1.
static int g_trace = -1;
#define TRACE(...) do { if (g_trace < 0) g_trace = debug_prop("debug.gmstream.trace") ? 60 : 0; \
	if (g_trace > 0) { g_trace--; LOGI(__VA_ARGS__); } } while (0)

void RiptideClient::Impl::decode_stripe(InterFrame &f, int k, bool last_of_frame)
{
	TRACE("riptide: frame %u stripe %d decode (%d/%d packets)", f.id, k, f.stripes[size_t(k)].received_count, f.stripes[size_t(k)].count);
	auto &s = stripes[size_t(k)];
	if (k == 0)
		f.decode_start = now_ns();
	auto cmd = dev->request_command_buffer(CommandBuffer::Type::Generic);
	for (auto &plane : s.planes)
		cmd->image_barrier(*plane, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	Riptide::ViewBuffers out = {};
	for (int c = 0; c < 3; c++)
		out.planes[c] = &s.planes[c]->get_view();
	bool decoded = s.dec->decode(*cmd, out);
	TRACE("riptide: stripe %d recorded: %d", k, decoded);
	if (decoded) {
		cmd->barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		for (int c = 0; c < 3; c++) {
			int w = c ? width / 2 : width, h = c ? s.h / 2 : s.h, y = c ? s.y / 2 : s.y;
			s.dec->copy_rows(*cmd, s.planes[c]->get_view(), ring[current][c]->get_view(), w, h, 0, y);
		}
		cmd->barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}
	auto &st = f.stripes[size_t(k)];
	if (!decoded || st.first < 0 || st.received_count < st.count)
		f.lost = true;
	if (last_of_frame)
		end_frame(&f, cmd);
	else
		dev->submit(cmd);
	TRACE("riptide: stripe %d submitted", k);
}

// The frame is decoded: the next one's reference, and (with a timing) shown.
void RiptideClient::Impl::end_frame(InterFrame *f, CommandBufferHandle &cmd)
{
	reference = current;
	current = -1;
	if (!f || !f->has_timing || !presenter || !presenter->ready() || skip) {
		bool skipped = skip && f && f->has_timing;
		skip = false;
		dev->submit(cmd);
		dev->next_frame_context();
		if (skipped && owner.on_dropped)
			owner.on_dropped(f->timing);
		return;
	}
	TRACE("riptide: frame %u to the screen", f->id);
	if (out_w != width || out_h != height) {
		// Made with the layout (ensure): never here, with a command buffer open.
		dev->submit(cmd);
		dev->next_frame_context();
		return;
	}
	// A buffer the screen is done with (scanout mode: always the one on screen).
	int slot = -1, wait_fd = -1;
	if (scanout && out_scanout) {
		slot = 0;
		if (!presenter->front())
			presenter->set_front(outs[0].ahb, width, height, buffer_transform());
	} else {
		std::lock_guard<std::mutex> g(out_lock);
		for (int i = 0; i < out_count; i++)
			if (!outs[i].busy) {
				slot = i;
				wait_fd = outs[i].release_fence;
				outs[i].release_fence = -1;
				outs[i].busy = true;
				break;
			}
	}
	if (slot < 0) {
		dev->submit(cmd);
		dev->next_frame_context();
		if (owner.on_dropped)
			owner.on_dropped(f->timing);
		return;
	}
	TRACE("riptide: out slot %d", slot);
	if (wait_fd >= 0) {
		pollfd p{wait_fd, POLLIN, 0};
		poll(&p, 1, 20);
		close(wait_fd);
	}
	auto &o = outs[slot];
	RenderPassInfo rp = {};
	rp.num_color_attachments = 1;
	rp.color_attachments[0] = o.view.get();
	rp.store_attachments = 1;
	if (debug_prop("debug.gmstream.red")) {
		rp.clear_attachments = 1;
		rp.clear_color[0].float32[0] = 1.0f;
		rp.clear_color[0].float32[3] = 1.0f;
	}
	cmd->begin_render_pass(rp);
	cmd->set_program(out_rotation == 1 ? program_r270 : out_rotation == 3 ? program_r90 : program);
	cmd->set_opaque_state();
	cmd->set_depth_test(false, false);
	cmd->set_cull_mode(VK_CULL_MODE_NONE);
	cmd->set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	for (int c = 0; c < 3; c++)
		cmd->set_texture(0, unsigned(c), ring[reference][c]->get_view(), StockSampler::LinearClamp);
	if (!debug_prop("debug.gmstream.red"))
		cmd->draw(3);
	cmd->end_render_pass();
	// To SurfaceFlinger (the foreign queue).
	VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	b.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	b.srcQueueFamilyIndex = context.get_queue_info().family_indices[QUEUE_INDEX_GRAPHICS];
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	b.image = o.image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd->get_command_buffer(), &dep);
	dev->submit(cmd);
	// The acquire fence: a sync fd signalled when the frame is drawn.
	int fence = -1;
	auto sem = dev->request_semaphore_external(VK_SEMAPHORE_TYPE_BINARY, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
	if (sem) {
		dev->submit_empty(CommandBuffer::Type::Generic, nullptr, sem.get());
		auto h = sem->export_to_handle();
		if (h)
			fence = int(h.handle);
	}
	if (fence < 0) {
		// No exportable fence: wait here instead.
		dev->wait_idle();
	}
	TRACE("riptide: fence %d", fence);
	dev->next_frame_context();
	TRACE("riptide: next frame context");
	FrameTiming t = f->timing;
	t.decode_start = f->decode_start;
	if (scanout && out_scanout) {
		presenter->front_written(t, fence);
		return;
	}
	Impl *self = this;
	presenter->present(o.ahb, fence, width, height, t, [self, slot](int release) {
		std::lock_guard<std::mutex> g(self->out_lock);
		auto &x = self->outs[slot];
		if (x.release_fence >= 0)
			close(x.release_fence);
		x.release_fence = release;
		x.busy = false;
	}, ADATASPACE_SRGB, buffer_transform());
}

// A newer frame has started: the rest of this one as it is.
void RiptideClient::Impl::finish(InterFrame &f)
{
	f.max_seen = int(f.stripes.size());
	while (f.next < int(f.stripes.size())) {
		decode_stripe(f, f.next, f.next + 1 == int(f.stripes.size()));
		f.next++;
	}
	done(f);
}

void RiptideClient::Impl::done(InterFrame &f)
{
	last_decoded = f.id;
	has_last = true;
	if (f.lost) {
		if (owner.on_gap)
			owner.on_gap();
		auto m = missing(f);
		report(f.id, m.first, m.second);
	}
}

// The first packet of a frame: bridges frames that never arrived, sets up the
// decode. False to drop the frame (nothing to predict it from).
bool RiptideClient::Impl::start(const RiptidePacket &p)
{
	if (!ensure(p))
		return false;
	if (!p.predicted) {
		waiting_intra = false;
		pending.clear();
	} else if (waiting_intra) {
		return false;
	} else if (reference < 0 || !has_last) {
		need_intra();
		return false;
	} else {
		int gap = int(p.frame_id - last_decoded) - 1;
		if (gap > max_bridge) {
			need_intra();
			return false;
		}
		for (int k = 0; k < gap; k++) {
			uint32_t id = last_decoded + 1;
			InterFrame f;
			f.id = id;
			f.predicted = true;
			f.stripes.assign(stripes.size(), InterStripe{});
			begin_frame(true);
			for (int s = 0; s < int(f.stripes.size()); s++)
				decode_stripe(f, s, s + 1 == int(f.stripes.size()));
			last_decoded = id;
			report(id, 0, {{0, 0xffff}});
		}
	}
	begin_frame(p.predicted);
	frame = InterFrame{};
	frame.id = p.frame_id;
	frame.predicted = p.predicted;
	frame.stripes.assign(stripes.size(), InterStripe{});
	in_frame = true;
	return true;
}

void RiptideClient::Impl::packet(const RiptidePacket &p, bool known, int64_t off)
{
	if (!init())
		return;
	if (keep_awake)
		warm.touch();
	// Reports the host has taken in, or made moot by starting over; the rest
	// again, once a frame.
	for (auto it = pending.begin(); it != pending.end();)
		it = int32_t(it->first - p.repaired) > 0 ? std::next(it) : pending.erase(it);
	bool newer = !has_last || int32_t(p.frame_id - last_decoded) > 0;
	if ((!in_frame || frame.id != p.frame_id) && newer)
		for (auto &[id, r] : pending)
			if (owner.on_loss)
				owner.on_loss(id, r.first, r.second);
	if (in_frame && frame.id != p.frame_id) {
		if (int32_t(p.frame_id - frame.id) <= 0)
			return; // late: counted lost already
		in_frame = false;
		finish(frame);
	}
	if (!in_frame) {
		if (has_last && int32_t(p.frame_id - last_decoded) <= 0)
			return; // late
		if (!start(p))
			return;
	}
	auto &f = frame;
	if (p.stripe >= int(f.stripes.size()) || p.stripe_count <= 0 || p.index < p.stripe_first ||
	    p.index >= p.stripe_first + p.stripe_count)
		return;
	int k = p.stripe;
	auto &st = f.stripes[size_t(k)];
	if (st.first < 0) {
		st.first = p.stripe_first;
		st.count = p.stripe_count;
		st.received.assign(size_t(p.stripe_count), false);
	}
	int i = p.index - st.first;
	if (k < f.next || i >= st.count || st.received[size_t(i)])
		return; // late or a duplicate
	st.received[size_t(i)] = true;
	st.received_count++;
	f.max_seen = std::max(f.max_seen, k);
	stripes[size_t(k)].dec->push_packet(p.payload, p.size);
	if (!f.first_received) {
		f.first_received = p.received_ns;
		f.first_encoded = p.encoded_ns;
	}
	// The frame's timing, from this packet (its stripe's encode time).
	FrameTiming t;
	t.encode_ns = p.encoded_ns - p.slot_ns;
	t.present_host = p.slot_ns;
	if (known) {
		t.local = true;
		t.present_local = uint64_t(int64_t(p.slot_ns) - off);
		t.encoded_local = uint64_t(int64_t(p.encoded_ns) - off);
	}
	t.received = p.received_ns;
	t.first_received = f.first_received;
	t.overlap_ns = p.encoded_ns > f.first_encoded ? p.encoded_ns - f.first_encoded : 0;
	f.timing = t;
	f.has_timing = true;
	while (f.next < int(f.stripes.size()) && f.ready(f.next)) {
		decode_stripe(f, f.next, f.next + 1 == int(f.stripes.size()));
		f.next++;
	}
	if (f.next == int(f.stripes.size())) {
		in_frame = false;
		done(f);
	}
}

// ---- RiptideClient ----------------------------------------------------------------

RiptideClient::RiptideClient() : impl_(std::make_unique<Impl>(*this)) {}
RiptideClient::~RiptideClient() = default;
bool RiptideClient::available() { return true; } // the device is made with the first packet
void RiptideClient::set_presenter(Presenter *p) { impl_->presenter = p; }
void RiptideClient::set_window(ANativeWindow *) {}
void RiptideClient::packet(const RiptidePacket &p, bool known, int64_t off) { impl_->packet(p, known, off); }
void RiptideClient::reset()
{
	impl_->in_frame = false;
	impl_->has_last = false;
	impl_->width = impl_->height = 0; // set up again with the next packet (settings may have changed)
	impl_->reference = -1;
	impl_->waiting_intra = false;
	impl_->pending.clear();
}
void RiptideClient::skip_one() { impl_->skip = true; }
void RiptideClient::set_keep_gpu_awake(bool on) { impl_->keep_awake = on; }
void RiptideClient::set_scanout(bool on) { impl_->scanout = on; }
void RiptideClient::set_rotation(int r) { impl_->rotation = r & 3; }
std::string RiptideClient::summary()
{
	if (!impl_->ok)
		return impl_->tried ? "Riptide: no usable Vulkan device" : "Riptide (Vulkan)";
	return "Riptide (Vulkan) " + std::to_string(impl_->width) + "x" + std::to_string(impl_->height) + ", " +
	       std::to_string(impl_->stripes.size()) + " stripes";
}

} // namespace gm
