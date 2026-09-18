#pragma once
#include <string>

// Optional BitTorrent swarm for poster/backdrop images (TMDB CDN URLs).
// Primary fetch stays HTTP; this only seeds after cache and retries via peers if HTTP fails.
namespace imgswarm {

void init();
void shutdown();
// Signal in-flight tryFetch loops to exit quickly (before joining workers).
void abortFetches();

// Announce + seed a file already on disk (non-blocking).
void offer(const std::string& url, const std::string& filePath);

// Try to download from peers into destPath. Returns true if destPath is a usable image file.
bool tryFetch(const std::string& url, const std::string& destPath, int timeoutMs = 3000);

} // namespace imgswarm
