#pragma once
#include <string>

// Native in-app trailer player via libVLC (YouTube URL → HWND).
// If native playback fails, opens YouTube in the default browser.
namespace ytplayer {
void open(const std::string& videoId, void* parentHwnd = nullptr);
bool isOpen();
void close();
void tick();
}
