// Riptide left out of this build (GM_RIPTIDE unset): the client offers the
// hardware codecs only.
#include "riptide_client.h"

namespace gm {
struct RiptideClient::Impl {};
RiptideClient::RiptideClient() = default;
RiptideClient::~RiptideClient() = default;
bool RiptideClient::available() { return false; }
void RiptideClient::set_presenter(Presenter *) {}
void RiptideClient::set_window(ANativeWindow *) {}
void RiptideClient::packet(const RiptidePacket &, bool, int64_t) {}
void RiptideClient::reset() {}
void RiptideClient::skip_one() {}
void RiptideClient::set_keep_gpu_awake(bool) {}
std::string RiptideClient::summary() { return "Riptide not built in"; }
} // namespace gm
