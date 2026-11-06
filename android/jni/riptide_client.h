// Riptide (codec 4) on this device: the decoder is the host's own Vulkan
// code (riptide/), stripe by stripe as they land, shown through a Vulkan
// swapchain on the stream's surface; losses go back to the host
// (apple/Riptide.swift is the Apple side of the same).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <android/native_window.h>

#include "conn.h"
#include "presenter.h"
#include "timing.h"

namespace gm {

class RiptideClient {
public:
	std::function<void(const FrameTiming &)> on_frame_done, on_dropped;
	std::function<void(int, int)> on_size;
	std::function<void(uint32_t, int, const std::vector<std::pair<int, int>> &)> on_loss;
	std::function<void()> on_need_intra, on_gap;

	RiptideClient();
	~RiptideClient();
	bool available();
	void set_presenter(Presenter *p);
	void set_window(ANativeWindow *w);
	// Network thread.
	void packet(const RiptidePacket &p, bool clock_known, int64_t offset);
	void reset();
	void skip_one();
	void set_keep_gpu_awake(bool on);
	// Scanout mode (Presenter::set_front): frames written into the buffer on screen.
	void set_scanout(bool on);
	// The display's rotation (Surface.ROTATION_*): output is drawn in the panel's own orientation.
	void set_rotation(int r);
	std::string summary();

	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
};

} // namespace gm
