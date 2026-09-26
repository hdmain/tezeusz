#pragma once
#include <string>

// Resolve a YouTube video ID to a direct media URL (no browser / no iframe),
// then play it in the in-app libVLC player. Falls back to opening YouTube
// in the system browser only if every resolver fails.
namespace ytplayer {
void open(const std::string& videoId, const std::string& title = {});
bool isOpen();
void close();
void tick(); // apply pending play on the UI thread
}
