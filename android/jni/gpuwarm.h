// See gpuwarm.cpp.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace gm {

class GpuWarm {
public:
	~GpuWarm();
	// A frame is coming: keep the GPU busy for the next second.
	void touch();

private:
	struct Vk;
	void run();
	std::mutex lock_;
	std::condition_variable cv_;
	std::thread thread_;
	std::atomic<bool> started_{false};
	std::atomic<uint64_t> last_touch_{0};
	bool quit_ = false;
};

} // namespace gm
