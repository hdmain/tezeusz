#include "ytplayer.hpp"
#include "platform.hpp"

namespace ytplayer {

void open(const std::string& videoId, void*) {
    if (videoId.empty()) return;
    platform::openUrl("https://www.youtube.com/watch?v=" + videoId);
}

bool isOpen() { return false; }
void close() {}
void tick() {}

} // namespace ytplayer
