// A build without Vulkan (GM_RIPTIDE unset): no scanout compositor; frames go
// through SurfaceFlinger.
#include "compositor.h"

namespace gm {
struct Compositor::Impl {};
Compositor::Compositor(Presenter &p) : presenter_(p) {}
Compositor::~Compositor() = default;
Gpu *Compositor::gpu() { return nullptr; }
bool Compositor::start(int, int, int) { return false; }
void Compositor::stop() {}
void Compositor::submit_planes(const VkImageView *, int, int, int fence, const FrameTiming &) { (void)fence; }
void Compositor::submit_image(AHardwareBuffer *, int, int, int, const FrameTiming &, std::function<void()> release)
{
	if (release)
		release();
}
void Compositor::forget_planes() {}
void Compositor::forget_images() {}
void Compositor::ring_presented(uint64_t, int, uint64_t) {}
std::string Compositor::summary() { return ""; }
} // namespace gm
