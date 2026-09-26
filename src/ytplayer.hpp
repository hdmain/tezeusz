#pragma once
#include <string>

// Opens the trailer on YouTube in the default browser.
namespace ytplayer {
void open(const std::string& videoId, void* parentHwnd = nullptr);
bool isOpen();
void close();
void tick();
}
