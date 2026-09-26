#pragma once
#include <string>

// Resolve a YouTube video ID to a direct media URL via InnerTube (HTTP JSON),
// then play it in the in-app libVLC player. No yt-dlp, no browser, no iframe.
// Falls back to opening YouTube in the system browser only if resolve fails.
namespace ytplayer {
void open(const std::string& videoId, const std::string& title = {});
bool isOpen();
bool isResolving(); // true from click until stream is ready / cancelled / failed-over
void openOnYoutube(); // open watch URL in system browser and cancel in-app resolve
void cancel();
void close();
void tick();      // apply pending play on the UI thread
void drawOverlay(); // loading card + "Go to YouTube" (Windows + Linux ImGui)
}
